#pragma once
// Bounded single-producer / single-consumer ring.
//
// - Capacity is a power of two, so index wrap is a mask, not a modulo.
// - head and tail live on separate cache lines; the producer only writes
//   tail, the consumer only writes head, so neither core bounces the other's
//   line on every operation (no false sharing).
// - Each side keeps a cached copy of the other side's index and only reloads
//   it (an acquire load across cores) when the ring looks full or empty.
// - Storage is allocated once in the constructor. push/pop never allocate,
//   never lock, never block. A full ring returns false and the caller decides.
//
// Correctness relies on exactly one producer thread and one consumer thread.
// That is the design rule of the whole engine: every piece of mutable state
// has exactly one owning thread, and rings are the only way across.

#include <atomic>
#include <cstddef>
#include <memory>
#include <new>
#include <stdexcept>
#include <type_traits>

namespace hft {

inline constexpr std::size_t kCacheLine = 64;

template <typename T>
class SpscRing {
  static_assert(std::is_trivially_copyable_v<T>, "ring slots are copied with plain stores");

 public:
  explicit SpscRing(std::size_t capacity_pow2)
      : mask_(capacity_pow2 - 1), slots_(std::make_unique<T[]>(capacity_pow2)) {
    if (capacity_pow2 < 2 || (capacity_pow2 & mask_) != 0) {
      throw std::invalid_argument("SpscRing capacity must be a power of two >= 2");
    }
  }

  SpscRing(const SpscRing&) = delete;
  SpscRing& operator=(const SpscRing&) = delete;

  // Producer thread only.
  bool try_push(const T& v) noexcept {
    const std::size_t tail = tail_.load(std::memory_order_relaxed);
    if (tail - head_cache_ > mask_) {
      head_cache_ = head_.load(std::memory_order_acquire);
      if (tail - head_cache_ > mask_) return false;  // full
    }
    slots_[tail & mask_] = v;
    tail_.store(tail + 1, std::memory_order_release);  // publish the slot
    return true;
  }

  // Consumer thread only.
  bool try_pop(T& out) noexcept {
    const std::size_t head = head_.load(std::memory_order_relaxed);
    if (head == tail_cache_) {
      tail_cache_ = tail_.load(std::memory_order_acquire);
      if (head == tail_cache_) return false;  // empty
    }
    out = slots_[head & mask_];
    head_.store(head + 1, std::memory_order_release);  // free the slot
    return true;
  }

  std::size_t capacity() const noexcept { return mask_ + 1; }

  // Approximate; for metrics only.
  std::size_t size_approx() const noexcept {
    return tail_.load(std::memory_order_acquire) - head_.load(std::memory_order_acquire);
  }

 private:
  const std::size_t mask_;
  std::unique_ptr<T[]> slots_;

  alignas(kCacheLine) std::atomic<std::size_t> head_{0};  // written by consumer
  alignas(kCacheLine) std::size_t tail_cache_{0};          // consumer's view of tail
  alignas(kCacheLine) std::atomic<std::size_t> tail_{0};  // written by producer
  alignas(kCacheLine) std::size_t head_cache_{0};          // producer's view of head
};

}  // namespace hft
