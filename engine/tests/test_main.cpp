// Unit tests. Deliberately dependency-free (no gtest download in CI).
// Build with -fsanitize=thread to check the ring under TSan: `make tsan`.

#include <cmath>
#include <cstdio>
#include <fstream>
#include <functional>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "hft/engine.hpp"
#include "hft/spsc_ring.hpp"

using namespace hft;

static int g_failed = 0, g_checks = 0;
#define CHECK(cond)                                                          \
  do {                                                                       \
    ++g_checks;                                                              \
    if (!(cond)) {                                                           \
      ++g_failed;                                                            \
      std::fprintf(stderr, "  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
    }                                                                        \
  } while (0)
#define CHECK_NEAR(a, b, eps) CHECK(std::fabs((a) - (b)) <= (eps))

static std::vector<std::pair<const char*, std::function<void()>>>& registry() {
  static std::vector<std::pair<const char*, std::function<void()>>> r;
  return r;
}
#define TEST(name)                                                       \
  static void name();                                                    \
  static const bool name##_reg = (registry().emplace_back(#name, name), true); \
  static void name()

// ---------------------------------------------------------------- ring

TEST(ring_rejects_non_power_of_two) {
  bool threw = false;
  try { SpscRing<int> r(100); } catch (const std::invalid_argument&) { threw = true; }
  CHECK(threw);
}

TEST(ring_fifo_full_empty) {
  SpscRing<int> r(4);
  int v = 0;
  CHECK(!r.try_pop(v));
  for (int i = 0; i < 4; ++i) CHECK(r.try_push(i));
  CHECK(!r.try_push(99));  // full
  for (int i = 0; i < 4; ++i) {
    CHECK(r.try_pop(v));
    CHECK(v == i);
  }
  CHECK(!r.try_pop(v));
}

TEST(ring_two_threads_preserve_every_item_in_order) {
  constexpr std::uint64_t N = 2'000'000;
  SpscRing<std::uint64_t> r(1024);
  std::thread producer([&] {
    for (std::uint64_t i = 0; i < N; ++i) while (!r.try_push(i)) {}
  });
  std::uint64_t expect = 0, v = 0;
  bool ordered = true;
  while (expect < N) {
    if (r.try_pop(v)) {
      ordered &= (v == expect);
      ++expect;
    }
  }
  producer.join();
  CHECK(ordered);
  CHECK(expect == N);
}

// ---------------------------------------------------------------- book

TEST(book_orders_levels_best_first) {
  OrderBook b;
  b.set(Side::Bid, 100, 1);
  b.set(Side::Bid, 102, 2);
  b.set(Side::Bid, 101, 3);
  b.set(Side::Ask, 105, 1);
  b.set(Side::Ask, 103, 2);
  CHECK(b.best(Side::Bid).price == 102);
  CHECK(b.level(Side::Bid, 1).price == 101);
  CHECK(b.level(Side::Bid, 2).price == 100);
  CHECK(b.best(Side::Ask).price == 103);
  CHECK(b.depth(Side::Ask) == 2);
}

TEST(book_update_and_remove) {
  OrderBook b;
  b.set(Side::Bid, 100, 1);
  b.set(Side::Bid, 100, 7);
  CHECK(b.best(Side::Bid).qty == 7);
  CHECK(b.depth(Side::Bid) == 1);
  b.set(Side::Bid, 100, 0);
  CHECK(b.empty(Side::Bid));
  CHECK(b.set(Side::Bid, 55, 0));  // removing an absent level is a no-op
}

TEST(book_caps_depth_and_drops_worst) {
  OrderBook b;
  for (Price p = 1; p <= static_cast<Price>(OrderBook::kDepth) + 10; ++p) b.set(Side::Bid, p, 1);
  CHECK(b.depth(Side::Bid) == OrderBook::kDepth);
  CHECK(b.best(Side::Bid).price == static_cast<Price>(OrderBook::kDepth) + 10);
  CHECK(!b.set(Side::Bid, 0, 1));  // worse than everything tracked: dropped
}

TEST(book_crossed_invalidates_until_snapshot) {
  OrderBook b;
  b.set(Side::Bid, 100, 1);
  b.set(Side::Ask, 101, 1);
  b.set_valid(true);
  CHECK(b.valid());
  b.set(Side::Bid, 101, 1);  // bid == ask: we missed an update somewhere
  CHECK(!b.valid());
  CHECK(b.crossed_events() == 1);
}

// ---------------------------------------------------------------- features

static std::vector<long long> split_ll(const std::string& s) {
  std::vector<long long> out;
  std::stringstream ss(s);
  for (std::string x; std::getline(ss, x, ';');) out.push_back(std::stoll(x));
  return out;
}

TEST(features_match_shared_vectors) {
  std::ifstream in(HFT_SHARED_DIR "/feature_vectors.csv");
  CHECK(static_cast<bool>(in));
  int cases = 0;
  for (std::string line; std::getline(in, line);) {
    if (line.empty() || line[0] == '#') continue;
    std::vector<std::string> col;
    std::stringstream ss(line);
    for (std::string c; std::getline(ss, c, ',');) col.push_back(c);
    CHECK(col.size() == 10);
    OrderBook b;
    auto bp = split_ll(col[1]), bq = split_ll(col[2]), ap = split_ll(col[3]), aq = split_ll(col[4]);
    for (std::size_t i = 0; i < bp.size(); ++i) b.set(Side::Bid, bp[i], bq[i]);
    for (std::size_t i = 0; i < ap.size(); ++i) b.set(Side::Ask, ap[i], aq[i]);
    FeatureState fs;
    FeatureVec x{};
    fs.compute(b, x);
    CHECK_NEAR(x[0], std::stod(col[5]), 1e-4);
    CHECK_NEAR(x[1], std::stod(col[6]), 1e-6);
    CHECK_NEAR(x[2], std::stod(col[7]), 1e-6);
    CHECK_NEAR(x[3], std::stod(col[8]), 1e-4);
    CHECK_NEAR(x[7], std::stod(col[9]), 1e-6);
    CHECK(x[4] == 0.0f && x[5] == 0.0f && x[6] == 0.0f);  // no history
    ++cases;
  }
  CHECK(cases == 3);
}

TEST(features_history_returns_and_trade_flow) {
  OrderBook b;
  FeatureState fs;
  b.set(Side::Bid, 10000, 1);
  b.set(Side::Ask, 10002, 1);
  for (int i = 0; i < 10; ++i) fs.on_book(b);  // mid 10001 x10
  b.set(Side::Ask, 10004, 1);                    // mid 10002
  b.set(Side::Ask, 10002, 0);
  fs.on_book(b);
  fs.on_trade(Aggressor::Buy, 3);
  fs.on_trade(Aggressor::Sell, 1);
  FeatureVec x{};
  fs.compute(b, x);
  CHECK_NEAR(x[4], std::log(10002.0 / 10001.0) * 1e4, 1e-3);
  CHECK(x[5] == 0.0f);                // fewer than 51 mids
  CHECK_NEAR(x[6], 0.5f, 1e-6);       // (3 - 1) / 4
}

// ---------------------------------------------------------------- risk

static OrderBook valid_book(Price bid = 10000, Price ask = 10001) {
  OrderBook b;
  b.set(Side::Bid, bid, 100);
  b.set(Side::Ask, ask, 100);
  b.set_valid(true);
  return b;
}

TEST(risk_each_rejection_reason) {
  RiskLimits L;
  L.max_order_qty = 5;
  L.max_position = 10;
  L.price_collar_bps = 10;
  L.max_notional = 1e12;
  const OrderBook b = valid_book();
  const auto buy = [](Qty q, Price lim) { return OrderIntent{OrderSide::Buy, q, lim, false, 0}; };
  {
    RiskEngine r(L);
    CHECK(r.check(buy(1, 10002), b, 0, 1) == RiskReason::Ok);
    CHECK(r.check(buy(6, 10002), b, 0, 2) == RiskReason::OrderSize);
    CHECK(r.check(buy(5, 10002), b, 8, 3) == RiskReason::Position);
    CHECK(r.check(buy(1, 10100), b, 0, 4) == RiskReason::Collar);  // ~100 bps away
    OrderBook bad = b;
    bad.set_valid(false);
    CHECK(r.check(buy(1, 10002), bad, 0, 5) == RiskReason::BookInvalid);
  }
  {
    RiskLimits n = L;
    n.max_notional = 5 * 10000.5;
    RiskEngine r(n);
    CHECK(r.check(buy(5, 10002), b, 1, 1) == RiskReason::Notional);
  }
}

TEST(risk_rate_limit_uses_event_clock) {
  RiskLimits L;
  L.max_orders_per_sec = 2;
  RiskEngine r(L);
  const OrderBook b = valid_book();
  const OrderIntent o{OrderSide::Buy, 1, 10002, false, 0};
  CHECK(r.check(o, b, 0, 1'000'000'000) == RiskReason::Ok);
  CHECK(r.check(o, b, 1, 1'000'000'001) == RiskReason::Ok);
  CHECK(r.check(o, b, 2, 1'000'000'002) == RiskReason::RateLimit);
  CHECK(r.check(o, b, 2, 1'600'000'000) == RiskReason::Ok);  // 0.6 s later: refilled 1.2 tokens
}

TEST(risk_kill_switch_latches_and_allows_only_reducing) {
  RiskEngine r(RiskLimits{});
  const OrderBook b = valid_book();
  r.kill(RiskReason::KillSwitch);
  CHECK(r.killed());
  CHECK(r.check({OrderSide::Buy, 1, 10002, false, 0}, b, 5, 1) == RiskReason::KillSwitch);   // adds
  CHECK(r.check({OrderSide::Sell, 1, 9999, false, 0}, b, 5, 2) == RiskReason::KillSwitch);   // reduces but not marked
  CHECK(r.check({OrderSide::Sell, 5, 9999, true, 0}, b, 5, 3) == RiskReason::Ok);            // reducing flatten
  CHECK(r.check({OrderSide::Sell, 6, 9999, true, 0}, b, 5, 4) == RiskReason::KillSwitch);    // would flip short
}

TEST(risk_drawdown_trips_kill_switch) {
  RiskLimits L;
  L.max_drawdown = 100;
  RiskEngine r(L);
  r.on_pnl(50);
  r.on_pnl(-40);
  CHECK(!r.killed());
  r.on_pnl(-60);  // 110 below the 50 high-water mark
  CHECK(r.killed() && r.kill_reason() == RiskReason::Drawdown);
  r.on_pnl(1000);
  CHECK(r.killed());  // latched
}

// ---------------------------------------------------------------- latency

TEST(latency_percentiles_within_bucket_error) {
  LatencyHistogram h;
  for (std::uint64_t v = 1; v <= 100'000; ++v) h.record(v);
  const auto p50 = static_cast<double>(h.percentile(50));
  const auto p99 = static_cast<double>(h.percentile(99));
  CHECK(p50 >= 50'000 * 0.99 && p50 <= 50'000 * 1.07);
  CHECK(p99 >= 99'000 * 0.99 && p99 <= 99'000 * 1.07);
  CHECK(h.percentile(100) >= 100'000);
}

// ---------------------------------------------------------------- paper exchange + engine

struct FixedModel final : Model {
  float p;
  explicit FixedModel(float v) : p(v) {}
  float predict(const FeatureVec&) noexcept override { return p; }
  const char* name() const noexcept override { return "fixed"; }
};

static MarketEvent level(Side s, Price px, Qty q, Nanos t) {
  MarketEvent e{};
  e.type = EventType::BookLevel;
  e.side = static_cast<std::uint8_t>(s);
  e.price = px;
  e.qty = q;
  e.exchange_ts_ns = t;
  return e;
}
static MarketEvent marker(EventType t, Nanos ts) {
  MarketEvent e{};
  e.type = t;
  e.exchange_ts_ns = ts;
  return e;
}

TEST(paper_exchange_ioc_respects_limit_and_latency) {
  PaperExchange px(ExecConfig{1000, 0.0});
  OrderBook b = valid_book(10000, 10001);
  b.set(Side::Ask, 10003, 50);
  std::vector<Fill> fills;
  auto emit = [&](const Fill& f, double) { fills.push_back(f); };
  px.submit({OrderSide::Buy, 150, 10002, false, 0}, 0, 1);  // limit 10002: only the 10001 level qualifies
  px.on_market(b, 999, emit);
  CHECK(fills.empty());  // not arrived yet
  px.on_market(b, 1000, emit);
  CHECK(fills.size() == 1);
  CHECK(fills[0].price == 10001 && fills[0].qty == 100);  // rest cancelled, never fills at 10003
  CHECK(px.in_flight() == 0);
}

template <typename F>
static void feed_book(Engine& eng, std::vector<OutRecord>& out, Nanos& t, int steps, F&& mutate) {
  auto emit = [&](const OutRecord& r) { out.push_back(r); };
  for (int i = 0; i < steps; ++i) {
    t += 100'000'000;  // 100 ms
    for (const auto& e : mutate(t)) eng.on_event(e, steady_ns(), emit);
  }
}

TEST(engine_never_exceeds_position_limit) {
  EngineConfig cfg;
  cfg.risk.max_position = 5;
  cfg.risk.max_order_qty = 2;
  cfg.risk.max_notional = 1e12;
  cfg.strategy.order_qty = 2;
  cfg.strategy.cooldown_ns = 0;
  cfg.exec.latency_ns = 1;
  FixedModel always_buy(0.99f);
  Engine eng(cfg, always_buy);
  std::vector<OutRecord> out;
  auto emit = [&](const OutRecord& r) { out.push_back(r); };
  Nanos t = 1;
  eng.on_event(marker(EventType::SnapshotBegin, t), steady_ns(), emit);
  eng.on_event(level(Side::Bid, 10000, 1000, t), steady_ns(), emit);
  eng.on_event(level(Side::Ask, 10001, 1000, t), steady_ns(), emit);
  eng.on_event(marker(EventType::SnapshotEnd, t), steady_ns(), emit);
  feed_book(eng, out, t, 200, [](Nanos ts) {
    return std::vector<MarketEvent>{level(Side::Bid, 10000, 1000 + static_cast<Qty>(ts % 7), ts)};
  });
  CHECK(eng.max_abs_position() <= 5);
  CHECK(eng.position() == 4);  // 2 + 2; a third order of 2 would breach 5
  CHECK(eng.risk().count(RiskReason::Position) > 0);
}

TEST(engine_kill_switch_flattens_and_stops) {
  EngineConfig cfg;
  cfg.risk.max_notional = 1e12;
  cfg.strategy.cooldown_ns = 0;
  cfg.exec.latency_ns = 1;
  FixedModel always_buy(0.99f);
  Engine eng(cfg, always_buy);
  std::vector<OutRecord> out;
  auto emit = [&](const OutRecord& r) { out.push_back(r); };
  Nanos t = 1;
  eng.on_event(level(Side::Bid, 10000, 1000, t), steady_ns(), emit);
  eng.on_event(level(Side::Ask, 10001, 1000, t), steady_ns(), emit);
  eng.on_event(marker(EventType::SnapshotEnd, t), steady_ns(), emit);
  auto tick = [](Nanos ts) { return std::vector<MarketEvent>{level(Side::Bid, 10000, 1000 + static_cast<Qty>(ts % 5), ts)}; };
  feed_book(eng, out, t, 20, tick);
  CHECK(eng.position() > 0);
  eng.on_event(marker(EventType::KillSwitch, t), steady_ns(), emit);
  feed_book(eng, out, t, 50, tick);
  CHECK(eng.position() == 0);
  CHECK(eng.risk().killed());
  const auto orders_after_kill = eng.orders();
  feed_book(eng, out, t, 50, tick);
  CHECK(eng.orders() == orders_after_kill);  // flat and killed: no new orders
}

TEST(engine_gap_blocks_trading_until_snapshot) {
  EngineConfig cfg;
  cfg.strategy.cooldown_ns = 0;
  FixedModel always_buy(0.99f);
  Engine eng(cfg, always_buy);
  std::vector<OutRecord> out;
  auto emit = [&](const OutRecord& r) { out.push_back(r); };
  eng.on_event(level(Side::Bid, 10000, 10, 1), steady_ns(), emit);
  eng.on_event(level(Side::Ask, 10001, 10, 1), steady_ns(), emit);
  eng.on_event(marker(EventType::SnapshotEnd, 1), steady_ns(), emit);
  MarketEvent gap = level(Side::Bid, 10000, 11, 2);
  gap.flags = kFlagGap;
  eng.on_event(gap, steady_ns(), emit);
  const auto decisions = eng.decisions();
  eng.on_event(level(Side::Bid, 10000, 12, 3), steady_ns(), emit);
  CHECK(eng.decisions() == decisions);  // invalid book: no decision
  CHECK(eng.gaps() == 1);
}

int main() {
  for (auto& [name, fn] : registry()) {
    const int before = g_failed;
    fn();
    std::printf("%s %s\n", g_failed == before ? "ok  " : "FAIL", name);
  }
  std::printf("\n%d checks, %d failed\n", g_checks, g_failed);
  return g_failed == 0 ? 0 : 1;
}
