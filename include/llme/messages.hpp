// Fixed-layout wire records: MlogHeader (64 B), InMsg (32 B), OutMsg (48 B), spec section 7(b).
//
// Invariants: little-endian, trivially copyable, no implicit padding, every reserved byte zero
// (DQ-09). Enumerated fields are raw bytes because logs and fuzzers can carry any value; the
// engines validate them. Thread-safety: plain value types, safe to copy across threads.
#pragma once

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <type_traits>

#include "llme/types.hpp"

namespace llme {

static_assert(std::endian::native == std::endian::little, "wire formats are little-endian");

inline constexpr std::array<char, 8> kMlogMagic = {'L', 'L', 'M', 'E', 'M', 'L', 'O', 'G'};
inline constexpr std::array<char, 8> kOlogMagic = {'L', 'L', 'M', 'E', 'O', 'L', 'O', 'G'};
inline constexpr std::uint16_t kLogVersion = 1;

enum class LogSource : std::uint8_t { Synthetic = 0, BookL3 = 1, Lobster = 2, ItchFallback = 3 };

struct MlogHeader {
  std::array<char, 8> magic{};
  std::uint16_t version = 0;
  std::uint16_t record_size = 0;
  std::uint32_t symbol_id = 0;
  std::int64_t tick_size_e9 = 0;
  std::uint64_t record_count = 0;
  std::uint64_t seed = 0;
  std::uint8_t source = 0;
  std::array<std::uint8_t, 23> reserved{};
};

struct InMsg {
  std::uint8_t type = 0;      // MsgType
  std::uint8_t side = 0;      // Side; ignored for CANCEL and MODIFY
  std::uint8_t ord_type = 0;  // OrdType; NEW only
  std::uint8_t tif = 0;       // Tif; NEW only
  Qty qty = 0;                // NEW: order qty; MODIFY: new leaves
  OrderId order_id = 0;
  Price price = 0;  // ignored for MARKET and CANCEL
  std::uint32_t trader_id = 0;
  std::uint32_t reserved = 0;
};

struct OutMsg {
  std::uint8_t type = 0;    // OutType
  std::uint8_t side = 0;    // order side (FILL), aggressor side (TRADE), level side (LEVEL_UPDATE)
  std::uint8_t reason = 0;  // RejectReason or CancelReason; 0 when not applicable
  std::uint8_t flags = 0;   // kFlag* bits
  Qty qty = 0;
  std::uint64_t seq = 0;
  OrderId order_id = 0;
  OrderId contra_id = 0;
  Price price = 0;
  Qty leaves = 0;
  std::uint32_t reserved = 0;

  friend bool operator==(const OutMsg&, const OutMsg&) = default;
};

static_assert(sizeof(MlogHeader) == 64 && sizeof(InMsg) == 32 && sizeof(OutMsg) == 48);
static_assert(alignof(MlogHeader) == 8 && alignof(InMsg) == 8 && alignof(OutMsg) == 8);
static_assert(std::is_trivially_copyable_v<MlogHeader> && std::is_standard_layout_v<MlogHeader>);
static_assert(std::is_trivially_copyable_v<InMsg> && std::is_standard_layout_v<InMsg>);
static_assert(std::is_trivially_copyable_v<OutMsg> && std::is_standard_layout_v<OutMsg>);

static_assert(offsetof(MlogHeader, magic) == 0);
static_assert(offsetof(MlogHeader, version) == 8);
static_assert(offsetof(MlogHeader, record_size) == 10);
static_assert(offsetof(MlogHeader, symbol_id) == 12);
static_assert(offsetof(MlogHeader, tick_size_e9) == 16);
static_assert(offsetof(MlogHeader, record_count) == 24);
static_assert(offsetof(MlogHeader, seed) == 32);
static_assert(offsetof(MlogHeader, source) == 40);
static_assert(offsetof(MlogHeader, reserved) == 41);

static_assert(offsetof(InMsg, type) == 0);
static_assert(offsetof(InMsg, side) == 1);
static_assert(offsetof(InMsg, ord_type) == 2);
static_assert(offsetof(InMsg, tif) == 3);
static_assert(offsetof(InMsg, qty) == 4);
static_assert(offsetof(InMsg, order_id) == 8);
static_assert(offsetof(InMsg, price) == 16);
static_assert(offsetof(InMsg, trader_id) == 24);
static_assert(offsetof(InMsg, reserved) == 28);

static_assert(offsetof(OutMsg, type) == 0);
static_assert(offsetof(OutMsg, side) == 1);
static_assert(offsetof(OutMsg, reason) == 2);
static_assert(offsetof(OutMsg, flags) == 3);
static_assert(offsetof(OutMsg, qty) == 4);
static_assert(offsetof(OutMsg, seq) == 8);
static_assert(offsetof(OutMsg, order_id) == 16);
static_assert(offsetof(OutMsg, contra_id) == 24);
static_assert(offsetof(OutMsg, price) == 32);
static_assert(offsetof(OutMsg, leaves) == 40);
static_assert(offsetof(OutMsg, reserved) == 44);

[[nodiscard]] constexpr std::uint8_t to_wire(auto e) noexcept {
  return static_cast<std::uint8_t>(e);
}

}  // namespace llme
