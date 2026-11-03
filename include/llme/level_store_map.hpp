// D1 level store: one std::pmr::map of levels per side, the textbook design and the baseline that
// D2 is measured against (FR-26). The map's nodes come from a fixed pool carved out of one buffer
// at construction, so this store allocates nothing on the hot path either. That isolates the
// layout under test, a balanced tree of separately placed nodes, from the cost of malloc.
//
// Invariants: a map holds exactly the non-empty levels of its side; bids sort descending and asks
// ascending, so begin() is the best price on both. Pool blocks in use = levels stored.
// Thread-safety: belongs to the matching thread.
#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <map>
#include <memory_resource>
#include <new>
#include <optional>
#include <stdexcept>
#include <vector>

#include "llme/order_pool.hpp"
#include "llme/types.hpp"

namespace llme {

// Fixed-size blocks from one preallocated buffer, recycled through a free list. A request larger
// than a block, or one past capacity, is a sizing bug: it throws, and on the noexcept hot path
// that terminates, which is louder than a silent fall-back to malloc.
class FixedBlockResource final : public std::pmr::memory_resource {
 public:
  static constexpr std::size_t kBlockBytes = 64;

  explicit FixedBlockResource(std::size_t blocks) : storage_(blocks * kBlockBytes) {
    free_.reserve(blocks);
    for (std::size_t i = blocks; i > 0; --i) {
      free_.push_back(storage_.data() + (i - 1) * kBlockBytes);
    }
  }

  [[nodiscard]] std::size_t bytes() const noexcept {
    return storage_.size() + free_.capacity() * sizeof(std::byte*);
  }

 private:
  void* do_allocate(std::size_t bytes, std::size_t alignment) override {
    if (bytes > kBlockBytes || alignment > alignof(std::max_align_t) || free_.empty()) {
      throw std::bad_alloc();
    }
    std::byte* block = free_.back();
    free_.pop_back();
    return block;
  }

  // Never past the reserved capacity: only blocks handed out come back.
  void do_deallocate(void* p, std::size_t /*bytes*/, std::size_t /*alignment*/) override {
    free_.push_back(static_cast<std::byte*>(p));
  }

  [[nodiscard]] bool do_is_equal(const memory_resource& other) const noexcept override {
    return this == &other;
  }

  std::vector<std::byte> storage_;  // operator new alignment, enough for a tree node
  std::vector<std::byte*> free_;    // stack of free blocks, capacity reserved up front
};

class MapLevelStore {
 public:
  explicit MapLevelStore(const EngineConfig& cfg)
      : bid_pool_(capacity(cfg)), ask_pool_(capacity(cfg)), bids_(&bid_pool_), asks_(&ask_pool_) {}

  [[nodiscard]] std::optional<Price> best(Side side) const noexcept {
    return side == Side::Buy ? first(bids_) : first(asks_);
  }

  [[nodiscard]] std::optional<Price> next_after(Side side, Price price) const noexcept {
    return side == Side::Buy ? after(bids_, price) : after(asks_, price);
  }

  [[nodiscard]] Level& level(Side side, Price price) noexcept {
    return side == Side::Buy ? bids_[price] : asks_[price];
  }

  [[nodiscard]] const Level* find(Side side, Price price) const noexcept {
    return side == Side::Buy ? lookup(bids_, price) : lookup(asks_, price);
  }

  void on_level_empty(Side side, Price price) noexcept {
    if (side == Side::Buy) {
      bids_.erase(price);
    } else {
      asks_.erase(price);
    }
  }

  void check_consistency() const {
    const auto check = [](const auto& side) {
      for (const auto& [price, level] : side) {
        if (level.count == 0) {
          throw std::logic_error("empty level left in the map");
        }
      }
    };
    check(bids_);
    check(asks_);
  }

  [[nodiscard]] std::size_t bytes() const noexcept { return bid_pool_.bytes() + ask_pool_.bytes(); }

 private:
  using Bids = std::pmr::map<Price, Level, std::greater<>>;
  using Asks = std::pmr::map<Price, Level, std::less<>>;

  // A side never holds more levels than live orders or prices in the band; one block per node.
  static std::size_t capacity(const EngineConfig& cfg) {
    const auto band = static_cast<std::uint64_t>(cfg.price_max - cfg.price_min) + 1;
    return static_cast<std::size_t>(std::min<std::uint64_t>(band, cfg.max_orders)) + 1;
  }

  template <class Map>
  static std::optional<Price> first(const Map& side) noexcept {
    return side.empty() ? std::nullopt : std::optional<Price>(side.begin()->first);
  }

  template <class Map>
  static std::optional<Price> after(const Map& side, Price price) noexcept {
    const auto it = side.upper_bound(price);
    return it == side.end() ? std::nullopt : std::optional<Price>(it->first);
  }

  template <class Map>
  static const Level* lookup(const Map& side, Price price) noexcept {
    const auto it = side.find(price);
    return it == side.end() ? nullptr : &it->second;
  }

  FixedBlockResource bid_pool_;
  FixedBlockResource ask_pool_;
  Bids bids_;
  Asks asks_;
};

}  // namespace llme
