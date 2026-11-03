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

TEST(LobsterRowTest, ReadsEveryColumnAndRefusesPartialLevels) {
  std::vector<std::int64_t> row;
  ASSERT_TRUE(parse_lobster_row("11,1,9,2,12,3,8,4\r", row));
  EXPECT_EQ(row, (std::vector<std::int64_t>{11, 1, 9, 2, 12, 3, 8, 4}));
  EXPECT_FALSE(parse_lobster_row("11,1,9", row));
  EXPECT_FALSE(parse_lobster_row("", row));
  EXPECT_FALSE(parse_lobster_row("a,1,9,2", row));
}

class OneStepTest : public testing::Test {
 protected:
  // A row is: ask price, ask size, bid price, bid size, then the second level.
  const std::vector<std::int64_t> before_{10200, 5, 10000, 7, 10300, 2, 9900, 3};
  OneStepChecker checker_{2000};

  void expect(const LobsterEvent& event, const std::vector<std::int64_t>& after, bool agrees) {
    checker_.check(before_, &event, after);
    EXPECT_EQ(checker_.result().agree, agrees ? 1U : 0U);
    EXPECT_EQ(checker_.result().compared, 1U);
  }
};

TEST_F(OneStepTest, AddThatImprovesTheBid) {
  expect({1, 9, 4, 10100, 1}, {10200, 5, 10100, 4, 10300, 2, 10000, 7}, true);
}

TEST_F(OneStepTest, ExecutionTakesFromTheTouch) {
  expect({4, 9, 3, 10000, 1}, {10200, 5, 10000, 4, 10300, 2, 9900, 3}, true);
}

TEST_F(OneStepTest, ExecutionThatEmptiesTheTouchUncoversTheNextLevel) {
  expect({4, 9, 5, 10200, -1}, {10300, 2, 10000, 7, 0, 0, 9900, 3}, true);
}

TEST_F(OneStepTest, CancelAndDeleteReduceTheLevel) {
  expect({2, 9, 2, 10200, -1}, {10200, 3, 10000, 7, 10300, 2, 9900, 3}, true);
}

TEST_F(OneStepTest, DeleteOfTheWholeLevelUncoversTheNext) {
  expect({3, 9, 7, 10000, 1}, {10200, 5, 9900, 3, 10300, 2, 0, 0}, true);
}

TEST_F(OneStepTest, HiddenExecutionChangesNothing) { expect({5, 9, 1, 10100, -1}, before_, true); }

TEST_F(OneStepTest, EventOutsideTheVisibleLevelsIsCounted) {
  expect({3, 9, 1, 9000, 1}, before_, true);
  EXPECT_EQ(checker_.result().events_outside_window, 1U);
}

TEST_F(OneStepTest, AWrongOutcomeIsNoticed) { expect({1, 9, 4, 10100, 1}, before_, false); }

TEST_F(OneStepTest, ReusesTheBookFromLineToLine) {
  checker_.check(before_, nullptr, before_);
  checker_.check(before_, nullptr, before_);
  EXPECT_EQ(checker_.result().agree, 2U);
}

}  // namespace
}  // namespace llme::workload
