#include "llme/price_ladder.hpp"

#include <gtest/gtest.h>

#include <map>
#include <optional>
#include <set>
#include <vector>

#include "generator.hpp"

namespace llme {
namespace {

EngineConfig band(Price low, Price high) {
  return {.price_min = low, .price_max = high, .max_orders = 16};
}

void occupy(LadderLevelStore& ladder, Side side, Price price) {
  ladder.level(side, price).count = 1;
}

void vacate(LadderLevelStore& ladder, Side side, Price price) {
  ladder.level(side, price).count = 0;
  ladder.on_level_empty(side, price);
}

std::vector<Price> walk(const LadderLevelStore& ladder, Side side) {
  std::vector<Price> prices;
  for (auto p = ladder.best(side); p; p = ladder.next_after(side, *p)) {
    prices.push_back(*p);
  }
  return prices;
}

TEST(LadderTest, EmptyLadderHasNoBest) {
  const LadderLevelStore ladder(band(0, 1000));
  EXPECT_FALSE(ladder.best(Side::Buy).has_value());
  EXPECT_FALSE(ladder.best(Side::Sell).has_value());
  EXPECT_EQ(ladder.find(Side::Buy, 5), nullptr);
}

TEST(LadderTest, BidsWalkDownAsksWalkUp) {
  LadderLevelStore ladder(band(0, 1000));
  for (const Price p : {5, 900, 70}) {
    occupy(ladder, Side::Buy, p);
    occupy(ladder, Side::Sell, p);
  }
  EXPECT_EQ(walk(ladder, Side::Buy), (std::vector<Price>{900, 70, 5}));
  EXPECT_EQ(walk(ladder, Side::Sell), (std::vector<Price>{5, 70, 900}));
}

TEST(LadderTest, NegativeBandsWork) {
  LadderLevelStore ladder(band(-100, -1));
  occupy(ladder, Side::Sell, -100);
  occupy(ladder, Side::Sell, -1);
  EXPECT_EQ(walk(ladder, Side::Sell), (std::vector<Price>{-100, -1}));
}

TEST(LadderTest, BestMovesWhenTheBestLevelEmpties) {
  LadderLevelStore ladder(band(0, 1 << 20));
  occupy(ladder, Side::Buy, 10);
  occupy(ladder, Side::Buy, 1 << 19);
  occupy(ladder, Side::Buy, (1 << 20) - 1);
  vacate(ladder, Side::Buy, (1 << 20) - 1);
  EXPECT_EQ(ladder.best(Side::Buy), 1 << 19);
  vacate(ladder, Side::Buy, 1 << 19);
  EXPECT_EQ(ladder.best(Side::Buy), 10);
  vacate(ladder, Side::Buy, 10);
  EXPECT_FALSE(ladder.best(Side::Buy).has_value());
}

TEST(LadderTest, RemovingALevelBehindTheBestKeepsTheBest) {
  LadderLevelStore ladder(band(0, 100));
  occupy(ladder, Side::Sell, 10);
  occupy(ladder, Side::Sell, 20);
  vacate(ladder, Side::Sell, 20);
  EXPECT_EQ(ladder.best(Side::Sell), 10);
  EXPECT_FALSE(ladder.next_after(Side::Sell, 10).has_value());
}

// Search crosses word and summary-word boundaries in both directions.
TEST(LadderTest, SearchesAcrossWordBoundaries) {
  const Price top = (Price{1} << 20) - 1;
  for (const Price gap : {Price{1}, Price{63}, Price{64}, Price{65}, Price{4095}, Price{4096},
                          Price{4097}, Price{70000}, top}) {
    LadderLevelStore ladder(band(0, top));
    std::vector<Price> up;
    for (Price p = 0; p <= top; p += gap) {
      occupy(ladder, Side::Sell, p);
      occupy(ladder, Side::Buy, p);
      up.push_back(p);
    }
    EXPECT_EQ(walk(ladder, Side::Sell), up);
    std::vector<Price> down(up.rbegin(), up.rend());
    EXPECT_EQ(walk(ladder, Side::Buy), down);
    ladder.check_consistency();
  }
}

TEST(LadderTest, EdgesOfTheBandAreReachable) {
  LadderLevelStore ladder(band(0, 127));
  occupy(ladder, Side::Buy, 0);
  occupy(ladder, Side::Buy, 127);
  occupy(ladder, Side::Sell, 0);
  occupy(ladder, Side::Sell, 127);
  EXPECT_FALSE(ladder.next_after(Side::Buy, 0).has_value());
  EXPECT_FALSE(ladder.next_after(Side::Sell, 127).has_value());
  EXPECT_EQ(ladder.next_after(Side::Buy, 127), 0);
  EXPECT_EQ(ladder.next_after(Side::Sell, 0), 127);
}

TEST(LadderTest, SingleLevelBand) {
  LadderLevelStore ladder(band(7, 7));
  occupy(ladder, Side::Buy, 7);
  EXPECT_EQ(ladder.best(Side::Buy), 7);
  EXPECT_FALSE(ladder.next_after(Side::Buy, 7).has_value());
  vacate(ladder, Side::Buy, 7);
  EXPECT_FALSE(ladder.best(Side::Buy).has_value());
}

TEST(LadderTest, AgreesWithStdSet) {
  LadderLevelStore ladder(band(0, 9999));
  std::set<Price> model;
  workload::Rng rng(3);
  for (int step = 0; step < 100000; ++step) {
    const auto price = static_cast<Price>(rng.below(10000));
    if (model.contains(price)) {
      model.erase(price);
      vacate(ladder, Side::Sell, price);
    } else {
      model.insert(price);
      occupy(ladder, Side::Sell, price);
    }
    ASSERT_EQ(ladder.best(Side::Sell),
              model.empty() ? std::nullopt : std::optional<Price>(*model.begin()));
    const auto probe = static_cast<Price>(rng.below(10000));
    const auto next = model.upper_bound(probe);
    ASSERT_EQ(ladder.next_after(Side::Sell, probe),
              next == model.end() ? std::nullopt : std::optional<Price>(*next));
  }
  ladder.check_consistency();
}

TEST(LadderTest, RejectsBadBands) {
  EXPECT_THROW(LadderLevelStore(band(5, 4)), std::invalid_argument);
  EXPECT_THROW(LadderLevelStore(band(0, Price{1} << 28U)), std::invalid_argument);
}

TEST(LadderTest, ConsistencyCheckCatchesCorruption) {
  LadderLevelStore ladder(band(0, 100));
  occupy(ladder, Side::Buy, 10);
  ladder.check_consistency();
  ladder.level(Side::Buy, 10).count = 0;  // occupied bit left behind
  EXPECT_THROW(ladder.check_consistency(), std::logic_error);
}

}  // namespace
}  // namespace llme
