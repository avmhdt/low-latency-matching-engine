// Reference engine: the semantics of docs/design.md in standard containers, with no performance
// work at all, so that it can be read and checked by eye (FR-16). The fast engines are compared
// against it record by record.
//
// Invariants: the book is never crossed after a message; every live order is in exactly one
// queue and in index_. Thread-safety: an instance belongs to one thread.
#pragma once

#include <cstdint>
#include <list>
#include <map>
#include <optional>
#include <unordered_map>
#include <vector>

#include "llme/messages.hpp"
#include "llme/types.hpp"

namespace llme {

class ReferenceEngine {
 public:
  explicit ReferenceEngine(const EngineConfig& cfg);

  // Appends this message's output records to out.
  void on_message(const InMsg& m, std::vector<OutMsg>& out);

  [[nodiscard]] BookSnapshot snapshot() const;
  [[nodiscard]] std::optional<Price> best(Side side) const;
  [[nodiscard]] std::uint32_t live_orders() const;

 private:
  struct Order {
    OrderId id = 0;
    Side side = Side::Buy;
    Price price = 0;
    Qty leaves = 0;
    std::uint32_t trader_id = 0;
  };
  using Queue = std::list<Order>;
  using Book = std::map<Price, Queue>;

  void on_new(const InMsg& m);
  void on_cancel(const InMsg& m);
  void on_modify(const InMsg& m);

  [[nodiscard]] Book& book(Side side);
  [[nodiscard]] const Book& book(Side side) const;
  [[nodiscard]] bool crosses(Side side, Price limit, Price contra_price) const;
  [[nodiscard]] bool fok_fillable(Side side, Price limit, Qty qty) const;
  Qty match(Side side, OrderId id, std::optional<Price> limit, Qty qty);
  void rest(const Order& order);
  void remove(Queue::iterator it);

  void emit(OutMsg r);
  void emit_level(Side side, Price price);
  void emit_canceled(OrderId id, Side side, Price price, Qty qty, CancelReason reason);

  EngineConfig cfg_;
  Book bids_;
  Book asks_;
  std::unordered_map<OrderId, Queue::iterator> index_;
  std::uint64_t seq_ = 0;
  std::vector<OutMsg>* out_ = nullptr;
};

}  // namespace llme
