// Preallocated order storage with an index free list, plus the intrusive FIFO of pool indices
// that forms one price level.
//
// Invariants: every slot is on the free list or in exactly one level; level.qty and level.count
// equal the sums over its orders; links are pool indices, kNil terminates. All memory is touched
// in the constructor, so nothing faults or allocates afterwards.
// Thread-safety: belongs to the matching thread.
#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>
#include <vector>

#include "llme/types.hpp"

namespace llme {

inline constexpr std::uint32_t kNil = std::numeric_limits<std::uint32_t>::max();

struct Order {
  OrderId id = 0;
  Price price = 0;
  std::uint64_t arrival = 0;  // queue-entry stamp, used only by the invariant checker
  std::uint32_t prev = kNil;
  std::uint32_t next = kNil;  // also links the free list
  Qty leaves = 0;
  std::uint32_t trader_id = 0;
  Side side = Side::Buy;
};

struct Level {
  std::uint64_t qty = 0;  // aggregate leaves; u64 because orders can each hold 2^32-1 lots
  std::uint32_t head = kNil;
  std::uint32_t tail = kNil;
  std::uint32_t count = 0;
};

class OrderPool {
 public:
  explicit OrderPool(std::uint32_t capacity) : orders_(capacity) {
    // Built back to front so the list hands out low indices first.
    for (std::uint32_t i = capacity; i > 0; --i) {
      orders_[i - 1].next = free_head_;
      free_head_ = i - 1;
    }
  }

  [[nodiscard]] bool full() const noexcept { return free_head_ == kNil; }
  [[nodiscard]] std::uint32_t live() const noexcept { return live_; }
  [[nodiscard]] std::uint32_t capacity() const noexcept {
    return static_cast<std::uint32_t>(orders_.size());
  }

  // Precondition: !full().
  [[nodiscard]] std::uint32_t allocate() noexcept {
    const std::uint32_t index = free_head_;
    free_head_ = orders_[index].next;
    ++live_;
    return index;
  }

  void release(std::uint32_t index) noexcept {
    orders_[index].next = free_head_;
    free_head_ = index;
    --live_;
  }

  [[nodiscard]] Order& operator[](std::uint32_t index) noexcept { return orders_[index]; }
  [[nodiscard]] const Order& operator[](std::uint32_t index) const noexcept {
    return orders_[index];
  }

  // Walks the free list; for the invariant checker.
  [[nodiscard]] std::uint32_t free_count() const noexcept {
    std::uint32_t count = 0;
    for (std::uint32_t i = free_head_; i != kNil && count <= capacity(); i = orders_[i].next) {
      ++count;
    }
    return count;
  }

  [[nodiscard]] std::size_t bytes() const noexcept { return orders_.size() * sizeof(Order); }

 private:
  std::vector<Order> orders_;
  std::uint32_t free_head_ = kNil;
  std::uint32_t live_ = 0;
};

// Appends to the tail of the level.
inline void level_push_back(OrderPool& pool, Level& level, std::uint32_t index) noexcept {
  Order& order = pool[index];
  order.prev = level.tail;
  order.next = kNil;
  if (level.tail == kNil) {
    level.head = index;
  } else {
    pool[level.tail].next = index;
  }
  level.tail = index;
  level.qty += order.leaves;
  ++level.count;
}

// Removes from anywhere in the level in O(1); level.qty loses the order's current leaves.
inline void level_unlink(OrderPool& pool, Level& level, std::uint32_t index) noexcept {
  const Order& order = pool[index];
  if (order.prev == kNil) {
    level.head = order.next;
  } else {
    pool[order.prev].next = order.next;
  }
  if (order.next == kNil) {
    level.tail = order.prev;
  } else {
    pool[order.next].prev = order.prev;
  }
  level.qty -= order.leaves;
  --level.count;
}

}  // namespace llme
