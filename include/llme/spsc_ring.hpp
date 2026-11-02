// Bounded lock-free single-producer single-consumer ring (FR-13).
//
// Invariants: 0 <= tail - head <= N, with head and tail free-running counters masked on use. The
// producer alone writes tail and the slots in [tail, head + N); the consumer alone writes head and
// reads the slots in [head, tail). Items arrive in push order, with none lost or duplicated.
//
// Memory orders: the producer stores tail with release after writing the slot, and the consumer
// loads it with acquire, so a slot's contents are visible before its index is. The same pairing
// on head hands slots back. Nothing needs sequential consistency, because each index has one
// writer. Each side keeps a private cached copy of the other's index and reloads the shared atomic
// only when the cached value says full or empty, which keeps the two cache lines from bouncing on
// every operation. head, tail and the cached copies sit on separate cache lines to avoid false
// sharing, and the slots start on a line of their own.
//
// Thread-safety: exactly one thread pushes and exactly one thread pops, for the ring's lifetime.
// No locks and no allocation; the storage is inline, so large rings should live on the heap.
#pragma once

#include <array>
#include <atomic>
#include <bit>
#include <cstddef>

namespace llme {

template <class T, std::size_t N>
  requires(std::has_single_bit(N))
class SpscRing {
 public:
  [[nodiscard]] bool try_push(const T& v) noexcept {
    const std::size_t tail = tail_.load(std::memory_order_relaxed);
    if (tail - cached_head_ == N) {
      cached_head_ = head_.load(std::memory_order_acquire);
      if (tail - cached_head_ == N) {
        return false;
      }
    }
    slots_[tail & kMask] = v;
    tail_.store(tail + 1, std::memory_order_release);
    return true;
  }

  [[nodiscard]] bool try_pop(T& out) noexcept {
    const std::size_t head = head_.load(std::memory_order_relaxed);
    if (head == cached_tail_) {
      cached_tail_ = tail_.load(std::memory_order_acquire);
      if (head == cached_tail_) {
        return false;
      }
    }
    out = slots_[head & kMask];
    head_.store(head + 1, std::memory_order_release);
    return true;
  }

  // Pops up to max items into out; returns how many.
  [[nodiscard]] std::size_t pop_batch(T* out, std::size_t max) noexcept {
    const std::size_t head = head_.load(std::memory_order_relaxed);
    if (head == cached_tail_) {
      cached_tail_ = tail_.load(std::memory_order_acquire);
    }
    const std::size_t available = cached_tail_ - head;
    const std::size_t count = available < max ? available : max;
    for (std::size_t i = 0; i < count; ++i) {
      out[i] = slots_[(head + i) & kMask];
    }
    if (count != 0) {
      head_.store(head + count, std::memory_order_release);
    }
    return count;
  }

  // Items in the ring at this instant. Exact only when both sides are idle; used for depth stats.
  [[nodiscard]] std::size_t approx_size() const noexcept {
    const std::size_t head = head_.load(std::memory_order_acquire);
    const std::size_t tail = tail_.load(std::memory_order_acquire);
    return tail - head;
  }

  [[nodiscard]] static constexpr std::size_t capacity() noexcept { return N; }

 private:
  static constexpr std::size_t kMask = N - 1;
  static constexpr std::size_t kCacheLine = 64;

  alignas(kCacheLine) std::atomic<std::size_t> head_{0};
  alignas(kCacheLine) std::size_t cached_tail_{0};  // consumer's view of tail_
  alignas(kCacheLine) std::atomic<std::size_t> tail_{0};
  alignas(kCacheLine) std::size_t cached_head_{0};  // producer's view of head_
  alignas(kCacheLine) std::array<T, N> slots_{};
};

}  // namespace llme
