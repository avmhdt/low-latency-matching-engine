// MatchingEngine: the hot path. One instance owns one book and runs on one thread.
//
// Invariants after every message (FR-19): the book is not crossed; each level's cached qty and
// count equal the sums over its orders; FIFO order equals queue-entry order; the ID map holds
// exactly the live orders; pool free + live = capacity; occupancy matches non-empty levels.
//
// Hot-path rules (NFR-03): after the constructor, on_message performs no heap allocation, system
// call, lock, exception, virtual call or logging. Capacity exhaustion is a REJECT, never a resize.
// Matching uses integers only. Output depends only on the input messages: nothing reads time,
// addresses or a container's iteration order.
//
// Thread-safety: on_message and the accessors must all run on the owning thread.
#pragma once

#include <algorithm>
#include <array>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>

#include "llme/id_map.hpp"
#include "llme/messages.hpp"
#include "llme/order_pool.hpp"
#include "llme/price_ladder.hpp"
#include "llme/types.hpp"

namespace llme {

template <class S>
concept OutputSink = requires(S& s, const OutMsg& m) {
  { s(m) } noexcept;
};

template <class L>
concept LevelStore = requires(L& l, const L& cl, Side sd, Price p) {
  { cl.best(sd) } noexcept -> std::same_as<std::optional<Price>>;
  { cl.next_after(sd, p) } noexcept -> std::same_as<std::optional<Price>>;
  { cl.find(sd, p) } noexcept -> std::same_as<const Level*>;
  { l.level(sd, p) } noexcept -> std::same_as<Level&>;
  { l.on_level_empty(sd, p) } noexcept;
  { cl.check_consistency() };
};

// Discards output; used to measure matching alone.
struct NullSink {
  void operator()(const OutMsg& /*m*/) const noexcept {}
};

template <LevelStore L, OutputSink Sink>
class MatchingEngine {
 public:
  // Allocates and prefaults everything; throws std::invalid_argument on a bad config.
  MatchingEngine(const EngineConfig& cfg, Sink sink)
      : cfg_(validated(cfg)),
        levels_(cfg),
        pool_(cfg.max_orders),
        ids_(cfg.max_orders, cfg.id_map_load),
        sink_(std::move(sink)) {}

  void on_message(const InMsg& m) noexcept {
    ++msgs_;
    if (m.type == to_wire(MsgType::New)) {
      on_new(m);
    } else if (m.type == to_wire(MsgType::Cancel)) {
      on_cancel(m);
    } else if (m.type == to_wire(MsgType::Modify)) {
      on_modify(m);
    } else [[unlikely]] {
      reject(m, RejectReason::InvalidType);
    }
  }

  [[nodiscard]] std::optional<Price> best_bid() const noexcept { return levels_.best(Side::Buy); }
  [[nodiscard]] std::optional<Price> best_ask() const noexcept { return levels_.best(Side::Sell); }

  [[nodiscard]] std::uint64_t depth_at(Side side, Price price) const noexcept {
    const Level* level = levels_.find(side, price);
    return level == nullptr ? 0 : level->qty;
  }

  [[nodiscard]] std::uint32_t live_orders() const noexcept { return pool_.live(); }

  [[nodiscard]] EngineCounters counters() const noexcept {
    const auto count = [&](OutType type) { return out_counts_[to_wire(type)]; };
    return {.msgs = msgs_,
            .acks = count(OutType::Ack),
            .rejects = count(OutType::Reject),
            .fills = count(OutType::Fill),
            .trades = count(OutType::Trade),
            .canceled = count(OutType::Canceled),
            .modified = count(OutType::Modified),
            .cancel_rejects = count(OutType::CancelReject),
            .modify_rejects = count(OutType::ModifyReject),
            .level_updates = count(OutType::LevelUpdate),
            .out_records = seq_};
  }

  // Bytes held by the book structures; printed at start-up and checked against NFR-12.
  [[nodiscard]] std::size_t footprint_bytes() const noexcept {
    return levels_.bytes() + pool_.bytes() + ids_.bytes();
  }

  // Tests and debug only: full O(book) check; throws std::logic_error on any violation.
  void check_invariants() const {
    check_counts();
    levels_.check_consistency();
    std::uint64_t resting = 0;
    for (const Side side : {Side::Buy, Side::Sell}) {
      for (auto price = levels_.best(side); price; price = levels_.next_after(side, *price)) {
        check_level(side, *price);
        resting += levels_.find(side, *price)->count;
      }
    }
    require(resting == pool_.live(), "level counts do not add up to the live orders");
  }

  // O(1) part of the check, cheap enough to run after every message.
  void check_counts() const {
    const auto bid = levels_.best(Side::Buy);
    const auto ask = levels_.best(Side::Sell);
    require(!bid || !ask || *bid < *ask, "book is crossed");
    require(ids_.size() == pool_.live(), "id map size differs from live orders");
  }

  // Walks one level; O(orders in the level).
  void check_level(Side side, Price price) const {
    const Level* level = levels_.find(side, price);
    if (level == nullptr) {
      return;
    }
    std::uint64_t qty = 0;
    std::uint32_t count = 0;
    std::uint32_t prev = kNil;
    std::uint64_t arrival = 0;
    for (std::uint32_t i = level->head; i != kNil; i = pool_[i].next) {
      const Order& order = pool_[i];
      require(count < pool_.capacity(), "cycle in level queue");
      require(order.prev == prev, "broken prev link");
      require(order.side == side && order.price == price, "order on the wrong level");
      require(order.leaves > 0, "empty order on the book");
      require(count == 0 || order.arrival > arrival, "queue is not in arrival order");
      require(ids_.find(order.id) == i, "id map does not point at the order");
      arrival = order.arrival;
      qty += order.leaves;
      ++count;
      prev = i;
    }
    require(level->tail == prev, "level tail is stale");
    require(level->qty == qty, "cached level qty is wrong");
    require(level->count == count, "cached level count is wrong");
  }

  // Walks the free list as well; O(capacity).
  void check_pool() const {
    require(pool_.free_count() + pool_.live() == pool_.capacity(),
            "pool free + live differs from capacity");
  }

  // Tests only: allocates.
  [[nodiscard]] BookSnapshot snapshot() const {
    BookSnapshot snap;
    copy_side(Side::Buy, snap.bids);
    copy_side(Side::Sell, snap.asks);
    return snap;
  }

 private:
  friend struct EngineTestAccess;

  static const EngineConfig& validated(const EngineConfig& cfg) {
    if (cfg.max_orders == 0 || cfg.max_orders == kNil) {
      throw std::invalid_argument("max_orders must be in [1, 2^32 - 2]");
    }
    if (!(cfg.id_map_load > 0.0 && cfg.id_map_load <= 1.0)) {
      throw std::invalid_argument("id_map_load must be in (0, 1]");
    }
    if (cfg.stp != StpMode::Off) {
      throw std::invalid_argument("self-trade prevention is not implemented");
    }
    return cfg;
  }

  static void require(bool condition, const char* what) {
    if (!condition) {
      throw std::logic_error(what);
    }
  }

  void copy_side(Side side, std::vector<SnapshotLevel>& out) const {
    for (auto price = levels_.best(side); price; price = levels_.next_after(side, *price)) {
      const Level* level = levels_.find(side, *price);
      SnapshotLevel copy;
      copy.price = *price;
      copy.qty = level->qty;
      for (std::uint32_t i = level->head; i != kNil; i = pool_[i].next) {
        copy.orders.push_back({pool_[i].id, pool_[i].leaves, pool_[i].trader_id});
      }
      out.push_back(std::move(copy));
    }
  }

  [[nodiscard]] bool in_band(Price price) const noexcept {
    return price >= cfg_.price_min && price <= cfg_.price_max;
  }

  void on_new(const InMsg& m) noexcept {
    if (m.side > to_wire(Side::Sell)) [[unlikely]] {
      return reject(m, RejectReason::InvalidSide);
    }
    if (m.ord_type > to_wire(OrdType::Market) || m.tif > to_wire(Tif::Fok)) [[unlikely]] {
      return reject(m, RejectReason::InvalidType);
    }
    if (m.qty == 0) [[unlikely]] {
      return reject(m, RejectReason::InvalidQty);
    }
    const bool market = m.ord_type == to_wire(OrdType::Market);
    if (!market && !in_band(m.price)) [[unlikely]] {
      return reject(m, RejectReason::PriceOutOfBand);
    }
    if (ids_.find(m.order_id) != kNil) [[unlikely]] {
      return reject(m, RejectReason::DuplicateId);
    }
    const bool rests = !market && m.tif == to_wire(Tif::Gtc);
    if (rests && pool_.full()) [[unlikely]] {
      return reject(m, RejectReason::PoolFull);
    }

    const auto side = static_cast<Side>(m.side);
    const Price price = market ? 0 : m.price;
    emit({.type = to_wire(OutType::Ack),
          .side = m.side,
          .qty = m.qty,
          .order_id = m.order_id,
          .price = price,
          .leaves = m.qty});

    if (!market && m.tif == to_wire(Tif::Fok) && !fok_fillable(side, m.price, m.qty)) {
      return emit_canceled(m.order_id, side, price, m.qty, CancelReason::FokUnfillable);
    }
    const Qty leaves = match(side, m.order_id, price, market, m.qty);
    if (leaves == 0) {
      return;
    }
    if (rests) {
      const std::uint32_t index = pool_.allocate();
      Order& order = pool_[index];
      order.id = m.order_id;
      order.side = side;
      order.trader_id = m.trader_id;
      ids_.insert(m.order_id, index);
      enqueue(index, m.price, leaves);
    } else {
      const bool none_filled = market && leaves == m.qty;
      emit_canceled(m.order_id, side, price, leaves,
                    none_filled ? CancelReason::NoLiquidity : CancelReason::IocRemainder);
    }
  }

  void on_cancel(const InMsg& m) noexcept {
    const std::uint32_t index = ids_.find(m.order_id);
    if (index == kNil) {
      emit({.type = to_wire(OutType::CancelReject),
            .reason = to_wire(RejectReason::UnknownId),
            .order_id = m.order_id});
      return;
    }
    cancel_resting(index);
  }

  void on_modify(const InMsg& m) noexcept {
    const std::uint32_t index = ids_.find(m.order_id);
    if (index == kNil) {
      return reject_modify(m, RejectReason::UnknownId);
    }
    if (m.qty == 0) {
      return cancel_resting(index);
    }
    if (!in_band(m.price)) {
      return reject_modify(m, RejectReason::PriceOutOfBand);
    }

    Order& order = pool_[index];
    const Side side = order.side;
    const Price old_price = order.price;
    const auto modified = [&](std::uint8_t flags) {
      emit({.type = to_wire(OutType::Modified),
            .side = to_wire(side),
            .flags = flags,
            .qty = m.qty,
            .order_id = m.order_id,
            .price = m.price,
            .leaves = m.qty});
    };

    if (m.price == old_price && m.qty <= order.leaves) {
      const Qty reduction = order.leaves - m.qty;
      Level& level = levels_.level(side, old_price);
      order.leaves = m.qty;
      level.qty -= reduction;
      modified(0);
      if (reduction != 0) {
        emit_level(side, old_price, level.qty);
      }
      return;
    }

    Level& old_level = levels_.level(side, old_price);
    level_unlink(pool_, old_level, index);
    const std::uint64_t old_qty = old_level.qty;
    if (old_level.count == 0) {
      levels_.on_level_empty(side, old_price);
    }
    modified(kFlagPriorityLost);
    if (m.price != old_price) {
      emit_level(side, old_price, old_qty);
    }
    const Qty leaves = match(side, m.order_id, m.price, false, m.qty);
    if (leaves == 0) {
      ids_.erase(m.order_id);
      pool_.release(index);
    } else {
      enqueue(index, m.price, leaves);
    }
  }

  // Puts an order at the tail of its level and publishes the level.
  void enqueue(std::uint32_t index, Price price, Qty leaves) noexcept {
    Order& order = pool_[index];
    order.price = price;
    order.leaves = leaves;
    order.arrival = ++arrivals_;
    Level& level = levels_.level(order.side, price);
    level_push_back(pool_, level, index);
    emit_level(order.side, price, level.qty);
  }

  void cancel_resting(std::uint32_t index) noexcept {
    const Order& order = pool_[index];
    const OrderId id = order.id;
    const Side side = order.side;
    const Price price = order.price;
    const Qty leaves = order.leaves;
    Level& level = levels_.level(side, price);
    level_unlink(pool_, level, index);
    const std::uint64_t remaining = level.qty;
    if (level.count == 0) {
      levels_.on_level_empty(side, price);
    }
    ids_.erase(id);
    pool_.release(index);
    emit_canceled(id, side, price, leaves, CancelReason::User);
    emit_level(side, price, remaining);
  }

  [[nodiscard]] static bool crosses(Side side, Price limit, Price contra_price) noexcept {
    return side == Side::Buy ? contra_price <= limit : contra_price >= limit;
  }

  [[nodiscard]] bool fok_fillable(Side side, Price limit, Qty qty) const noexcept {
    const Side contra = opposite(side);
    std::uint64_t available = 0;
    for (auto price = levels_.best(contra); price && crosses(side, limit, *price);
         price = levels_.next_after(contra, *price)) {
      available += levels_.find(contra, *price)->qty;
      if (available >= qty) {
        return true;
      }
    }
    return false;
  }

  // Trades qty against the contra side; returns what is left. The aggressor is not on the book.
  Qty match(Side side, OrderId id, Price limit, bool market, Qty qty) noexcept {
    const Side contra = opposite(side);
    while (qty > 0) {
      const auto best = levels_.best(contra);
      if (!best || (!market && !crosses(side, limit, *best))) {
        break;
      }
      const Price price = *best;
      Level& level = levels_.level(contra, price);
      while (qty > 0 && level.count > 0) {
        qty = fill_head(level, side, id, price, qty);
      }
    }
    return qty;
  }

  Qty fill_head(Level& level, Side side, OrderId id, Price price, Qty qty) noexcept {
    const std::uint32_t index = level.head;
    Order& resting = pool_[index];
    const OrderId resting_id = resting.id;
    const Side contra = resting.side;
    const Qty fill = std::min(qty, resting.leaves);
    resting.leaves -= fill;
    level.qty -= fill;
    qty -= fill;

    emit({.type = to_wire(OutType::Fill),
          .side = to_wire(contra),
          .qty = fill,
          .order_id = resting_id,
          .contra_id = id,
          .price = price,
          .leaves = resting.leaves});
    emit({.type = to_wire(OutType::Fill),
          .side = to_wire(side),
          .flags = kFlagAggressor,
          .qty = fill,
          .order_id = id,
          .contra_id = resting_id,
          .price = price,
          .leaves = qty});
    emit({.type = to_wire(OutType::Trade),
          .side = to_wire(side),
          .qty = fill,
          .order_id = id,
          .contra_id = resting_id,
          .price = price,
          .leaves = qty});

    if (resting.leaves == 0) {
      level_unlink(pool_, level, index);
      ids_.erase(resting_id);
      pool_.release(index);
      if (level.count == 0) {
        levels_.on_level_empty(contra, price);
      }
    }
    emit_level(contra, price, level.qty);
    return qty;
  }

  void reject(const InMsg& m, RejectReason reason) noexcept {
    emit({.type = to_wire(OutType::Reject),
          .side = m.side,
          .reason = to_wire(reason),
          .order_id = m.order_id});
  }

  void reject_modify(const InMsg& m, RejectReason reason) noexcept {
    emit({.type = to_wire(OutType::ModifyReject),
          .reason = to_wire(reason),
          .order_id = m.order_id});
  }

  void emit_canceled(OrderId id, Side side, Price price, Qty qty, CancelReason reason) noexcept {
    emit({.type = to_wire(OutType::Canceled),
          .side = to_wire(side),
          .reason = to_wire(reason),
          .qty = qty,
          .order_id = id,
          .price = price});
  }

  void emit_level(Side side, Price price, std::uint64_t aggregate) noexcept {
    constexpr std::uint64_t kMaxQty = std::numeric_limits<Qty>::max();
    emit({.type = to_wire(OutType::LevelUpdate),
          .side = to_wire(side),
          .flags = aggregate == 0 ? kFlagLevelRemoved : std::uint8_t{0},
          .qty = static_cast<Qty>(std::min(aggregate, kMaxQty)),
          .price = price});
  }

  void emit(OutMsg r) noexcept {
    r.seq = ++seq_;
    ++out_counts_[r.type];
    sink_(r);
  }

  EngineConfig cfg_;
  L levels_;
  OrderPool pool_;
  IdMap ids_;
  Sink sink_;
  std::uint64_t seq_ = 0;
  std::uint64_t arrivals_ = 0;
  std::uint64_t msgs_ = 0;
  std::array<std::uint64_t, 10> out_counts_{};
};

}  // namespace llme
