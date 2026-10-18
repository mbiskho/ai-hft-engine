#pragma once
// The engine thread's state machine: event -> book -> features -> model ->
// strategy -> risk -> paper exchange. Single-threaded by construction. The
// only objects that cross threads are the input and output rings owned by the
// runner (src/runner.hpp).

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>

#include "features.hpp"
#include "latency.hpp"
#include "model.hpp"
#include "order_book.hpp"
#include "paper_exchange.hpp"
#include "risk.hpp"

namespace hft {

struct StrategyConfig {
  float buy_threshold = 0.60f;
  float sell_threshold = 0.40f;
  Qty order_qty = 1;
  Nanos cooldown_ns = 50'000'000;  // min market time between decisions
  Price aggression_ticks = 1;      // IOC limit = touch +/- this many ticks
};

struct EngineConfig {
  StrategyConfig strategy;
  RiskLimits risk;
  ExecConfig exec;
};

// What the engine hands to the writer thread. Fixed size, trivially copyable.
struct alignas(64) OutRecord {
  enum Kind : std::uint8_t { FillRec = 1, Decision = 2, Reject = 3, KillRec = 4, End = 255 } kind;
  std::uint8_t reason;  // RiskReason for Reject / KillRec
  OrderSide side;
  float p_up;
  Qty qty;
  Price price;
  Qty position_after;
  Nanos market_ns;
  double pnl;  // marked to mid, net of fees
};

inline Nanos steady_ns() noexcept {
  return static_cast<Nanos>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                std::chrono::steady_clock::now().time_since_epoch())
                                .count());
}

class Engine {
 public:
  Engine(const EngineConfig& cfg, Model& model) : cfg_(cfg), model_(model), risk_(cfg.risk), px_(cfg.exec) {}

  // Process one event. `stamp_ns` is when the event entered the pipeline, on
  // the same clock as steady_ns(); `out` receives zero or more records.
  template <typename Out>
  void on_event(const MarketEvent& e, Nanos stamp_ns, Out&& out) noexcept {
    ++events_;
    market_ns_ = e.exchange_ts_ns;
    if (e.flags & kFlagGap) {  // the gateway saw a sequence gap: book is unreliable until resnapshot
      book_.set_valid(false);
      ++gaps_;
    }
    switch (e.type) {
      case EventType::SnapshotBegin: book_.clear(); return;
      case EventType::SnapshotEnd: book_.set_valid(true); return;
      case EventType::Trade: features_.on_trade(static_cast<Aggressor>(e.side), e.qty); return;
      case EventType::KillSwitch: on_kill(RiskReason::KillSwitch, out); return;
      case EventType::Heartbeat: return;
      case EventType::BookLevel: break;
    }

    book_.set(static_cast<Side>(e.side), e.price, e.qty);
    ++book_updates_;
    if (!book_.valid()) return;
    features_.on_book(book_);

    px_.on_market(book_, market_ns_, [&](const Fill& f, double fee) { apply_fill(f, fee, out); });
    mark_to_market();

    // Kill switch latched (operator, drawdown): stop trading, keep flattening
    // until flat. An IOC can partially fill, so retry once nothing is in flight.
    if (risk_.killed()) {
      if (position_ != 0 && px_.in_flight() == 0) flatten(out);
      return;
    }

    if (market_ns_ - last_decision_ns_ < cfg_.strategy.cooldown_ns) return;

    // ---- hot path: features -> model -> strategy -> risk ----
    const Nanos t0 = steady_ns();
    features_.compute(book_, x_);
    const float p = model_.predict(x_);
    ++decisions_;

    OrderIntent intent{};
    bool want = false;
    if (p >= cfg_.strategy.buy_threshold) {
      intent = {OrderSide::Buy, cfg_.strategy.order_qty, book_.best(Side::Ask).price + cfg_.strategy.aggression_ticks, false, t0};
      want = true;
    } else if (p <= cfg_.strategy.sell_threshold) {
      intent = {OrderSide::Sell, cfg_.strategy.order_qty, book_.best(Side::Bid).price - cfg_.strategy.aggression_ticks, false, t0};
      want = true;
    }
    if (want) {
      last_decision_ns_ = market_ns_;
      const RiskReason r = risk_.check(intent, book_, position_, market_ns_);
      if (r == RiskReason::Ok) {
        px_.submit(intent, market_ns_, ++next_order_id_);
        ++orders_;
        out(OutRecord{OutRecord::Decision, 0, intent.side, p, intent.qty, intent.limit, position_, market_ns_, pnl_});
      } else {
        out(OutRecord{OutRecord::Reject, static_cast<std::uint8_t>(r), intent.side, p, intent.qty, intent.limit, position_, market_ns_, pnl_});
      }
    }
    const Nanos t1 = steady_ns();
    compute_latency_.record(t1 - t0);          // features + model + risk
    signal_latency_.record(t1 - stamp_ns);     // pipeline entry -> decision (includes ring queueing)
  }

  // ---- read-only views for the runner's report ----
  const OrderBook& book() const noexcept { return book_; }
  const RiskEngine& risk() const noexcept { return risk_; }
  const LatencyHistogram& compute_latency() const noexcept { return compute_latency_; }
  const LatencyHistogram& signal_latency() const noexcept { return signal_latency_; }
  Qty position() const noexcept { return position_; }
  double pnl() const noexcept { return pnl_; }
  double max_drawdown() const noexcept { return max_dd_; }
  double fees() const noexcept { return fees_; }
  std::uint64_t events() const noexcept { return events_; }
  std::uint64_t book_updates() const noexcept { return book_updates_; }
  std::uint64_t decisions() const noexcept { return decisions_; }
  std::uint64_t orders() const noexcept { return orders_; }
  std::uint64_t fills() const noexcept { return fills_; }
  std::uint64_t gaps() const noexcept { return gaps_; }
  std::uint64_t max_abs_position() const noexcept { return max_abs_pos_; }

 private:
  template <typename Out>
  void apply_fill(const Fill& f, double fee, Out& out) noexcept {
    const Qty signed_qty = f.side == OrderSide::Buy ? f.qty : -f.qty;
    position_ += signed_qty;
    cash_ -= static_cast<double>(signed_qty) * static_cast<double>(f.price);
    cash_ -= fee;
    fees_ += fee;
    ++fills_;
    const auto ap = static_cast<std::uint64_t>(std::llabs(position_));
    if (ap > max_abs_pos_) max_abs_pos_ = ap;
    mark_to_market();
    out(OutRecord{OutRecord::FillRec, 0, f.side, 0.0f, f.qty, f.price, position_, f.fill_ns, pnl_});
  }

  void mark_to_market() noexcept {
    if (!book_.two_sided()) return;
    pnl_ = cash_ + static_cast<double>(position_) * book_.mid();
    if (pnl_ > peak_) peak_ = pnl_;
    if (peak_ - pnl_ > max_dd_) max_dd_ = peak_ - pnl_;
    risk_.on_pnl(pnl_);
  }

  template <typename Out>
  void on_kill(RiskReason why, Out& out) noexcept {
    risk_.kill(why);
    out(OutRecord{OutRecord::KillRec, static_cast<std::uint8_t>(why), OrderSide::Buy, 0, 0, 0, position_, market_ns_, pnl_});
    flatten(out);
  }

  // Send one reducing IOC for the whole position. Repeated on later events
  // until flat, since an IOC can partially fill.
  template <typename Out>
  void flatten(Out& out) noexcept {
    if (position_ == 0 || !book_.valid()) return;
    const bool sell = position_ > 0;
    const Qty q = std::min<Qty>(std::llabs(position_), cfg_.risk.max_order_qty);
    const Price lim = sell ? book_.best(Side::Bid).price - cfg_.strategy.aggression_ticks
                           : book_.best(Side::Ask).price + cfg_.strategy.aggression_ticks;
    const OrderIntent o{sell ? OrderSide::Sell : OrderSide::Buy, q, lim, true, steady_ns()};
    if (risk_.check(o, book_, position_, market_ns_) == RiskReason::Ok) {
      px_.submit(o, market_ns_, ++next_order_id_);
      ++orders_;
      out(OutRecord{OutRecord::Decision, 0, o.side, 0.0f, o.qty, o.limit, position_, market_ns_, pnl_});
    }
  }

  EngineConfig cfg_;
  Model& model_;
  OrderBook book_;
  FeatureState features_;
  RiskEngine risk_;
  PaperExchange px_;
  FeatureVec x_{};

  Qty position_ = 0;
  double cash_ = 0, pnl_ = 0, peak_ = 0, max_dd_ = 0, fees_ = 0;
  Nanos market_ns_ = 0, last_decision_ns_ = 0;
  std::uint64_t next_order_id_ = 0;
  std::uint64_t events_ = 0, book_updates_ = 0, decisions_ = 0, orders_ = 0, fills_ = 0, gaps_ = 0, max_abs_pos_ = 0;
  LatencyHistogram compute_latency_, signal_latency_;
};

}  // namespace hft
