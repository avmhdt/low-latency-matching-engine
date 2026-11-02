// Seeded properties of the reference engine, 100k ops per seed over 20 seeds (spec section 11).
#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <map>
#include <optional>
#include <vector>

#include "generator.hpp"
#include "llme/reference_engine.hpp"

namespace llme {
namespace {

constexpr std::uint64_t kOpsPerSeed = 100000;

workload::Profile test_profile() {
  auto profile = workload::load_profile("configs/workloads/adversarial.toml");
  profile.mix.invalid = 0.02;
  profile.mix.new_limit = 0.36;
  return profile;
}

class ReferencePropertyTest : public testing::TestWithParam<std::uint64_t> {};

TEST_P(ReferencePropertyTest, BookStaysConsistent) {
  const auto profile = test_profile();
  workload::Generator generator(profile, GetParam());
  ReferenceEngine engine(profile.engine);
  std::vector<OutMsg> out;
  std::uint64_t seq = 0;

  for (std::uint64_t i = 0; i < kOpsPerSeed; ++i) {
    const InMsg m = generator.next();
    out.clear();
    engine.on_message(m, out);

    std::uint64_t buy_fills = 0;
    std::uint64_t sell_fills = 0;
    for (const OutMsg& r : out) {
      ASSERT_EQ(r.seq, ++seq);
      ASSERT_EQ(r.reserved, 0U);
      if (r.type == to_wire(OutType::Fill)) {
        (r.side == to_wire(Side::Buy) ? buy_fills : sell_fills) += r.qty;
      }
    }
    ASSERT_EQ(buy_fills, sell_fills) << "fill conservation at message " << i;

    const auto bid = engine.best(Side::Buy);
    const auto ask = engine.best(Side::Sell);
    if (bid && ask) {
      ASSERT_LT(*bid, *ask) << "crossed at message " << i;
    }
    ASSERT_LE(engine.live_orders(), profile.engine.max_orders);
  }
}

TEST_P(ReferencePropertyTest, CancelAfterAddRestoresTheSnapshot) {
  const auto profile = test_profile();
  workload::Generator generator(profile, GetParam());
  ReferenceEngine engine(profile.engine);
  std::vector<OutMsg> out;

  for (std::uint64_t i = 0; i < 2000; ++i) {
    engine.on_message(generator.next(), out);
    const auto bid = engine.best(Side::Buy);
    const Price passive = bid.value_or(profile.engine.price_min + 1) - 1;
    if (passive < profile.engine.price_min) {
      continue;
    }
    const auto before = engine.snapshot();
    InMsg add;
    add.type = to_wire(MsgType::New);
    add.side = to_wire(Side::Buy);
    add.qty = 1;
    add.order_id = (std::uint64_t{1} << 62U) + i;
    add.price = passive;
    if (engine.live_orders() == profile.engine.max_orders) {
      continue;
    }
    engine.on_message(add, out);
    InMsg cancel;
    cancel.type = to_wire(MsgType::Cancel);
    cancel.order_id = add.order_id;
    engine.on_message(cancel, out);
    ASSERT_EQ(engine.snapshot(), before) << "at message " << i;
  }
}

TEST_P(ReferencePropertyTest, ModifyDownKeepsQueuePosition) {
  const auto profile = test_profile();
  workload::Generator generator(profile, GetParam());
  ReferenceEngine engine(profile.engine);
  std::vector<OutMsg> out;

  for (std::uint64_t i = 0; i < 2000; ++i) {
    engine.on_message(generator.next(), out);
    const auto before = engine.snapshot();
    for (const auto& level : before.bids) {
      const auto big = std::find_if(level.orders.begin(), level.orders.end(),
                                    [](const SnapshotOrder& o) { return o.leaves > 1; });
      if (big == level.orders.end()) {
        continue;
      }
      InMsg modify;
      modify.type = to_wire(MsgType::Modify);
      modify.order_id = big->id;
      modify.qty = big->leaves - 1;
      modify.price = level.price;
      engine.on_message(modify, out);

      const auto after = engine.snapshot();
      const auto now = std::find_if(after.bids.begin(), after.bids.end(),
                                    [&](const auto& l) { return l.price == level.price; });
      ASSERT_NE(now, after.bids.end());
      ASSERT_EQ(now->orders.size(), level.orders.size());
      for (std::size_t q = 0; q < level.orders.size(); ++q) {
        ASSERT_EQ(now->orders[q].id, level.orders[q].id) << "queue moved at message " << i;
      }
      break;
    }
  }
}

TEST_P(ReferencePropertyTest, FokFillsFullyOrLeavesTheBookUnchanged) {
  const auto profile = test_profile();
  workload::Generator generator(profile, GetParam());
  ReferenceEngine engine(profile.engine);
  std::vector<OutMsg> out;
  std::uint64_t filled = 0;
  std::uint64_t killed = 0;

  for (std::uint64_t i = 0; i < 20000; ++i) {
    const InMsg m = generator.next();
    const bool is_fok = m.type == to_wire(MsgType::New) && m.ord_type == 0 &&
                        m.tif == to_wire(Tif::Fok) && m.side <= 1 && m.qty != 0;
    const auto before = is_fok ? std::optional(engine.snapshot()) : std::nullopt;
    out.clear();
    engine.on_message(m, out);
    if (!before || out.size() < 2 || out[0].type != to_wire(OutType::Ack)) {
      continue;
    }
    if (out[1].type == to_wire(OutType::Canceled)) {
      ASSERT_EQ(out[1].reason, to_wire(CancelReason::FokUnfillable));
      ASSERT_EQ(out.size(), 2U);
      ASSERT_EQ(engine.snapshot(), *before);
      ++killed;
    } else {
      std::uint64_t qty = 0;
      for (const OutMsg& r : out) {
        if (r.type == to_wire(OutType::Trade)) {
          qty += r.qty;
        }
      }
      ASSERT_EQ(qty, m.qty);
      ++filled;
    }
  }
  EXPECT_GT(filled, 0U);
  EXPECT_GT(killed, 0U);
}

INSTANTIATE_TEST_SUITE_P(Seeds, ReferencePropertyTest, testing::Range<std::uint64_t>(1, 21));

}  // namespace
}  // namespace llme
