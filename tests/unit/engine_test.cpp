// Engine-specific behaviour. Order-type semantics are covered by the golden scenarios, which run
// on every engine; this file covers what only the fast engine has.
#include "llme/engine.hpp"

#include <gtest/gtest.h>

#include <limits>
#include <optional>
#include <stdexcept>
#include <vector>

namespace llme {

// Reaches inside the engine to corrupt it, proving that the invariant checker notices.
struct EngineTestAccess {
  template <class E>
  static OrderPool& pool(E& engine) {
    return engine.pool_;
  }
  template <class E>
  static IdMap& ids(E& engine) {
    return engine.ids_;
  }
  template <class E>
  static LadderLevelStore& levels(E& engine) {
    return engine.levels_;
  }
};

namespace {

struct CaptureSink {
  std::vector<OutMsg>* out;
  void operator()(const OutMsg& m) const noexcept { out->push_back(m); }
};

InMsg limit(OrderId id, Side side, Qty qty, Price price) {
  InMsg m;
  m.type = to_wire(MsgType::New);
  m.side = to_wire(side);
  m.tif = to_wire(Tif::Gtc);
  m.qty = qty;
  m.order_id = id;
  m.price = price;
  return m;
}

InMsg cancel(OrderId id) {
  InMsg m;
  m.type = to_wire(MsgType::Cancel);
  m.order_id = id;
  return m;
}

class EngineTest : public testing::Test {
 protected:
  using Engine = MatchingEngine<LadderLevelStore, CaptureSink>;

  EngineConfig config_{.price_min = 0, .price_max = 1000, .max_orders = 8};
  std::vector<OutMsg> out_;
  Engine engine_{config_, CaptureSink{&out_}};
};

TEST_F(EngineTest, AccessorsReflectTheBook) {
  EXPECT_FALSE(engine_.best_bid().has_value());
  EXPECT_FALSE(engine_.best_ask().has_value());
  engine_.on_message(limit(1, Side::Buy, 3, 100));
  engine_.on_message(limit(2, Side::Buy, 4, 100));
  engine_.on_message(limit(3, Side::Sell, 5, 110));
  EXPECT_EQ(engine_.best_bid(), 100);
  EXPECT_EQ(engine_.best_ask(), 110);
  EXPECT_EQ(engine_.depth_at(Side::Buy, 100), 7U);
  EXPECT_EQ(engine_.depth_at(Side::Buy, 99), 0U);
  EXPECT_EQ(engine_.live_orders(), 3U);
  engine_.check_invariants();
  engine_.check_pool();
}

TEST_F(EngineTest, CountersMatchTheOutputStream) {
  engine_.on_message(limit(1, Side::Buy, 3, 100));
  engine_.on_message(limit(2, Side::Sell, 3, 100));
  engine_.on_message(cancel(9));
  engine_.on_message(limit(3, Side::Buy, 0, 100));
  const auto c = engine_.counters();
  EXPECT_EQ(c.msgs, 4U);
  EXPECT_EQ(c.acks, 2U);
  EXPECT_EQ(c.fills, 2U);
  EXPECT_EQ(c.trades, 1U);
  EXPECT_EQ(c.cancel_rejects, 1U);
  EXPECT_EQ(c.rejects, 1U);
  EXPECT_EQ(c.level_updates, 2U);
  EXPECT_EQ(c.out_records, out_.size());
}

TEST_F(EngineTest, FootprintIsReported) { EXPECT_GT(engine_.footprint_bytes(), 0U); }

TEST(EngineFootprintTest, DefaultEngineStaysUnderNfr12) {
  const MatchingEngine<LadderLevelStore, NullSink> engine(EngineConfig{}, NullSink{});
  EXPECT_LE(engine.footprint_bytes(), std::size_t{256} << 20U);
}

TEST(EngineConfigTest, RejectsBadConfigs) {
  using Engine = MatchingEngine<LadderLevelStore, NullSink>;
  EXPECT_THROW(Engine({.price_min = 0, .price_max = 10, .max_orders = 0}, NullSink{}),
               std::invalid_argument);
  EXPECT_THROW(Engine({.price_min = 0,
                       .price_max = 10,
                       .max_orders = std::numeric_limits<std::uint32_t>::max()},
                      NullSink{}),
               std::invalid_argument);
  EXPECT_THROW(
      Engine({.price_min = 0, .price_max = 10, .max_orders = 4, .id_map_load = 0.0}, NullSink{}),
      std::invalid_argument);
  EXPECT_THROW(
      Engine({.price_min = 0, .price_max = 10, .max_orders = 4, .id_map_load = 1.5}, NullSink{}),
      std::invalid_argument);
  EXPECT_THROW(Engine({.price_min = 0,
                       .price_max = 10,
                       .max_orders = 4,
                       .id_map_load = 0.5,
                       .stp = StpMode::CancelNewest},
                      NullSink{}),
               std::invalid_argument);
}

// Every corruption below must trip the checker, or a clean --check-invariants run proves nothing.
class InvariantCheckerTest : public EngineTest {
 protected:
  void SetUp() override {
    engine_.on_message(limit(1, Side::Buy, 3, 100));
    engine_.on_message(limit(2, Side::Buy, 4, 100));
    engine_.on_message(limit(3, Side::Sell, 5, 110));
    engine_.check_invariants();
    engine_.check_pool();
  }

  Order& order_at(std::uint32_t index) { return EngineTestAccess::pool(engine_)[index]; }
  Level& level(Side side, Price price) {
    return EngineTestAccess::levels(engine_).level(side, price);
  }
  std::uint32_t head() { return level(Side::Buy, 100).head; }
  std::uint32_t tail() { return level(Side::Buy, 100).tail; }
};

TEST_F(InvariantCheckerTest, CachedQtyWrong) {
  level(Side::Buy, 100).qty += 1;
  EXPECT_THROW(engine_.check_invariants(), std::logic_error);
}

TEST_F(InvariantCheckerTest, CachedCountWrong) {
  level(Side::Buy, 100).count = 3;
  EXPECT_THROW(engine_.check_invariants(), std::logic_error);
}

TEST_F(InvariantCheckerTest, StaleTail) {
  level(Side::Buy, 100).tail = head();
  EXPECT_THROW(engine_.check_invariants(), std::logic_error);
}

TEST_F(InvariantCheckerTest, BrokenPrevLink) {
  order_at(tail()).prev = kNil;
  EXPECT_THROW(engine_.check_invariants(), std::logic_error);
}

TEST_F(InvariantCheckerTest, QueueOutOfArrivalOrder) {
  order_at(tail()).arrival = 0;
  EXPECT_THROW(engine_.check_invariants(), std::logic_error);
}

TEST_F(InvariantCheckerTest, OrderOnTheWrongLevel) {
  order_at(head()).price = 99;
  EXPECT_THROW(engine_.check_invariants(), std::logic_error);
}

TEST_F(InvariantCheckerTest, EmptyOrderOnTheBook) {
  order_at(head()).leaves = 0;
  EXPECT_THROW(engine_.check_invariants(), std::logic_error);
}

TEST_F(InvariantCheckerTest, IdMapPointsElsewhere) {
  EngineTestAccess::ids(engine_).erase(1);
  EngineTestAccess::ids(engine_).insert(1, tail());
  EXPECT_THROW(engine_.check_invariants(), std::logic_error);
}

TEST_F(InvariantCheckerTest, IdMapSizeDiffers) {
  EngineTestAccess::ids(engine_).insert(77, 5);
  EXPECT_THROW(engine_.check_counts(), std::logic_error);
}

TEST_F(InvariantCheckerTest, CycleInQueue) {
  order_at(tail()).next = head();
  EXPECT_THROW(engine_.check_invariants(), std::logic_error);
}

TEST_F(InvariantCheckerTest, CrossedBook) {
  level(Side::Sell, 90).count = 1;  // occupies a level below the bid with nothing in it
  EXPECT_THROW(engine_.check_counts(), std::logic_error);
}

TEST_F(InvariantCheckerTest, LiveCountDiffersFromLevels) {
  (void)EngineTestAccess::pool(engine_).allocate();
  EngineTestAccess::ids(engine_).insert(78, 5);
  EXPECT_THROW(engine_.check_invariants(), std::logic_error);
}

TEST_F(InvariantCheckerTest, BrokenFreeList) {
  // Orders 1-3 took slots 0-2, so the free list runs 3, 4, ..., 7; cut it short.
  EngineTestAccess::pool(engine_)[5].next = kNil;
  EXPECT_THROW(engine_.check_pool(), std::logic_error);
}

}  // namespace
}  // namespace llme
