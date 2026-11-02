// D2 level store: per side, a dense Level array over [price_min, price_max] and a two-level
// occupancy bitmap (FR-11). Level access is one array index. Bit i of `bits` says level i is
// non-empty, and bit w of `summary` says word w of `bits` is non-zero, so the next non-empty level
// is found with a word scan on each layer plus a walk over summary words. The best price is
// cached, so that walk happens only when the best level empties.
//
// Invariants: a level's bit is set exactly when count > 0; `summary` matches `bits`; `best` is the
// highest set index for bids and the lowest for asks, or -1. All memory is touched in the
// constructor. Thread-safety: belongs to the matching thread.
#pragma once

#include <bit>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <vector>

#include "llme/order_pool.hpp"
#include "llme/types.hpp"

namespace llme {

class LadderLevelStore {
 public:
  explicit LadderLevelStore(const EngineConfig& cfg)
      : price_min_(cfg.price_min), bids_(level_count(cfg)), asks_(level_count(cfg)) {}

  [[nodiscard]] std::optional<Price> best(Side side) const noexcept {
    const Ladder& ladder = of(side);
    return ladder.best < 0 ? std::nullopt : std::optional<Price>(to_price(ladder.best));
  }

  // The next non-empty level behind `price`, moving away from the touch.
  [[nodiscard]] std::optional<Price> next_after(Side side, Price price) const noexcept {
    const Ladder& ladder = of(side);
    const std::int64_t index = side == Side::Buy ? ladder.highest_at_or_below(to_index(price) - 1)
                                                 : ladder.lowest_at_or_above(to_index(price) + 1);
    return index < 0 ? std::nullopt : std::optional<Price>(to_price(index));
  }

  // Returns the level, marking it occupied if it was empty.
  [[nodiscard]] Level& level(Side side, Price price) noexcept {
    Ladder& ladder = of(side);
    const std::int64_t index = to_index(price);
    Level& lvl = ladder.levels[static_cast<std::size_t>(index)];
    if (lvl.count == 0) {
      ladder.mark(index, side);
    }
    return lvl;
  }

  // Read-only lookup; nullptr for an empty level.
  [[nodiscard]] const Level* find(Side side, Price price) const noexcept {
    const Level& lvl = of(side).levels[static_cast<std::size_t>(to_index(price))];
    return lvl.count == 0 ? nullptr : &lvl;
  }

  void on_level_empty(Side side, Price price) noexcept { of(side).unmark(to_index(price), side); }

  // Throws std::logic_error on any broken invariant.
  void check_consistency() const {
    bids_.check(Side::Buy);
    asks_.check(Side::Sell);
  }

  [[nodiscard]] std::size_t bytes() const noexcept { return 2 * bids_.bytes(); }

 private:
  static constexpr unsigned kWordShift = 6;
  static constexpr std::uint64_t kWordMask = 63;

  struct Ladder {
    explicit Ladder(std::size_t levels_in_side)
        : levels(levels_in_side),
          bits(((levels_in_side - 1) >> kWordShift) + 1),
          summary(((bits.size() - 1) >> kWordShift) + 1) {}

    void mark(std::int64_t index, Side side) noexcept {
      const auto i = static_cast<std::uint64_t>(index);
      const std::uint64_t word = i >> kWordShift;
      bits[word] |= std::uint64_t{1} << (i & kWordMask);
      summary[word >> kWordShift] |= std::uint64_t{1} << (word & kWordMask);
      if (best < 0 || (side == Side::Buy ? index > best : index < best)) {
        best = index;
      }
    }

    void unmark(std::int64_t index, Side side) noexcept {
      const auto i = static_cast<std::uint64_t>(index);
      const std::uint64_t word = i >> kWordShift;
      bits[word] &= ~(std::uint64_t{1} << (i & kWordMask));
      if (bits[word] == 0) {
        summary[word >> kWordShift] &= ~(std::uint64_t{1} << (word & kWordMask));
      }
      if (index == best) {
        best = side == Side::Buy ? highest_at_or_below(index - 1) : lowest_at_or_above(index + 1);
      }
    }

    [[nodiscard]] std::int64_t highest_at_or_below(std::int64_t index) const noexcept {
      if (index < 0) {
        return -1;
      }
      const auto i = static_cast<std::uint64_t>(index);
      std::uint64_t word = i >> kWordShift;
      const std::uint64_t in_word =
          bits[word] & (~std::uint64_t{0} >> (kWordMask - (i & kWordMask)));
      if (in_word != 0) {
        return compose(word, kWordMask - static_cast<std::uint64_t>(std::countl_zero(in_word)));
      }
      if (word == 0) {
        return -1;
      }
      --word;
      std::uint64_t group = word >> kWordShift;
      std::uint64_t mask = summary[group] & (~std::uint64_t{0} >> (kWordMask - (word & kWordMask)));
      while (true) {
        if (mask != 0) {
          const std::uint64_t found = (group << kWordShift) + kWordMask -
                                      static_cast<std::uint64_t>(std::countl_zero(mask));
          return compose(found,
                         kWordMask - static_cast<std::uint64_t>(std::countl_zero(bits[found])));
        }
        if (group == 0) {
          return -1;
        }
        --group;
        mask = summary[group];
      }
    }

    [[nodiscard]] std::int64_t lowest_at_or_above(std::int64_t index) const noexcept {
      const auto i = static_cast<std::uint64_t>(index);
      if (i >= levels.size()) {
        return -1;
      }
      std::uint64_t word = i >> kWordShift;
      const std::uint64_t in_word = bits[word] & (~std::uint64_t{0} << (i & kWordMask));
      if (in_word != 0) {
        return compose(word, static_cast<std::uint64_t>(std::countr_zero(in_word)));
      }
      ++word;
      if (word >= bits.size()) {
        return -1;
      }
      std::uint64_t group = word >> kWordShift;
      std::uint64_t mask = summary[group] & (~std::uint64_t{0} << (word & kWordMask));
      while (true) {
        if (mask != 0) {
          const std::uint64_t found =
              (group << kWordShift) + static_cast<std::uint64_t>(std::countr_zero(mask));
          return compose(found, static_cast<std::uint64_t>(std::countr_zero(bits[found])));
        }
        ++group;
        if (group >= summary.size()) {
          return -1;
        }
        mask = summary[group];
      }
    }

    void check(Side side) const {
      std::int64_t expected_best = -1;
      for (std::size_t i = 0; i < levels.size(); ++i) {
        const bool set = ((bits[i >> kWordShift] >> (i & kWordMask)) & 1U) != 0;
        if (set != (levels[i].count != 0)) {
          throw std::logic_error("occupancy bit does not match its level");
        }
        if (set && (expected_best < 0 || side == Side::Buy)) {
          expected_best = static_cast<std::int64_t>(i);
        }
      }
      for (std::size_t w = 0; w < bits.size(); ++w) {
        const bool set = ((summary[w >> kWordShift] >> (w & kWordMask)) & 1U) != 0;
        if (set != (bits[w] != 0)) {
          throw std::logic_error("summary bit does not match its word");
        }
      }
      if (best != expected_best) {
        throw std::logic_error("cached best price is stale");
      }
    }

    [[nodiscard]] std::size_t bytes() const noexcept {
      return levels.size() * sizeof(Level) + (bits.size() + summary.size()) * sizeof(std::uint64_t);
    }

    [[nodiscard]] static std::int64_t compose(std::uint64_t word, std::uint64_t bit) noexcept {
      return static_cast<std::int64_t>((word << kWordShift) + bit);
    }

    std::vector<Level> levels;
    std::vector<std::uint64_t> bits;
    std::vector<std::uint64_t> summary;
    std::int64_t best = -1;
  };

  static std::size_t level_count(const EngineConfig& cfg) {
    constexpr std::int64_t kMaxLevels = std::int64_t{1} << 28U;
    if (cfg.price_min > cfg.price_max || cfg.price_max - cfg.price_min >= kMaxLevels) {
      throw std::invalid_argument("price band must be non-empty and at most 2^28 levels");
    }
    return static_cast<std::size_t>(cfg.price_max - cfg.price_min) + 1;
  }

  [[nodiscard]] Ladder& of(Side side) noexcept { return side == Side::Buy ? bids_ : asks_; }
  [[nodiscard]] const Ladder& of(Side side) const noexcept {
    return side == Side::Buy ? bids_ : asks_;
  }
  [[nodiscard]] std::int64_t to_index(Price price) const noexcept { return price - price_min_; }
  [[nodiscard]] Price to_price(std::int64_t index) const noexcept { return price_min_ + index; }

  Price price_min_;
  Ladder bids_;
  Ladder asks_;
};

}  // namespace llme
