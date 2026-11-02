// Core vocabulary types shared by every engine, tool and test.
//
// Invariants: prices are integer ticks and quantities are integer lots; no floating point
// reaches matching (NFR-05). The enum values are the wire values of spec section 7(b).
// Thread-safety: plain value types, safe to copy across threads.
#pragma once

#include <cstdint>

namespace llme {

using Price = std::int64_t;     // ticks
using Qty = std::uint32_t;      // lots
using OrderId = std::uint64_t;  // top bit reserved for replay-synthesised aggressors

enum class Side : std::uint8_t { Buy = 0, Sell = 1 };
enum class MsgType : std::uint8_t { New = 1, Cancel = 2, Modify = 3 };
enum class OrdType : std::uint8_t { Limit = 0, Market = 1 };
enum class Tif : std::uint8_t { Gtc = 0, Ioc = 1, Fok = 2 };
enum class OutType : std::uint8_t {
  Ack = 1,
  Reject,
  Fill,
  Canceled,
  Modified,
  CancelReject,
  ModifyReject,
  Trade,
  LevelUpdate
};

enum class RejectReason : std::uint8_t {
  None = 0,
  InvalidQty = 1,
  PriceOutOfBand = 2,
  DuplicateId = 3,
  PoolFull = 4,
  UnknownId = 5,
  InvalidType = 6,
  InvalidSide = 7
};

enum class CancelReason : std::uint8_t {
  None = 0,
  User = 1,
  IocRemainder = 2,
  FokUnfillable = 3,
  NoLiquidity = 4,
  SelfTrade = 5
};

enum class StpMode : std::uint8_t { Off = 0, CancelNewest = 1 };

// OutMsg::flags bits.
inline constexpr std::uint8_t kFlagAggressor = 1U << 0U;
inline constexpr std::uint8_t kFlagPriorityLost = 1U << 1U;
inline constexpr std::uint8_t kFlagLevelRemoved = 1U << 2U;

[[nodiscard]] constexpr Side opposite(Side s) noexcept {
  return s == Side::Buy ? Side::Sell : Side::Buy;
}

struct EngineConfig {
  Price price_min = 0;
  Price price_max = 1048575;
  std::uint32_t max_orders = 1048576;
  double id_map_load = 0.5;
  StpMode stp = StpMode::Off;
};

}  // namespace llme
