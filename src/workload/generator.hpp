// Synthetic order-flow generator (FR-18).
//
// Determinism (DQ-08): the same profile and seed give byte-identical messages on every compiler.
// Randomness comes from xoshiro256** seeded by splitmix64, and every distribution is an integer
// transform written here. The floating point that remains turns profile numbers into integer
// thresholds, once, using only + - * / and sqrt, which IEEE 754 defines exactly; the library is
// built with -ffp-contract=off so no FMA changes a result.
//
// The generator keeps a ReferenceEngine as a shadow book. Cancels and modifies therefore target
// orders that are actually live, marketable orders are priced from the real touch, and the book
// size stays regulated. Any correct engine would produce the same stream, so a defect in a fast
// engine cannot change the input it is tested on. Thread-safety: one instance per thread.
#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "llme/messages.hpp"
#include "llme/reference_engine.hpp"
#include "llme/types.hpp"

namespace llme::workload {

class Rng {
 public:
  explicit Rng(std::uint64_t seed) noexcept;

  std::uint64_t next() noexcept;
  // Uniform in [0, bound); bound must be positive.
  std::uint64_t below(std::uint64_t bound) noexcept;
  // True with the probability encoded by threshold (see probability_threshold).
  bool chance(std::uint64_t threshold) noexcept { return next() < threshold; }

 private:
  std::array<std::uint64_t, 4> state_{};
};

// Fixed-point encoding of a probability; 1.0 and above saturate.
[[nodiscard]] std::uint64_t probability_threshold(double p) noexcept;

enum class PriceDist : std::uint8_t { Geometric, PowerLaw, Uniform };
enum class SizeDist : std::uint8_t { Geometric, Uniform };
enum class CancelTarget : std::uint8_t { Uniform, Oldest, Newest, NearTouch };

struct Mix {
  double new_limit = 0.48;
  double cancel = 0.38;
  double modify = 0.08;
  double ioc = 0.03;
  double fok = 0.01;
  double market = 0.02;
  double invalid = 0.0;  // malformed or stale requests, for the reject paths
};

struct Profile {
  std::string name = "balanced";
  std::uint64_t seed = 42;

  Price mid_start = 500000;
  double mid_walk_sigma = 1.0;  // ticks per 1000 messages
  Price half_spread = 1;
  // Resting and repriced orders stay in [price_floor, price_ceil]; unset means the engine band.
  std::optional<Price> price_floor;
  std::optional<Price> price_ceil;

  Mix mix;

  PriceDist price_dist = PriceDist::Geometric;
  double price_p = 0.25;
  double price_alpha = 1.5;
  std::uint32_t price_max_offset = 1024;
  double cross_prob = 0.05;

  SizeDist size_dist = SizeDist::Geometric;
  double size_mean = 5.0;
  Qty size_min = 1;
  Qty size_max = 100;

  CancelTarget cancel_target = CancelTarget::Uniform;

  // Shares among the modify flavours; they need not sum to 1.
  double modify_reduce = 0.5;
  double modify_increase = 0.2;
  double modify_reprice = 0.3;

  bool burst_enabled = false;
  std::uint64_t burst_length = 10000;
  std::uint64_t burst_every = 1000000;

  // A NEW that would take the shadow book past this many live orders becomes a cancel.
  std::uint32_t target_live = 100000;
  std::uint32_t traders = 8;

  EngineConfig engine;
};

// Reads a profile TOML; throws std::runtime_error with the file name on anything invalid.
[[nodiscard]] Profile load_profile(const std::filesystem::path& file);

// Reads the [engine] table of configs/engine.toml.
[[nodiscard]] EngineConfig load_engine_config(const std::filesystem::path& file);

// Header metadata for a log produced from this profile.
class Generator {
 public:
  Generator(const Profile& profile, std::uint64_t seed);

  [[nodiscard]] InMsg next();

  [[nodiscard]] std::uint64_t emitted() const noexcept { return emitted_; }
  [[nodiscard]] const ReferenceEngine& shadow() const noexcept { return shadow_; }

 private:
  struct Tracked {
    OrderId id = 0;
    Price price = 0;
    Qty qty = 0;
    Side side = Side::Buy;
    std::uint32_t dense_pos = 0;
    std::uint32_t older = kNil;
    std::uint32_t newer = kNil;
  };
  static constexpr std::uint32_t kNil = std::numeric_limits<std::uint32_t>::max();

  [[nodiscard]] InMsg make_message();
  [[nodiscard]] InMsg make_new_limit();
  [[nodiscard]] InMsg make_aggressive(Tif tif);
  [[nodiscard]] InMsg make_market();
  [[nodiscard]] InMsg make_cancel(CancelTarget target);
  [[nodiscard]] InMsg make_modify();
  [[nodiscard]] InMsg make_invalid();

  [[nodiscard]] Side random_side() noexcept;
  [[nodiscard]] Qty random_size() noexcept;
  [[nodiscard]] std::uint32_t random_offset() noexcept;
  [[nodiscard]] Price clamp_price(Price p) const noexcept;
  [[nodiscard]] Price passive_price(Side side) noexcept;
  [[nodiscard]] Price marketable_price(Side side) noexcept;
  [[nodiscard]] OrderId fresh_id() noexcept { return ++last_id_; }
  [[nodiscard]] std::uint32_t pick_target(CancelTarget target);

  void advance_mid() noexcept;
  void apply(const InMsg& m);
  void track(const InMsg& m, Qty leaves);
  void untrack(std::uint32_t slot);
  void update_tracked(std::uint32_t slot, Price price, Qty qty, bool to_tail);
  void link_newest(std::uint32_t slot);
  void unlink_age(std::uint32_t slot);

  Profile profile_;
  Rng rng_;
  ReferenceEngine shadow_;
  std::vector<OutMsg> out_;

  Price mid_;
  Price floor_;
  Price ceil_;
  std::uint64_t mid_step_threshold_;
  std::uint64_t cross_threshold_;
  std::uint64_t price_geometric_threshold_;
  std::uint64_t size_geometric_threshold_;
  std::array<std::uint64_t, 6> mix_cumulative_{};  // new, cancel, modify, ioc, fok, market
  std::array<std::uint64_t, 2> modify_cumulative_{};
  std::vector<std::uint64_t> power_law_cdf_;

  std::vector<Tracked> slots_;
  std::vector<std::uint32_t> free_slots_;
  std::vector<std::uint32_t> dense_;
  std::unordered_map<OrderId, std::uint32_t> slot_of_;
  std::uint32_t oldest_ = kNil;
  std::uint32_t newest_ = kNil;

  OrderId last_id_ = 0;
  std::uint64_t emitted_ = 0;
  std::uint64_t burst_left_ = 0;
};

}  // namespace llme::workload
