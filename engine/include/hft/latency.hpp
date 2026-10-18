#pragma once
// Fixed-size log-linear latency histogram (HdrHistogram-style, much smaller).
// 2^k buckets each split into 16 linear sub-buckets: ~6% worst-case relative
// error, from 1 ns to ~68 s, in 37 * 16 counters. record() is a few integer
// ops and one increment; it never allocates.

#include <array>
#include <bit>
#include <cstdint>

namespace hft {

class LatencyHistogram {
 public:
  void record(std::uint64_t ns) noexcept {
    ++counts_[index(ns)];
    ++total_;
    if (ns > max_) max_ = ns;
  }

  std::uint64_t percentile(double p) const noexcept {
    if (total_ == 0) return 0;
    const auto target = static_cast<std::uint64_t>(p / 100.0 * static_cast<double>(total_ - 1)) + 1;
    std::uint64_t seen = 0;
    for (std::size_t i = 0; i < counts_.size(); ++i) {
      seen += counts_[i];
      if (seen >= target) return upper(i);
    }
    return max_;
  }

  std::uint64_t count() const noexcept { return total_; }
  std::uint64_t max() const noexcept { return max_; }

 private:
  static constexpr int kSub = 4;  // 2^4 = 16 sub-buckets
  static constexpr std::size_t kBuckets = 37 * (1u << kSub);

  static std::size_t index(std::uint64_t v) noexcept {
    if (v < (1u << kSub)) return static_cast<std::size_t>(v);
    const int msb = 63 - std::countl_zero(v);
    const int shift = msb - kSub;
    const std::uint64_t sub = (v >> shift) & ((1u << kSub) - 1);
    const std::size_t i = static_cast<std::size_t>((shift + 1) * (1 << kSub) + static_cast<int>(sub));
    return i < kBuckets ? i : kBuckets - 1;
  }

  static std::uint64_t upper(std::size_t i) noexcept {
    if (i < (1u << kSub)) return i;
    const std::size_t shift = i / (1u << kSub) - 1;
    const std::uint64_t sub = i % (1u << kSub);
    return (((1ull << kSub) | sub) + 1) << shift;  // exclusive upper edge of the bucket
  }

  std::array<std::uint64_t, kBuckets> counts_{};
  std::uint64_t total_ = 0;
  std::uint64_t max_ = 0;
};

}  // namespace hft
