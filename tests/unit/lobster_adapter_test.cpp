#include "lobster_adapter.hpp"

#include <gtest/gtest.h>

#include <string>
#include <vector>

namespace llme::workload {
namespace {

TEST(LobsterParseTest, ReadsAMessageLine) {
  LobsterEvent event;
  ASSERT_TRUE(parse_lobster_message("34200.004241176,1,16113575,18,5853300,1", event));
  EXPECT_EQ(event.type, 1);
  EXPECT_EQ(event.order_id, 16113575);
  EXPECT_EQ(event.size, 18);
  EXPECT_EQ(event.price, 5853300);
  EXPECT_EQ(event.direction, 1);
}

TEST(LobsterParseTest, AcceptsNegativeDirectionAndCarriageReturn) {
  LobsterEvent event;
  ASSERT_TRUE(parse_lobster_message("34200.01399412,3,16085616,100,310400,-1\r", event));
  EXPECT_EQ(event.direction, -1);
}

TEST(LobsterParseTest, RefusesMalformedLines) {
  LobsterEvent event;
  for (const char* line :
       {"", ",", "34200.0,1,2,3,4", "34200.0,1,2,3,4,1,9", "abc,1,2,3,4,1", "34200.0,x,2,3,4,1",
        "34200.0,1,,3,4,1", "34200.0,1,99999999999999999999,3,4,1", "-1.0,1,2,3,4,1"}) {
    EXPECT_FALSE(parse_lobster_message(line, event)) << line;
  }
}

class LobsterApplyTest : public testing::Test {
 protected:
  bool apply(int type, std::int64_t id, std::int64_t size, std::int64_t price, int direction) {
    return apply_lobster_event({type, id, size, price, direction}, builder_);
  }

  std::vector<InMsg> msgs_;
  ReplayBuilder builder_{[this](const InMsg& m) { msgs_.push_back(m); }};
};

TEST_F(LobsterApplyTest, MapsEachEventType) {
  EXPECT_TRUE(apply(1, 5, 100, 5853300, 1));
  EXPECT_EQ(msgs_.back().type, to_wire(MsgType::New));
  EXPECT_EQ(msgs_.back().side, to_wire(Side::Buy));
  EXPECT_EQ(msgs_.back().price, 58533);

  EXPECT_TRUE(apply(2, 5, 30, 5853300, 1));
  EXPECT_EQ(msgs_.back().type, to_wire(MsgType::Modify));
  EXPECT_EQ(msgs_.back().qty, 70U);

  EXPECT_TRUE(apply(4, 5, 20, 5853300, 1));
  EXPECT_EQ(msgs_.back().tif, to_wire(Tif::Ioc));
  EXPECT_EQ(msgs_.back().side, to_wire(Side::Sell));
  EXPECT_EQ(msgs_.back().order_id, 5 | kSyntheticIdBit);

  EXPECT_TRUE(apply(3, 5, 50, 5853300, 1));
  EXPECT_EQ(msgs_.back().type, to_wire(MsgType::Cancel));
}

TEST_F(LobsterApplyTest, SellDirection) {
  EXPECT_TRUE(apply(1, 5, 100, 5853300, -1));
  EXPECT_EQ(msgs_.back().side, to_wire(Side::Sell));
}

TEST_F(LobsterApplyTest, SkipsHiddenCrossAndHaltEvents) {
  EXPECT_TRUE(apply(5, 0, 100, 5853300, -1));
  EXPECT_TRUE(apply(6, 0, 100, 5853300, -1));
  EXPECT_TRUE(apply(7, 0, 0, -1, -1));
  EXPECT_TRUE(msgs_.empty());
  EXPECT_EQ(builder_.stats().skipped_hidden, 2U);
  EXPECT_EQ(builder_.stats().skipped_halt, 1U);
}

TEST_F(LobsterApplyTest, RefusesWhatItCannotPlace) {
  EXPECT_FALSE(apply(8, 5, 100, 5853300, 1));
  EXPECT_FALSE(apply(1, 5, 100, 5853300, 0));
  EXPECT_FALSE(apply(1, -5, 100, 5853300, 1));
  EXPECT_TRUE(msgs_.empty());
}

TEST(LobsterTopTest, ReadsTheFirstLevel) {
  TopOfBook top;
  ASSERT_TRUE(parse_lobster_top("5859400,200,5853300,18,5859800,200,5853000,150", top));
  EXPECT_EQ(top.ask_price, 5859400);
  EXPECT_EQ(top.ask_size, 200);
  EXPECT_EQ(top.bid_price, 5853300);
  EXPECT_EQ(top.bid_size, 18);
}

TEST(LobsterTopTest, ReadsDummyPricesAndRefusesShortLines) {
  TopOfBook top;
  ASSERT_TRUE(parse_lobster_top("9999999999,0,-9999999999,0\r", top));
  EXPECT_EQ(top.bid_price, -9999999999);
  EXPECT_FALSE(parse_lobster_top("1,2,3", top));
  EXPECT_FALSE(parse_lobster_top("", top));
}

}  // namespace
}  // namespace llme::workload
