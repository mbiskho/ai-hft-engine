#pragma once
// L2 order book over fixed arrays.
//
// Each side is a sorted array of up to kDepth levels, best first (bids
// descending, asks ascending). Updates are a binary search plus a memmove of a
// few dozen 16-byte entries, all in two or three cache lines, which beats a
// node-based map by a wide margin at this depth. Nothing allocates after
// construction. Levels beyond kDepth are dropped: a strategy that trades the
// touch never looks that far, and the exchange snapshot resets it anyway.
//
// A crossed book (best bid >= best ask) means we missed an update. The book
// marks itself invalid; risk refuses to trade until the next snapshot.

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstring>

#include "types.hpp"

namespace hft {

struct Level {
  Price price;
  Qty qty;
};

class OrderBook {
 public:
  static constexpr std::size_t kDepth = 64;

  void clear() noexcept {
    n_[0] = n_[1] = 0;
    valid_ = false;
  }

  // qty == 0 removes the level. Returns false if the update was dropped
  // because it falls outside the tracked depth.
  bool set(Side side, Price price, Qty qty) noexcept {
    auto& lv = levels_[idx(side)];
    std::size_t& n = n_[idx(side)];
    const bool bid = side == Side::Bid;
    // First position whose price is not better than `price`.
    auto better = [bid](const Level& l, Price p) { return bid ? l.price > p : l.price < p; };
    Level* begin = lv.data();
    Level* end = begin + n;
    Level* it = std::lower_bound(begin, end, price, better);
    const std::size_t pos = static_cast<std::size_t>(it - begin);

    if (pos < n && it->price == price) {
      if (qty == 0) {
        std::memmove(it, it + 1, (n - pos - 1) * sizeof(Level));
        --n;
      } else {
        it->qty = qty;
      }
      check_crossed();
      return true;
    }
    if (qty == 0) return true;            // removing a level we don't have
    if (pos >= kDepth) return false;      // worse than everything we track
    const std::size_t move = std::min(n, kDepth - 1) - pos;
    std::memmove(it + 1, it, move * sizeof(Level));
    *it = Level{price, qty};
    n = std::min(n + 1, kDepth);
    check_crossed();
    return true;
  }

  bool empty(Side s) const noexcept { return n_[idx(s)] == 0; }
  std::size_t depth(Side s) const noexcept { return n_[idx(s)]; }
  const Level& level(Side s, std::size_t i) const noexcept { return levels_[idx(s)][i]; }
  const Level& best(Side s) const noexcept { return levels_[idx(s)][0]; }

  bool two_sided() const noexcept { return n_[0] > 0 && n_[1] > 0; }
  bool valid() const noexcept { return valid_ && two_sided(); }
  void set_valid(bool v) noexcept { valid_ = v; check_crossed(); }

  // Mid in ticks as a double (half-tick precision).
  double mid() const noexcept { return 0.5 * static_cast<double>(best(Side::Bid).price + best(Side::Ask).price); }

  // Sum of quantity over the best n levels.
  Qty depth_qty(Side s, std::size_t n) const noexcept {
    Qty total = 0;
    const std::size_t m = std::min(n, depth(s));
    for (std::size_t i = 0; i < m; ++i) total += levels_[idx(s)][i].qty;
    return total;
  }

  std::uint64_t crossed_events() const noexcept { return crossed_; }

 private:
  static constexpr std::size_t idx(Side s) noexcept { return static_cast<std::size_t>(s); }

  void check_crossed() noexcept {
    if (two_sided() && best(Side::Bid).price >= best(Side::Ask).price) {
      if (valid_) ++crossed_;
      valid_ = false;
    }
  }

  std::array<std::array<Level, kDepth>, 2> levels_{};
  std::array<std::size_t, 2> n_{0, 0};
  bool valid_ = false;
  std::uint64_t crossed_ = 0;
};

}  // namespace hft
