#include "llme/reference_engine.hpp"

#include <gtest/gtest.h>

#include <vector>

namespace llme {
namespace {

InMsg new_order(OrderId id, Side side, Tif tif, Qty qty, Price price) {
  InMsg m;
  m.type = to_wire(MsgType::New);
  m.side = to_wire(side);
  m.ord_type = to_wire(OrdType::Limit);
  m.tif = to_wire(tif);
  m.qty = qty;
  m.order_id = id;
  m.price = price;
  return m;
}

class ReferenceEngineTest : public testing::Test {
 protected:
  std::vector<OutMsg> send(const InMsg& m) {
    std::vector<OutMsg> out;
    engine_.on_message(m, out);
    return out;
  }

  ReferenceEngine engine_{EngineConfig{.price_min = 0, .price_max = 1000, .max_orders = 8}};
};

TEST_F(ReferenceEngineTest, SequenceIsGapFreeAcrossMessages) {
  std::uint64_t expected = 1;
  for (const auto& m :
       {new_order(1, Side::Buy, Tif::Gtc, 1, 100), new_order(2, Side::Sell, Tif::Gtc, 1, 100),
        new_order(3, Side::Sell, Tif::Gtc, 0, 100)}) {
    for (const auto& r : send(m)) {
      EXPECT_EQ(r.seq, expected++);
    }
  }
  EXPECT_EQ(expected, 9U);
}

TEST_F(ReferenceEngineTest, BestAndSnapshotOrderLevelsBestFirst) {
  send(new_order(1, Side::Buy, Tif::Gtc, 1, 100));
  send(new_order(2, Side::Buy, Tif::Gtc, 2, 101));
  send(new_order(3, Side::Sell, Tif::Gtc, 3, 110));
  send(new_order(4, Side::Sell, Tif::Gtc, 4, 105));
  EXPECT_EQ(engine_.best(Side::Buy), 101);
  EXPECT_EQ(engine_.best(Side::Sell), 105);
  const auto snap = engine_.snapshot();
  ASSERT_EQ(snap.bids.size(), 2U);
  ASSERT_EQ(snap.asks.size(), 2U);
  EXPECT_EQ(snap.bids[0].price, 101);
  EXPECT_EQ(snap.bids[1].price, 100);
  EXPECT_EQ(snap.asks[0].price, 105);
  EXPECT_EQ(snap.asks[1].price, 110);
  EXPECT_EQ(snap.asks[1].orders[0].id, 3U);
}

TEST_F(ReferenceEngineTest, EmptySideHasNoBest) {
  EXPECT_FALSE(engine_.best(Side::Buy).has_value());
  EXPECT_FALSE(engine_.best(Side::Sell).has_value());
  EXPECT_EQ(engine_.live_orders(), 0U);
}

TEST_F(ReferenceEngineTest, FokFillsAgainstBidsWhenSelling) {
  send(new_order(1, Side::Buy, Tif::Gtc, 2, 100));
  send(new_order(2, Side::Buy, Tif::Gtc, 2, 99));
  const auto out = send(new_order(3, Side::Sell, Tif::Fok, 3, 99));
  EXPECT_EQ(engine_.live_orders(), 1U);
  EXPECT_EQ(out.back().type, to_wire(OutType::LevelUpdate));
  EXPECT_EQ(engine_.snapshot().bids[0].qty, 1U);
}

}  // namespace
}  // namespace llme
