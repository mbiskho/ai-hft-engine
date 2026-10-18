#pragma once
// Core value types. Prices and quantities are integers (ticks, lots) all the
// way through: no floating-point drift in the book, exact risk arithmetic.

#include <cstdint>
#include <type_traits>

namespace hft {

using Price = std::int64_t;  // ticks
using Qty = std::int64_t;    // lots
using Nanos = std::uint64_t;

enum class Side : std::uint8_t { Bid = 0, Ask = 1 };
enum class Aggressor : std::uint8_t { Buy = 0, Sell = 1 };

enum class EventType : std::uint8_t {
  BookLevel = 1,
  Trade = 2,
  SnapshotBegin = 3,
  SnapshotEnd = 4,
  KillSwitch = 5,
  Heartbeat = 6,
};

// Wire format: shared/event_format.md. One cache line, trivially copyable, so
// the SPSC ring moves it with a plain store and nothing allocates.
struct alignas(64) MarketEvent {
  EventType type;
  std::uint8_t side;  // Side for book levels, Aggressor for trades
  std::uint16_t symbol_id;
  std::uint32_t flags;  // bit 0: gap before this event
  Nanos exchange_ts_ns;
  Nanos recv_ts_ns;
  Price price;
  Qty qty;
  std::uint64_t seq;
  std::uint8_t reserved[16];
};
static_assert(sizeof(MarketEvent) == 64, "MarketEvent must match the 64-byte wire format");
static_assert(std::is_trivially_copyable_v<MarketEvent>);

constexpr std::uint32_t kFlagGap = 1u << 0;

enum class OrderSide : std::uint8_t { Buy, Sell };

struct OrderIntent {
  OrderSide side;
  Qty qty;
  Price limit;     // worst acceptable price (IOC)
  bool reducing;   // only reduces exposure; allowed while the kill switch is latched
  Nanos created_ns;
};

struct Fill {
  std::uint64_t order_id;
  OrderSide side;
  Qty qty;
  Price price;
  Nanos decision_ns;  // engine clock when the order was decided
  Nanos fill_ns;      // market time the simulated fill happened
};

}  // namespace hft
