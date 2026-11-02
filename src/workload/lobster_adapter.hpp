// LOBSTER message and orderbook CSV parsing (FR-22).
//
// Message columns: time, type, order ID, size, price (USD x 10^4), direction. Orderbook columns:
// ask price, ask size, bid price, bid size, repeated per level. Any line is safe input: a line
// that does not parse is counted and dropped. Column definitions were confirmed against the
// ReadMe inside the sample; docs/decisions.md records the one difference from the spec.
// Thread-safety: one instance per thread. Tools only; never on the hot path.
#pragma once

#include <cstdint>
#include <string_view>

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

}  // namespace llme::workload
