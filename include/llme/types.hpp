// Core vocabulary types shared by every engine, tool and test.
//
// Invariants: prices are integer ticks and quantities are integer lots; no floating point
// reaches matching (NFR-05). The enum values are the wire values of spec section 7(b).
// Thread-safety: plain value types, safe to copy across threads.
#pragma once

#include <cstdint>
#include <vector>

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

// Plain integers updated on the core and read at end of run (NFR-11).
struct EngineCounters {
  std::uint64_t msgs = 0;
  std::uint64_t acks = 0;
  std::uint64_t rejects = 0;
  std::uint64_t fills = 0;
  std::uint64_t trades = 0;
  std::uint64_t canceled = 0;
  std::uint64_t modified = 0;
  std::uint64_t cancel_rejects = 0;
  std::uint64_t modify_rejects = 0;
  std::uint64_t level_updates = 0;
  std::uint64_t out_records = 0;
  std::uint64_t levels_emptied = 0;
};

struct SnapshotOrder {
  OrderId id = 0;
  Qty leaves = 0;
  std::uint32_t trader_id = 0;

  friend bool operator==(const SnapshotOrder&, const SnapshotOrder&) = default;
};

struct SnapshotLevel {
  Price price = 0;
  std::uint64_t qty = 0;
  std::vector<SnapshotOrder> orders;  // queue order, oldest first

  friend bool operator==(const SnapshotLevel&, const SnapshotLevel&) = default;
};

// Full book state for tests and the differential harness; each side is best price first.
struct BookSnapshot {
  std::vector<SnapshotLevel> bids;
  std::vector<SnapshotLevel> asks;

  friend bool operator==(const BookSnapshot&, const BookSnapshot&) = default;
};

}  // namespace llme
