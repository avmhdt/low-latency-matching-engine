// LOBSTER message and orderbook CSV parsing (FR-22).
//
// Message columns: time, type, order ID, size, price (USD x 10^4), direction. Orderbook columns:
// ask price, ask size, bid price, bid size, repeated per level. Any line is safe input: a line
// that does not parse is counted and dropped. Column definitions were confirmed against the
// ReadMe inside the sample; docs/decisions.md records the one difference from the spec.
// Thread-safety: one instance per thread. Tools only; never on the hot path.
#pragma once

#include <cstdint>
#include <memory>
#include <string_view>
#include <vector>

#include "llme/types.hpp"
#include "replay.hpp"

namespace llme::workload {

struct LobsterEvent {
  int type = 0;
  std::int64_t order_id = 0;
  std::int64_t size = 0;
  std::int64_t price = 0;
  int direction = 0;
};

[[nodiscard]] bool parse_lobster_message(std::string_view line, LobsterEvent& out) noexcept;

// Applies the mapping of spec section 7(b). Returns false for an event it cannot place.
bool apply_lobster_event(const LobsterEvent& event, ReplayBuilder& builder);

struct TopOfBook {
  std::int64_t ask_price = 0;
  std::int64_t ask_size = 0;
  std::int64_t bid_price = 0;
  std::int64_t bid_size = 0;
};

// First level of an orderbook line. An empty side shows up as a dummy price with size 0.
[[nodiscard]] bool parse_lobster_top(std::string_view line, TopOfBook& out) noexcept;

// Every column of an orderbook line, in file order: ask price, ask size, bid price, bid size, then
// the same for each deeper level.
[[nodiscard]] bool parse_lobster_row(std::string_view line, std::vector<std::int64_t>& out);

// Result of the one-step check: book from row i-1, one event, compared with row i.
struct OneStepCheck {
  std::uint64_t compared = 0;
  std::uint64_t agree = 0;
  std::uint64_t events_outside_window = 0;
};

// Rebuilds the visible book from each orderbook row, applies the next event through D2 and
// compares the top of book with the row after it. Events are taken as stated, by price and size,
// so this needs no order IDs and inherits no error from earlier messages. A sample reports only
// events inside its visible levels, which is why continuous replay of one drifts and this does not.
class OneStepChecker {
 public:
  explicit OneStepChecker(Price max_ticks);
  ~OneStepChecker();
  OneStepChecker(const OneStepChecker&) = delete;
  OneStepChecker& operator=(const OneStepChecker&) = delete;
  OneStepChecker(OneStepChecker&&) = delete;
  OneStepChecker& operator=(OneStepChecker&&) = delete;

  // previous and current are rows from parse_lobster_row; event is null for an unparsable line.
  void check(const std::vector<std::int64_t>& previous, const LobsterEvent* event,
             const std::vector<std::int64_t>& current);

  [[nodiscard]] const OneStepCheck& result() const noexcept { return result_; }

 private:
  struct Engine;
  std::unique_ptr<Engine> engine_;
  OneStepCheck result_;
};

}  // namespace llme::workload
