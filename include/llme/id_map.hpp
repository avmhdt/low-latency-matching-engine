// Open-addressing order-ID map: linear probing, power-of-two capacity, backward-shift deletion.
//
// Invariants: for every stored key, all slots from its home slot up to its own are occupied, which
// backward-shift deletion preserves without tombstones, so probe lengths never degrade. Any
// 64-bit key is valid, including 0; emptiness lives in the value (kNil), not the key.
// Capacity never changes after construction and at least one slot always stays empty.
// Thread-safety: belongs to the matching thread.
#pragma once

#include <bit>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "llme/order_pool.hpp"
#include "llme/types.hpp"

namespace llme {

class IdMap {
 public:
  // Capacity is the next power of two at or above max_entries / load, and above max_entries.
  IdMap(std::uint32_t max_entries, double load)
      : slots_(capacity_for(max_entries, load)),
        mask_(slots_.size() - 1),
        shift_(64U - static_cast<unsigned>(std::countr_zero(slots_.size()))) {}

  [[nodiscard]] std::uint32_t find(OrderId key) const noexcept {
    for (std::size_t i = home(key);; i = (i + 1) & mask_) {
      const Slot& slot = slots_[i];
      if (slot.value == kNil) {
        return kNil;
      }
      if (slot.key == key) {
        return slot.value;
      }
    }
  }

  // Precondition: key is absent and size() < max_entries.
  void insert(OrderId key, std::uint32_t value) noexcept {
    std::size_t i = home(key);
    while (slots_[i].value != kNil) {
      i = (i + 1) & mask_;
    }
    slots_[i] = {key, value};
    ++size_;
  }

  // Precondition: key is present.
  void erase(OrderId key) noexcept {
    std::size_t hole = home(key);
    while (slots_[hole].key != key || slots_[hole].value == kNil) {
      hole = (hole + 1) & mask_;
    }
    // Walk the cluster and pull back any entry whose home does not lie in (hole, entry].
    for (std::size_t i = (hole + 1) & mask_; slots_[i].value != kNil; i = (i + 1) & mask_) {
      const std::size_t entry_home = home(slots_[i].key);
      const bool stays = ((i - entry_home) & mask_) < ((i - hole) & mask_);
      if (!stays) {
        slots_[hole] = slots_[i];
        hole = i;
      }
    }
    slots_[hole] = Slot{};
    --size_;
  }

  [[nodiscard]] std::uint32_t size() const noexcept { return size_; }
  [[nodiscard]] std::size_t capacity() const noexcept { return slots_.size(); }
  [[nodiscard]] std::size_t bytes() const noexcept { return slots_.size() * sizeof(Slot); }

 private:
  struct Slot {
    OrderId key = 0;
    std::uint32_t value = kNil;
  };

  [[nodiscard]] static std::size_t capacity_for(std::uint32_t max_entries, double load) {
    const auto wanted = static_cast<std::size_t>(static_cast<double>(max_entries) / load) + 1;
    return std::bit_ceil(wanted < std::size_t{2} ? std::size_t{2} : wanted);
  }

  // Fibonacci hashing: multiplicative, keeps the high bits, and spreads sequential IDs evenly.
  [[nodiscard]] std::size_t home(OrderId key) const noexcept {
    return static_cast<std::size_t>((key * 0x9E3779B97F4A7C15ULL) >> shift_);
  }

  std::vector<Slot> slots_;
  std::size_t mask_;
  unsigned shift_;
  std::uint32_t size_ = 0;
};

}  // namespace llme
