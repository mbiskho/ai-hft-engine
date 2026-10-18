#pragma once
// Pre-trade risk. Every order intent passes check() before it can reach the
// exchange; there is no other path. Checks are ordered cheapest-first and
// each rejection has a reason, counted separately, because "risk rejected
// 3%" is useless and "collar rejected 3% during the open" is actionable.
//
// The kill switch latches. Once tripped (drawdown limit, operator command, or
// a crossed book that doesn't recover), only exposure-reducing orders pass,
// until a human restarts the engine. Nothing un-trips it automatically.
//
// Owned by the engine thread. No locks, because nothing else touches it.

#include <array>
#include <cstdint>
#include <cstdlib>

#include "order_book.hpp"
#include "types.hpp"

namespace hft {

struct RiskLimits {
  Qty max_order_qty = 10;
  Qty max_position = 50;          // absolute, lots
  double max_notional = 5e9;      // ticks * lots, absolute position value
  double price_collar_bps = 20;   // limit must be within this of mid
  std::uint32_t max_orders_per_sec = 50;
  double max_drawdown = 2e6;      // ticks*lots from the PnL high-water mark
};

enum class RiskReason : std::uint8_t {
  Ok = 0,
  KillSwitch,
  BookInvalid,
  OrderSize,
  Position,
  Notional,
  Collar,
  RateLimit,
  Drawdown,
  Count
};

inline const char* to_string(RiskReason r) noexcept {
  switch (r) {
    case RiskReason::Ok: return "ok";
    case RiskReason::KillSwitch: return "kill_switch";
    case RiskReason::BookInvalid: return "book_invalid";
    case RiskReason::OrderSize: return "order_size";
    case RiskReason::Position: return "position";
    case RiskReason::Notional: return "notional";
    case RiskReason::Collar: return "collar";
    case RiskReason::RateLimit: return "rate_limit";
    case RiskReason::Drawdown: return "drawdown";
    default: return "?";
  }
}

class RiskEngine {
 public:
  explicit RiskEngine(RiskLimits l) : lim_(l), tokens_(static_cast<double>(l.max_orders_per_sec)) {}

  RiskReason check(const OrderIntent& o, const OrderBook& book, Qty position, Nanos now) noexcept {
    const RiskReason r = evaluate(o, book, position, now);
    ++counts_[static_cast<std::size_t>(r)];
    return r;
  }

  // Mark-to-market after every fill and book update; trips the kill switch on drawdown.
  void on_pnl(double pnl) noexcept {
    if (pnl > high_water_) high_water_ = pnl;
    if (high_water_ - pnl > lim_.max_drawdown && !killed_) {
      killed_ = true;
      kill_reason_ = RiskReason::Drawdown;
    }
  }

  void kill(RiskReason why) noexcept {
    if (!killed_) kill_reason_ = why;
    killed_ = true;
  }

  bool killed() const noexcept { return killed_; }
  RiskReason kill_reason() const noexcept { return kill_reason_; }
  std::uint64_t count(RiskReason r) const noexcept { return counts_[static_cast<std::size_t>(r)]; }
  const RiskLimits& limits() const noexcept { return lim_; }

 private:
  RiskReason evaluate(const OrderIntent& o, const OrderBook& book, Qty position, Nanos now) noexcept {
    const Qty signed_qty = o.side == OrderSide::Buy ? o.qty : -o.qty;
    const Qty after = position + signed_qty;
    const bool reduces = std::llabs(after) < std::llabs(position) && (after == 0 || (after > 0) == (position > 0));

    if (killed_ && !(o.reducing && reduces)) return RiskReason::KillSwitch;
    if (!book.valid()) return RiskReason::BookInvalid;
    if (o.qty <= 0 || o.qty > lim_.max_order_qty) return RiskReason::OrderSize;
    if (!reduces && std::llabs(after) > lim_.max_position) return RiskReason::Position;

    const double mid = book.mid();
    if (!reduces && std::abs(static_cast<double>(after)) * mid > lim_.max_notional) return RiskReason::Notional;
    if (std::abs(static_cast<double>(o.limit) - mid) / mid * 1e4 > lim_.price_collar_bps) return RiskReason::Collar;

    // Token bucket, refilled from the event clock so replays are deterministic.
    if (last_refill_ != 0) {
      tokens_ += static_cast<double>(now - last_refill_) * 1e-9 * lim_.max_orders_per_sec;
      if (tokens_ > lim_.max_orders_per_sec) tokens_ = lim_.max_orders_per_sec;
    }
    last_refill_ = now;
    if (tokens_ < 1.0) return RiskReason::RateLimit;
    tokens_ -= 1.0;
    return RiskReason::Ok;
  }

  RiskLimits lim_;
  double tokens_;
  Nanos last_refill_ = 0;
  double high_water_ = 0;
  bool killed_ = false;
  RiskReason kill_reason_ = RiskReason::Ok;
  std::array<std::uint64_t, static_cast<std::size_t>(RiskReason::Count)> counts_{};
};

}  // namespace hft
