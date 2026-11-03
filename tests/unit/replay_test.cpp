#include "replay.hpp"

#include <gtest/gtest.h>

#include <vector>

namespace llme::workload {
namespace {

class ReplayBuilderTest : public testing::Test {
 protected:
  std::vector<InMsg> msgs_;
  ReplayBuilder builder_{[this](const InMsg& m) { msgs_.push_back(m); }};
};

TEST_F(ReplayBuilderTest, AddBecomesNewGtcAtEngineTicks) {
  builder_.add(7, Side::Sell, 1234500, 100);
  ASSERT_EQ(msgs_.size(), 1U);
  EXPECT_EQ(msgs_[0].type, to_wire(MsgType::New));
  EXPECT_EQ(msgs_[0].side, to_wire(Side::Sell));
  EXPECT_EQ(msgs_[0].tif, to_wire(Tif::Gtc));
  EXPECT_EQ(msgs_[0].ord_type, to_wire(OrdType::Limit));
  EXPECT_EQ(msgs_[0].price, 12345);
  EXPECT_EQ(msgs_[0].qty, 100U);
  EXPECT_EQ(msgs_[0].order_id, 7U);
}

TEST_F(ReplayBuilderTest, ExecBecomesOppositeIocCarryingTheContra) {
  builder_.add(7, Side::Sell, 1000000, 10);
  builder_.exec(7, 4);
  ASSERT_EQ(msgs_.size(), 2U);
  EXPECT_EQ(msgs_[1].side, to_wire(Side::Buy));
  EXPECT_EQ(msgs_[1].tif, to_wire(Tif::Ioc));
  EXPECT_EQ(msgs_[1].price, 10000);
  EXPECT_EQ(msgs_[1].qty, 4U);
  EXPECT_EQ(msgs_[1].order_id, 7 | kSyntheticIdBit);
}

TEST_F(ReplayBuilderTest, ExecNeverExceedsTheLeaves) {
  builder_.add(7, Side::Buy, 1000000, 10);
  builder_.exec(7, 25);
  EXPECT_EQ(msgs_[1].qty, 10U);
  builder_.exec(7, 1);  // the order finished above
  EXPECT_EQ(msgs_.size(), 2U);
  EXPECT_EQ(builder_.stats().dropped_unknown_id, 1U);
}

TEST_F(ReplayBuilderTest, PartialCancelBecomesModifyToNewLeaves) {
  builder_.add(7, Side::Buy, 1000000, 10);
  builder_.partial_cancel(7, 4);
  EXPECT_EQ(msgs_[1].type, to_wire(MsgType::Modify));
  EXPECT_EQ(msgs_[1].qty, 6U);
  EXPECT_EQ(msgs_[1].price, 10000);
  builder_.partial_cancel(7, 6);
  EXPECT_EQ(msgs_[2].qty, 0U);  // a modify to zero is a cancel
  builder_.remove(7);
  EXPECT_EQ(builder_.stats().dropped_unknown_id, 1U);
}

TEST_F(ReplayBuilderTest, ReplaceIsCancelThenNewOnTheSameSide) {
  builder_.add(7, Side::Sell, 1000000, 10);
  builder_.replace(7, 8, 1010000, 5);
  ASSERT_EQ(msgs_.size(), 3U);
  EXPECT_EQ(msgs_[1].type, to_wire(MsgType::Cancel));
  EXPECT_EQ(msgs_[1].order_id, 7U);
  EXPECT_EQ(msgs_[2].type, to_wire(MsgType::New));
  EXPECT_EQ(msgs_[2].side, to_wire(Side::Sell));
  EXPECT_EQ(msgs_[2].order_id, 8U);
  EXPECT_EQ(msgs_[2].price, 10100);
  EXPECT_EQ(builder_.stats().adds, 1U);
  EXPECT_EQ(builder_.stats().replaces, 1U);
  EXPECT_EQ(builder_.stats().events, 2U);
}

TEST_F(ReplayBuilderTest, CrossExecReducesWithoutMatching) {
  builder_.add(7, Side::Buy, 1000000, 10);
  builder_.cross_exec(7, 4);
  EXPECT_EQ(msgs_[1].type, to_wire(MsgType::Modify));
  EXPECT_EQ(msgs_[1].qty, 6U);
  EXPECT_EQ(builder_.stats().skipped_cross, 1U);
}

TEST_F(ReplayBuilderTest, InexactPricesAreDroppedNeverRounded) {
  builder_.add(7, Side::Buy, 1000050, 10);
  EXPECT_TRUE(msgs_.empty());
  EXPECT_EQ(builder_.stats().dropped_price, 1U);
  builder_.remove(7);
  EXPECT_EQ(builder_.stats().dropped_unknown_id, 1U);
}

TEST_F(ReplayBuilderTest, BadQuantitiesAreDropped) {
  builder_.add(1, Side::Buy, 1000000, 0);
  builder_.add(2, Side::Buy, 1000000, -5);
  builder_.add(3, Side::Buy, 1000000, std::int64_t{1} << 32U);
  builder_.add(4, Side::Buy, 1000000, 10);
  builder_.exec(4, 0);
  builder_.partial_cancel(4, -1);
  builder_.cross_exec(4, 0);
  EXPECT_EQ(msgs_.size(), 1U);
  EXPECT_EQ(builder_.stats().dropped_qty, 6U);
}

TEST_F(ReplayBuilderTest, UnknownIdsAreDropped) {
  builder_.exec(1, 1);
  builder_.partial_cancel(1, 1);
  builder_.remove(1);
  builder_.replace(1, 2, 1000000, 1);
  builder_.cross_exec(1, 1);
  EXPECT_TRUE(msgs_.empty());
  EXPECT_EQ(builder_.stats().dropped_unknown_id, 5U);
}

TEST_F(ReplayBuilderTest, ReservedTopBitIsRefused) {
  builder_.add(kSyntheticIdBit | 1, Side::Buy, 1000000, 1);
  EXPECT_TRUE(msgs_.empty());
}

TEST_F(ReplayBuilderTest, TracksBandAndLiveOrders) {
  builder_.add(1, Side::Buy, 1000000, 1);
  builder_.add(2, Side::Sell, 2000000, 1);
  builder_.add(3, Side::Sell, 3000000, 1);
  builder_.remove(3);
  const auto& stats = builder_.stats();
  EXPECT_EQ(stats.min_ticks, 10000);
  EXPECT_EQ(stats.max_ticks, 30000);
  EXPECT_EQ(stats.max_live_orders, 3U);
  EXPECT_EQ(stats.messages(), 4U);
}

TEST(ReplayConfigTest, FollowsDq07) {
  ConversionStats stats;
  stats.min_ticks = 10000;
  stats.max_ticks = 20000;
  stats.max_live_orders = 1000;
  const auto config = replay_config(stats);
  EXPECT_EQ(config.price_min, 8999);
  EXPECT_EQ(config.price_max, 21001);
  EXPECT_EQ(config.max_orders, 2048U);
}

TEST(ReplayConfigTest, BandNeverGoesNegative) {
  ConversionStats stats;
  stats.min_ticks = 1;
  stats.max_ticks = 1000;
  stats.max_live_orders = 1;
  EXPECT_EQ(replay_config(stats).price_min, 0);
}

TEST(ReplayLogTest, ReportsContraMatchesAndCrossedAdds) {
  std::vector<InMsg> msgs;
  ReplayBuilder builder([&](const InMsg& m) { msgs.push_back(m); });
  builder.add(1, Side::Sell, 1000000, 10);
  builder.add(2, Side::Sell, 1000000, 10);
  builder.exec(1, 3);                     // hits order 1, as the feed said
  builder.exec(2, 3);                     // the feed says order 2, but order 1 is at the head here
  builder.add(3, Side::Buy, 1000000, 1);  // crosses the book
  builder.add(4, Side::Buy, 900000, 1);
  builder.exec(4, 1);
  builder.remove(9);

  EngineConfig config = replay_config(builder.stats());
  auto outcome = replay_log(msgs, config);
  EXPECT_EQ(outcome.msgs, msgs.size());
  EXPECT_EQ(outcome.exec_total, 3U);
  EXPECT_EQ(outcome.exec_contra_match, 2U);
  EXPECT_EQ(outcome.crossed_add, 1U);
  EXPECT_DOUBLE_EQ(outcome.exec_contra_match_rate(), 2.0 / 3.0);
  EXPECT_NE(outcome.output_hash, 0U);
}

TEST(ReplayLogTest, CountsRejects) {
  std::vector<InMsg> msgs;
  ReplayBuilder builder([&](const InMsg& m) { msgs.push_back(m); });
  builder.add(1, Side::Buy, 1000000, 1);
  builder.add(2, Side::Buy, 1000000, 1);
  builder.add(3, Side::Sell, 90000000, 1);
  InMsg stale_cancel;
  stale_cancel.type = to_wire(MsgType::Cancel);
  stale_cancel.order_id = 99;
  InMsg stale_modify = stale_cancel;
  stale_modify.type = to_wire(MsgType::Modify);
  stale_modify.qty = 1;
  msgs.push_back(stale_cancel);
  msgs.push_back(stale_modify);

  const auto outcome =
      replay_log(msgs, EngineConfig{.price_min = 0, .price_max = 20000, .max_orders = 1});
  EXPECT_EQ(outcome.rejects, 2U);
  EXPECT_EQ(outcome.pool_full, 1U);
  EXPECT_EQ(outcome.price_out_of_band, 1U);
  EXPECT_EQ(outcome.cancel_rejects, 1U);
  EXPECT_EQ(outcome.modify_rejects, 1U);
}

TEST(ReplayLogTest, EmptyLogHasNoRate) {
  const auto outcome = replay_log({}, EngineConfig{.price_max = 15, .max_orders = 4});
  EXPECT_DOUBLE_EQ(outcome.exec_contra_match_rate(), 0.0);
}

}  // namespace
}  // namespace llme::workload
