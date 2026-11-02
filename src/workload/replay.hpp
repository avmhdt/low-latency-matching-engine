// Shared mapping from real order events to .mlog messages (spec section 7(b)), plus replay of the
// resulting log. ITCH and LOBSTER both go through ReplayBuilder, and tools/py/llme_tools mirrors
// it line for line, so that every source gives identical .mlog semantics (D-04).
//
// Executions become IOC limits on the opposite side at the resting order's price. The aggressor's
// ID is the resting order's ID with the top bit set, which is how the expected contra travels
// with the log: replay reads it back to report exec_contra_match_rate.
//
// Thread-safety: one instance per thread. Tools only; never on the hot path.
#pragma once

#include <cstdint>
#include <functional>
#include <limits>
#include <optional>
#include <span>
#include <unordered_map>

#include "llme/messages.hpp"
#include "llme/types.hpp"

namespace llme::workload {

inline constexpr OrderId kSyntheticIdBit = OrderId{1} << 63U;
inline constexpr std::int64_t kRawPerTick = 100;        // source prices are USD x 10^4, tick $0.01
inline constexpr std::int64_t kTickSizeE9 = 10000000;  // $0.01 x 10^9
inline constexpr std::int64_t kSessionStartNs = 34200000000000;  // 09:30 ET
inline constexpr std::int64_t kSessionEndNs = 57600000000000;    // 16:00 ET

// DQ-06: what happened to every input event.
struct ConversionStats {
  std::uint64_t events = 0;

  // Input events by kind.
  std::uint64_t adds = 0;
  std::uint64_t execs = 0;
  std::uint64_t partial_cancels = 0;
  std::uint64_t deletes = 0;
  std::uint64_t replaces = 0;

  // Output messages by mapped type.
  std::uint64_t out_new = 0;
  std::uint64_t out_ioc = 0;
  std::uint64_t out_modify = 0;
  std::uint64_t out_cancel = 0;

  // DQ-01, DQ-02, DQ-04.
  std::uint64_t dropped_price = 0;
  std::uint64_t dropped_qty = 0;
  std::uint64_t dropped_unknown_id = 0;

  // DQ-05. Cross executions still reduce their order, so that the book stays true.
  std::uint64_t skipped_session = 0;
  std::uint64_t skipped_hidden = 0;
  std::uint64_t skipped_cross = 0;
  std::uint64_t skipped_halt = 0;

  std::uint32_t max_live_orders = 0;
  std::optional<Price> min_ticks;
  std::optional<Price> max_ticks;

  [[nodiscard]] std::uint64_t messages() const noexcept {
    return out_new + out_ioc + out_modify + out_cancel;
  }
};

class ReplayBuilder {
 public:
  using Sink = std::function<void(const InMsg&)>;

  explicit ReplayBuilder(Sink sink) : sink_(std::move(sink)) {}

  void add(OrderId id, Side side, std::int64_t raw_price, std::int64_t qty);
  void exec(OrderId id, std::int64_t qty);
  void partial_cancel(OrderId id, std::int64_t qty);
  void remove(OrderId id);
  void replace(OrderId id, OrderId new_id, std::int64_t raw_price, std::int64_t qty);
  // A non-printable execution from a cross: not matched, but the shares do leave the book.
  void cross_exec(OrderId id, std::int64_t qty);

  [[nodiscard]] ConversionStats& stats() noexcept { return stats_; }
  [[nodiscard]] const ConversionStats& stats() const noexcept { return stats_; }

 private:
  struct Tracked {
    Side side = Side::Buy;
    Price price = 0;
    Qty leaves = 0;
  };

  [[nodiscard]] Tracked* find(OrderId id);
  void shrink(OrderId id, Tracked& order, Qty removed);

  Sink sink_;
  std::unordered_map<OrderId, Tracked> orders_;
  ConversionStats stats_;
};

// DQ-07: observed band plus 10%, live orders x 1.5 rounded up to a power of two.
[[nodiscard]] EngineConfig replay_config(const ConversionStats& stats);

struct ReplayOutcome {
  std::uint64_t msgs = 0;
  std::uint64_t fills = 0;
  std::uint64_t crossed_add = 0;
  std::uint64_t exec_total = 0;
  std::uint64_t exec_contra_match = 0;
  std::uint64_t exec_unfilled = 0;
  std::uint64_t rejects = 0;
  std::uint64_t pool_full = 0;
  std::uint64_t price_out_of_band = 0;
  std::uint64_t cancel_rejects = 0;
  std::uint64_t modify_rejects = 0;
  std::uint64_t output_hash = 0;

  [[nodiscard]] double exec_contra_match_rate() const noexcept {
    return exec_total == 0 ? 0.0
                           : static_cast<double>(exec_contra_match) /
                                 static_cast<double>(exec_total);
  }
};

// Runs a log through D2 and reads off the numbers FR-21 asks a replay to report.
[[nodiscard]] ReplayOutcome replay_log(std::span<const InMsg> msgs, const EngineConfig& cfg);

}  // namespace llme::workload
