#include "llme/reference_engine.hpp"

#include <algorithm>
#include <iterator>
#include <limits>

namespace llme {

ReferenceEngine::ReferenceEngine(const EngineConfig& cfg) : cfg_(cfg) {}

void ReferenceEngine::on_message(const InMsg& m, std::vector<OutMsg>& out) {
  out_ = &out;
  if (m.type == to_wire(MsgType::New)) {
    on_new(m);
  } else if (m.type == to_wire(MsgType::Cancel)) {
    on_cancel(m);
  } else if (m.type == to_wire(MsgType::Modify)) {
    on_modify(m);
  } else {
    emit({.type = to_wire(OutType::Reject),
          .side = m.side,
          .reason = to_wire(RejectReason::InvalidType),
          .order_id = m.order_id});
  }
  out_ = nullptr;
}

void ReferenceEngine::on_new(const InMsg& m) {
  const auto reject = [&](RejectReason reason) {
    emit({.type = to_wire(OutType::Reject),
          .side = m.side,
          .reason = to_wire(reason),
          .order_id = m.order_id});
  };
  if (m.side > to_wire(Side::Sell)) {
    return reject(RejectReason::InvalidSide);
  }
  if (m.ord_type > to_wire(OrdType::Market) || m.tif > to_wire(Tif::Fok)) {
    return reject(RejectReason::InvalidType);
  }
  if (m.qty == 0) {
    return reject(RejectReason::InvalidQty);
  }
  const bool market = m.ord_type == to_wire(OrdType::Market);
  if (!market && (m.price < cfg_.price_min || m.price > cfg_.price_max)) {
    return reject(RejectReason::PriceOutOfBand);
  }
  if (index_.contains(m.order_id)) {
    return reject(RejectReason::DuplicateId);
  }
  const bool rests = !market && m.tif == to_wire(Tif::Gtc);
  if (rests && index_.size() >= cfg_.max_orders) {
    return reject(RejectReason::PoolFull);
  }

  const auto side = static_cast<Side>(m.side);
  const Price price = market ? 0 : m.price;
  const std::optional<Price> limit = market ? std::nullopt : std::optional(m.price);
  emit({.type = to_wire(OutType::Ack),
        .side = m.side,
        .qty = m.qty,
        .order_id = m.order_id,
        .price = price,
        .leaves = m.qty});

  if (!market && m.tif == to_wire(Tif::Fok) && !fok_fillable(side, m.price, m.qty)) {
    return emit_canceled(m.order_id, side, price, m.qty, CancelReason::FokUnfillable);
  }
  const Qty leaves = match(side, m.order_id, limit, m.qty);
  if (leaves == 0) {
    return;
  }
  if (rests) {
    rest({m.order_id, side, m.price, leaves, m.trader_id});
  } else if (market && leaves == m.qty) {
    emit_canceled(m.order_id, side, price, leaves, CancelReason::NoLiquidity);
  } else {
    emit_canceled(m.order_id, side, price, leaves, CancelReason::IocRemainder);
  }
}

void ReferenceEngine::on_cancel(const InMsg& m) {
  const auto found = index_.find(m.order_id);
  if (found == index_.end()) {
    emit({.type = to_wire(OutType::CancelReject),
          .reason = to_wire(RejectReason::UnknownId),
          .order_id = m.order_id});
    return;
  }
  const Order order = *found->second;
  remove(found->second);
  emit_canceled(order.id, order.side, order.price, order.leaves, CancelReason::User);
  emit_level(order.side, order.price);
}

void ReferenceEngine::on_modify(const InMsg& m) {
  const auto found = index_.find(m.order_id);
  const auto reject = [&](RejectReason reason) {
    emit({.type = to_wire(OutType::ModifyReject),
          .reason = to_wire(reason),
          .order_id = m.order_id});
  };
  if (found == index_.end()) {
    return reject(RejectReason::UnknownId);
  }
  if (m.qty == 0) {
    return on_cancel(m);
  }
  if (m.price < cfg_.price_min || m.price > cfg_.price_max) {
    return reject(RejectReason::PriceOutOfBand);
  }

  Order order = *found->second;
  const auto modified = [&](std::uint8_t flags) {
    emit({.type = to_wire(OutType::Modified),
          .side = to_wire(order.side),
          .flags = flags,
          .qty = m.qty,
          .order_id = order.id,
          .price = m.price,
          .leaves = m.qty});
  };

  if (m.price == order.price && m.qty <= order.leaves) {
    found->second->leaves = m.qty;
    modified(0);
    if (m.qty != order.leaves) {
      emit_level(order.side, order.price);
    }
    return;
  }

  remove(found->second);
  modified(kFlagPriorityLost);
  if (m.price != order.price) {
    emit_level(order.side, order.price);
  }
  order.price = m.price;
  order.leaves = match(order.side, order.id, m.price, m.qty);
  if (order.leaves > 0) {
    rest(order);
  }
}

ReferenceEngine::Book& ReferenceEngine::book(Side side) {
  return side == Side::Buy ? bids_ : asks_;
}

const ReferenceEngine::Book& ReferenceEngine::book(Side side) const {
  return side == Side::Buy ? bids_ : asks_;
}

bool ReferenceEngine::crosses(Side side, Price limit, Price contra_price) const {
  return side == Side::Buy ? contra_price <= limit : contra_price >= limit;
}

bool ReferenceEngine::fok_fillable(Side side, Price limit, Qty qty) const {
  std::uint64_t available = 0;
  const auto scan = [&](auto level, auto end) {
    for (; level != end && available < qty && crosses(side, limit, level->first); ++level) {
      for (const Order& order : level->second) {
        available += order.leaves;
      }
    }
  };
  if (side == Side::Buy) {
    scan(asks_.begin(), asks_.end());
  } else {
    scan(bids_.rbegin(), bids_.rend());
  }
  return available >= qty;
}

Qty ReferenceEngine::match(Side side, OrderId id, std::optional<Price> limit, Qty qty) {
  const Side contra = opposite(side);
  Book& contra_book = book(contra);
  while (qty > 0 && !contra_book.empty()) {
    const auto level = contra == Side::Sell ? contra_book.begin() : std::prev(contra_book.end());
    const Price price = level->first;
    if (limit && !crosses(side, *limit, price)) {
      break;
    }
    Order& resting = level->second.front();
    const Qty fill = std::min(qty, resting.leaves);
    const OrderId resting_id = resting.id;
    resting.leaves -= fill;
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
      remove(index_.at(resting_id));
    }
    emit_level(contra, price);
  }
  return qty;
}

void ReferenceEngine::rest(const Order& order) {
  Queue& queue = book(order.side)[order.price];
  queue.push_back(order);
  index_[order.id] = std::prev(queue.end());
  emit_level(order.side, order.price);
}

void ReferenceEngine::remove(Queue::iterator it) {
  const Order order = *it;
  Book& side_book = book(order.side);
  const auto level = side_book.find(order.price);
  level->second.erase(it);
  if (level->second.empty()) {
    side_book.erase(level);
  }
  index_.erase(order.id);
}

void ReferenceEngine::emit(OutMsg r) {
  r.seq = ++seq_;
  out_->push_back(r);
}

void ReferenceEngine::emit_level(Side side, Price price) {
  const Book& side_book = book(side);
  const auto level = side_book.find(price);
  std::uint64_t aggregate = 0;
  if (level != side_book.end()) {
    for (const Order& order : level->second) {
      aggregate += order.leaves;
    }
  }
  constexpr std::uint64_t kMaxQty = std::numeric_limits<Qty>::max();
  emit({.type = to_wire(OutType::LevelUpdate),
        .side = to_wire(side),
        .flags = aggregate == 0 ? kFlagLevelRemoved : std::uint8_t{0},
        .qty = static_cast<Qty>(std::min(aggregate, kMaxQty)),
        .price = price});
}

void ReferenceEngine::emit_canceled(OrderId id, Side side, Price price, Qty qty,
                                    CancelReason reason) {
  emit({.type = to_wire(OutType::Canceled),
        .side = to_wire(side),
        .reason = to_wire(reason),
        .qty = qty,
        .order_id = id,
        .price = price});
}

BookSnapshot ReferenceEngine::snapshot() const {
  const auto copy_level = [](const auto& level) {
    SnapshotLevel out;
    out.price = level.first;
    for (const Order& order : level.second) {
      out.qty += order.leaves;
      out.orders.push_back({order.id, order.leaves, order.trader_id});
    }
    return out;
  };
  BookSnapshot snap;
  std::transform(bids_.rbegin(), bids_.rend(), std::back_inserter(snap.bids), copy_level);
  std::transform(asks_.begin(), asks_.end(), std::back_inserter(snap.asks), copy_level);
  return snap;
}

std::optional<Price> ReferenceEngine::best(Side side) const {
  const Book& side_book = book(side);
  if (side_book.empty()) {
    return std::nullopt;
  }
  return side == Side::Buy ? side_book.rbegin()->first : side_book.begin()->first;
}

std::uint32_t ReferenceEngine::live_orders() const {
  return static_cast<std::uint32_t>(index_.size());
}

}  // namespace llme
