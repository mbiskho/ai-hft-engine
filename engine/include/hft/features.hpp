#pragma once
// Feature vector for the signal model. The definitions must match
// research/hft_research/features.py exactly. shared/feature_vectors.csv is the
// cross-language test that keeps them in step.
//
//   0 spread_bps      (ask - bid) / mid * 1e4
//   1 imb1            (bq1 - aq1) / (bq1 + aq1)
//   2 imb5            same over the best 5 levels
//   3 micro_dev_bps   (microprice - mid) / mid * 1e4, microprice = (bid*aq1 + ask*bq1) / (bq1 + aq1)
//   4 ret10_bps       log(mid / mid 10 book events ago) * 1e4
//   5 ret50_bps       log(mid / mid 50 book events ago) * 1e4
//   6 trade_imb       (buy vol - sell vol) / total over the last 64 trades
//   7 depth_ratio     log(sum bid qty top 5 / sum ask qty top 5)
//
// History buffers are fixed rings; update() does no allocation.

#include <array>
#include <cmath>
#include <cstddef>

#include "order_book.hpp"

namespace hft {

inline constexpr std::size_t kNumFeatures = 8;
using FeatureVec = std::array<float, kNumFeatures>;

class FeatureState {
 public:
  void on_trade(Aggressor a, Qty qty) noexcept {
    const Qty signed_qty = a == Aggressor::Buy ? qty : -qty;
    total_ -= std::abs(trades_[t_head_]);
    flow_ -= trades_[t_head_];
    trades_[t_head_] = signed_qty;
    total_ += qty;
    flow_ += signed_qty;
    t_head_ = (t_head_ + 1) % kTrades;
  }

  // Call after each book update that leaves the book valid.
  void on_book(const OrderBook& b) noexcept {
    mids_[m_head_] = b.mid();
    m_head_ = (m_head_ + 1) % kMids;
    if (m_count_ < kMids) ++m_count_;
  }

  void compute(const OrderBook& b, FeatureVec& out) const noexcept {
    const double bid = static_cast<double>(b.best(Side::Bid).price);
    const double ask = static_cast<double>(b.best(Side::Ask).price);
    const double bq = static_cast<double>(b.best(Side::Bid).qty);
    const double aq = static_cast<double>(b.best(Side::Ask).qty);
    const double mid = 0.5 * (bid + ask);
    const double sb = static_cast<double>(b.depth_qty(Side::Bid, 5));
    const double sa = static_cast<double>(b.depth_qty(Side::Ask, 5));
    const double micro = (bid * aq + ask * bq) / (bq + aq);

    out[0] = static_cast<float>((ask - bid) / mid * 1e4);
    out[1] = static_cast<float>((bq - aq) / (bq + aq));
    out[2] = static_cast<float>((sb - sa) / (sb + sa));
    out[3] = static_cast<float>((micro - mid) / mid * 1e4);
    out[4] = static_cast<float>(ret_bps(mid, 10));
    out[5] = static_cast<float>(ret_bps(mid, 50));
    out[6] = total_ > 0 ? static_cast<float>(static_cast<double>(flow_) / static_cast<double>(total_)) : 0.0f;
    out[7] = static_cast<float>(std::log(sb / sa));
  }

 private:
  static constexpr std::size_t kMids = 64;    // >= 50 + 1
  static constexpr std::size_t kTrades = 64;

  double ret_bps(double mid, std::size_t back) const noexcept {
    if (m_count_ <= back) return 0.0;
    const std::size_t i = (m_head_ + kMids - 1 - back) % kMids;  // m_head_-1 is the latest
    return std::log(mid / mids_[i]) * 1e4;
  }

  std::array<double, kMids> mids_{};
  std::size_t m_head_ = 0, m_count_ = 0;
  std::array<Qty, kTrades> trades_{};
  std::size_t t_head_ = 0;
  Qty flow_ = 0, total_ = 0;
};

}  // namespace hft
