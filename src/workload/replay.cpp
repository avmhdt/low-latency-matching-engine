#include "replay.hpp"

#include <algorithm>
#include <bit>
#include <utility>

#include "llme/engine.hpp"
#include "llme/output_hash.hpp"

namespace llme::workload {

namespace {

constexpr std::int64_t kMaxQty = std::numeric_limits<Qty>::max();

}  // namespace

ReplayBuilder::Tracked* ReplayBuilder::find(OrderId id) {
  const auto found = orders_.find(id);
  if (found == orders_.end()) {
    ++stats_.dropped_unknown_id;
    return nullptr;
  }
  return &found->second;
}

void ReplayBuilder::add(OrderId id, Side side, std::int64_t raw_price, std::int64_t qty) {
  ++stats_.events;
  ++stats_.adds;
  if (qty <= 0 || qty > kMaxQty) {
    ++stats_.dropped_qty;
    return;
  }
  if (raw_price % kRawPerTick != 0 || (id & kSyntheticIdBit) != 0) {
    ++stats_.dropped_price;
    return;
  }
  const Price price = raw_price / kRawPerTick;
  orders_[id] = {side, price, static_cast<Qty>(qty)};
  stats_.max_live_orders =
      std::max(stats_.max_live_orders, static_cast<std::uint32_t>(orders_.size()));
  stats_.min_ticks = std::min(stats_.min_ticks.value_or(price), price);
  stats_.max_ticks = std::max(stats_.max_ticks.value_or(price), price);

  InMsg m;
  m.type = to_wire(MsgType::New);
  m.side = to_wire(side);
  m.ord_type = to_wire(OrdType::Limit);
  m.tif = to_wire(Tif::Gtc);
  m.qty = static_cast<Qty>(qty);
  m.order_id = id;
  m.price = price;
  ++stats_.out_new;
  sink_(m);
}

void ReplayBuilder::shrink(OrderId id, Tracked& order, Qty removed) {
  order.leaves -= removed;
  if (order.leaves == 0) {
    orders_.erase(id);
  }
}

void ReplayBuilder::exec(OrderId id, std::int64_t qty) {
  ++stats_.events;
  ++stats_.execs;
  if (qty <= 0 || qty > kMaxQty) {
    ++stats_.dropped_qty;
    return;
  }
  Tracked* order = find(id);
  if (order == nullptr) {
    return;
  }
  const Qty filled = std::min(static_cast<Qty>(qty), order->leaves);
  InMsg m;
  m.type = to_wire(MsgType::New);
  m.side = to_wire(opposite(order->side));
  m.ord_type = to_wire(OrdType::Limit);
  m.tif = to_wire(Tif::Ioc);
  m.qty = filled;
  m.order_id = id | kSyntheticIdBit;
  m.price = order->price;
  ++stats_.out_ioc;
  sink_(m);
  shrink(id, *order, filled);
}

void ReplayBuilder::partial_cancel(OrderId id, std::int64_t qty) {
  ++stats_.events;
  ++stats_.partial_cancels;
  if (qty <= 0 || qty > kMaxQty) {
    ++stats_.dropped_qty;
    return;
  }
  Tracked* order = find(id);
  if (order == nullptr) {
    return;
  }
  const Qty removed = std::min(static_cast<Qty>(qty), order->leaves);
  InMsg m;
  m.type = to_wire(MsgType::Modify);
  m.qty = order->leaves - removed;
  m.order_id = id;
  m.price = order->price;
  ++stats_.out_modify;
  sink_(m);
  shrink(id, *order, removed);
}

void ReplayBuilder::remove(OrderId id) {
  ++stats_.events;
  ++stats_.deletes;
  if (find(id) == nullptr) {
    return;
  }
  InMsg m;
  m.type = to_wire(MsgType::Cancel);
  m.order_id = id;
  ++stats_.out_cancel;
  sink_(m);
  orders_.erase(id);
}

void ReplayBuilder::replace(OrderId id, OrderId new_id, std::int64_t raw_price, std::int64_t qty) {
  ++stats_.events;
  ++stats_.replaces;
  const Tracked* order = find(id);
  if (order == nullptr) {
    return;
  }
  const Side side = order->side;
  InMsg m;
  m.type = to_wire(MsgType::Cancel);
  m.order_id = id;
  ++stats_.out_cancel;
  sink_(m);
  orders_.erase(id);

  add(new_id, side, raw_price, qty);
  --stats_.events;
  --stats_.adds;
}

void ReplayBuilder::cross_exec(OrderId id, std::int64_t qty) {
  ++stats_.events;
  ++stats_.skipped_cross;
  if (qty <= 0 || qty > kMaxQty) {
    ++stats_.dropped_qty;
    return;
  }
  Tracked* order = find(id);
  if (order == nullptr) {
    return;
  }
  const Qty removed = std::min(static_cast<Qty>(qty), order->leaves);
  InMsg m;
  m.type = to_wire(MsgType::Modify);
  m.qty = order->leaves - removed;
  m.order_id = id;
  m.price = order->price;
  ++stats_.out_modify;
  sink_(m);
  shrink(id, *order, removed);
}

EngineConfig replay_config(const ConversionStats& stats) {
  const Price low = stats.min_ticks.value_or(0);
  const Price high = stats.max_ticks.value_or(0);
  const Price margin = (high - low) / 10 + 1;
  const std::uint64_t wanted = std::uint64_t{stats.max_live_orders} * 3 / 2 + 1;
  EngineConfig config;
  config.price_min = std::max<Price>(low - margin, 0);
  config.price_max = high + margin;
  config.max_orders = static_cast<std::uint32_t>(std::bit_ceil(wanted));
  return config;
}

namespace {

// Watches the output of the message in flight.
struct ReplaySink {
  ReplayOutcome* outcome;
  OutputHasher* hasher;
  bool* saw_fill;
  OrderId* first_contra;

  void operator()(const OutMsg& r) const noexcept {
    hasher->update(r);
    if (r.type == to_wire(OutType::Fill) && (r.flags & kFlagAggressor) != 0) {
      ++outcome->fills;
      if (!*saw_fill) {
        *saw_fill = true;
        *first_contra = r.contra_id;
      }
    } else if (r.type == to_wire(OutType::Reject)) {
      ++outcome->rejects;
      outcome->pool_full += r.reason == to_wire(RejectReason::PoolFull) ? 1U : 0U;
      outcome->price_out_of_band += r.reason == to_wire(RejectReason::PriceOutOfBand) ? 1U : 0U;
    } else if (r.type == to_wire(OutType::CancelReject)) {
      ++outcome->cancel_rejects;
    } else if (r.type == to_wire(OutType::ModifyReject)) {
      ++outcome->modify_rejects;
    }
  }
};

}  // namespace

ReplayOutcome replay_log(std::span<const InMsg> msgs, const EngineConfig& cfg) {
  ReplayOutcome outcome;
  OutputHasher hasher;
  bool saw_fill = false;
  OrderId first_contra = 0;
  MatchingEngine<LadderLevelStore, ReplaySink> engine(
      cfg, ReplaySink{&outcome, &hasher, &saw_fill, &first_contra});

  for (const InMsg& m : msgs) {
    saw_fill = false;
    engine.on_message(m);
    if (m.type != to_wire(MsgType::New)) {
      continue;
    }
    if ((m.order_id & kSyntheticIdBit) != 0) {
      ++outcome.exec_total;
      outcome.exec_unfilled += saw_fill ? 0U : 1U;
      outcome.exec_contra_match += saw_fill && first_contra == (m.order_id & ~kSyntheticIdBit);
    } else if (saw_fill) {
      ++outcome.crossed_add;
    }
  }
  outcome.msgs = msgs.size();
  outcome.output_hash = hasher.digest();
  return outcome;
}

}  // namespace llme::workload
