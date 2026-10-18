// Microbenchmark of the per-event hot path, with no I/O and no threads:
// book update -> features -> model -> risk. Reports ns/op percentiles.
//
//   ./build/hft-bench [iterations]
//
// Pin it and quiet the machine first (docs/linux-tuning.md) or the tail
// percentiles measure the OS, not the code.

#include <cstdio>
#include <cstdlib>
#include <random>

#include "hft/engine.hpp"

using namespace hft;

int main(int argc, char** argv) {
  const long iters = argc > 1 ? std::atol(argv[1]) : 5'000'000;
  OrderBook book;
  FeatureState fs;
  LinearModel model({0.02f, 0.9f, 0.6f, 0.3f, 0.01f, 0.005f, 0.4f, 0.2f}, 0.0f);
  RiskLimits lim;
  lim.max_orders_per_sec = 1'000'000;
  RiskEngine risk(lim);
  for (int i = 0; i < 20; ++i) {
    book.set(Side::Bid, 100000 - i, 10 + i);
    book.set(Side::Ask, 100001 + i, 10 + i);
  }
  book.set_valid(true);

  std::mt19937_64 rng(42);
  std::uniform_int_distribution<int> lvl(0, 19), qty(0, 50), side(0, 1);
  LatencyHistogram update_h, decide_h;
  FeatureVec x{};
  volatile float sink = 0;

  for (long i = 0; i < iters; ++i) {
    const Side s = side(rng) ? Side::Bid : Side::Ask;
    const Price px = s == Side::Bid ? 100000 - lvl(rng) : 100001 + lvl(rng);
    const Qty q = qty(rng) + 1;  // never empty a level, so the book stays two-sided

    const Nanos t0 = steady_ns();
    book.set(s, px, q);
    fs.on_book(book);
    const Nanos t1 = steady_ns();
    fs.compute(book, x);
    const float p = model.predict(x);
    const OrderIntent o{p > 0.5f ? OrderSide::Buy : OrderSide::Sell, 1, book.best(Side::Ask).price, false, t1};
    const RiskReason r = risk.check(o, book, 0, static_cast<Nanos>(i) * 1000);
    const Nanos t2 = steady_ns();

    sink = sink + p + static_cast<float>(r);
    update_h.record(t1 - t0);
    decide_h.record(t2 - t1);
  }
  std::printf("{\"iterations\": %ld,\n", iters);
  std::printf(" \"book_update_ns\": {\"p50\": %llu, \"p99\": %llu, \"p999\": %llu},\n",
              (unsigned long long)update_h.percentile(50), (unsigned long long)update_h.percentile(99),
              (unsigned long long)update_h.percentile(99.9));
  std::printf(" \"features_model_risk_ns\": {\"p50\": %llu, \"p99\": %llu, \"p999\": %llu}}\n",
              (unsigned long long)decide_h.percentile(50), (unsigned long long)decide_h.percentile(99),
              (unsigned long long)decide_h.percentile(99.9));
  return sink == -1.0f;  // keep the optimiser honest
}
