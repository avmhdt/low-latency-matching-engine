// D3 level store: one sorted std::vector of levels per side, best price at the back (FR-26).
// Lookup is a binary search over contiguous memory, and the best level is the last element, so the
// common case touches the end of the array. Adding or removing a level away from the touch shifts
// every level behind it: that is the cost this design trades for locality.
//
// Invariants: each vector holds exactly the non-empty levels of its side, bids in ascending price
// and asks in descending price, so that back() is the best on both. Capacity is reserved in the
// constructor for every level a side can hold, so insert never reallocates.
// Thread-safety: belongs to the matching thread.
#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <vector>

#include "llme/order_pool.hpp"
#include "llme/types.hpp"

namespace llme {

class SortedVecLevelStore {
 public:
  explicit SortedVecLevelStore(const EngineConfig& cfg) {
    const auto band = static_cast<std::uint64_t>(cfg.price_max - cfg.price_min) + 1;
    const auto cap = static_cast<std::size_t>(std::min<std::uint64_t>(band, cfg.max_orders)) + 1;
    bids_.reserve(cap);
    asks_.reserve(cap);
    // Touch every page now, so that a level insert never faults one in.
    bids_.resize(cap);
    asks_.resize(cap);
    bids_.clear();
    asks_.clear();
  }

  [[nodiscard]] std::optional<Price> best(Side side) const noexcept {
    const auto& levels = of(side);
    return levels.empty() ? std::nullopt : std::optional<Price>(levels.back().price);
  }

  [[nodiscard]] std::optional<Price> next_after(Side side, Price price) const noexcept {
    const auto& levels = of(side);
    const auto it = position(side, levels, price);
    if (it == levels.begin()) {
      return std::nullopt;
    }
    return std::prev(it)->price;
  }

  [[nodiscard]] Level& level(Side side, Price price) noexcept {
    auto& levels = of(side);
    auto it = position(side, levels, price);
    if (it == levels.end() || it->price != price) {
      it = levels.insert(it, Entry{price, Level{}});
    }
    return it->level;
  }

  [[nodiscard]] const Level* find(Side side, Price price) const noexcept {
    const auto& levels = of(side);
    const auto it = position(side, levels, price);
    return it == levels.end() || it->price != price ? nullptr : &it->level;
  }

  void on_level_empty(Side side, Price price) noexcept {
    auto& levels = of(side);
    const auto it = position(side, levels, price);
    if (it != levels.end() && it->price == price) {
      levels.erase(it);
    }
  }

  void check_consistency() const {
    for (const Side side : {Side::Buy, Side::Sell}) {
      const auto& levels = of(side);
      for (std::size_t i = 0; i < levels.size(); ++i) {
        if (levels[i].level.count == 0) {
          throw std::logic_error("empty level left in the vector");
        }
        if (i > 0 && !before(side, levels[i - 1].price, levels[i].price)) {
          throw std::logic_error("levels out of order");
        }
      }
    }
  }

  [[nodiscard]] std::size_t bytes() const noexcept {
    return (bids_.capacity() + asks_.capacity()) * sizeof(Entry);
  }

 private:
  struct Entry {
    Price price = 0;
    Level level;
  };
  using Levels = std::vector<Entry>;

  // Storage order: worst price first, best price last.
  [[nodiscard]] static bool before(Side side, Price a, Price b) noexcept {
    return side == Side::Buy ? a < b : a > b;
  }

  // First entry not before price in storage order.
  [[nodiscard]] static Levels::const_iterator position(Side side, const Levels& levels,
                                                       Price price) noexcept {
    return std::lower_bound(levels.begin(), levels.end(), price,
                            [side](const Entry& e, Price p) { return before(side, e.price, p); });
  }
  [[nodiscard]] static Levels::iterator position(Side side, Levels& levels, Price price) noexcept {
    return std::lower_bound(levels.begin(), levels.end(), price,
                            [side](const Entry& e, Price p) { return before(side, e.price, p); });
  }

  [[nodiscard]] Levels& of(Side side) noexcept { return side == Side::Buy ? bids_ : asks_; }
  [[nodiscard]] const Levels& of(Side side) const noexcept {
    return side == Side::Buy ? bids_ : asks_;
  }

  Levels bids_;
  Levels asks_;
};

}  // namespace llme
