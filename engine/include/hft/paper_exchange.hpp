#pragma once
// Paper exchange: simulated IOC execution against the live (or replayed) book.
//
// An order "arrives" at the exchange `latency_ns` of market time after it was
// decided, and fills against whatever the book looks like *then*, walking
// levels up to the limit price; the rest is cancelled. Filling against the
// book at decision time would hide adverse selection: the touch you saw is
// often gone by the time you arrive. Pending orders sit in a fixed ring.

#include <array>
#include <cstddef>
#include <cstdint>

#include "order_book.hpp"
#include "types.hpp"

namespace hft {

struct ExecConfig {
  Nanos latency_ns = 200'000;  // decision -> exchange, market time
  double fee_bps = 1.0;        // taker fee on notional
};

class PaperExchange {
 public:
  explicit PaperExchange(ExecConfig c) : cfg_(c) {}

  // Returns false if too many orders are in flight (risk's rate limit should
  // make this unreachable; it's a backstop, not a queue).
  bool submit(const OrderIntent& o, Nanos market_now, std::uint64_t id) noexcept {
    if (count_ == kMaxPending) return false;
    pending_[(head_ + count_) % kMaxPending] = Pending{o, market_now + cfg_.latency_ns, id};
    ++count_;
    return true;
  }

  // Fill every pending order whose arrival time has come. `emit(fill, fee)`
  // is called per fill.
  template <typename Emit>
  void on_market(const OrderBook& book, Nanos market_now, Emit&& emit) noexcept {
    while (count_ > 0 && pending_[head_].arrive_ns <= market_now) {
      const Pending p = pending_[head_];
      head_ = (head_ + 1) % kMaxPending;
      --count_;
      if (!book.two_sided()) continue;  // nothing to hit: IOC cancels
      execute(book, p, market_now, emit);
    }
  }

  std::size_t in_flight() const noexcept { return count_; }

 private:
  struct Pending {
    OrderIntent order;
    Nanos arrive_ns;
    std::uint64_t id;
  };
  static constexpr std::size_t kMaxPending = 64;

  template <typename Emit>
  void execute(const OrderBook& book, const Pending& p, Nanos now, Emit& emit) noexcept {
    const bool buy = p.order.side == OrderSide::Buy;
    const Side opposite = buy ? Side::Ask : Side::Bid;
    Qty left = p.order.qty;
    for (std::size_t i = 0; i < book.depth(opposite) && left > 0; ++i) {
      const Level& lv = book.level(opposite, i);
      if (buy ? lv.price > p.order.limit : lv.price < p.order.limit) break;
      const Qty q = left < lv.qty ? left : lv.qty;
      left -= q;
      const double fee = static_cast<double>(q) * static_cast<double>(lv.price) * cfg_.fee_bps * 1e-4;
      emit(Fill{p.id, p.order.side, q, lv.price, p.order.created_ns, now}, fee);
    }
  }

  ExecConfig cfg_;
  std::array<Pending, kMaxPending> pending_{};
  std::size_t head_ = 0, count_ = 0;
};

}  // namespace hft
