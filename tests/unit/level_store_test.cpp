// The three level stores behind one interface (FR-26): the same walk, best-price and lookup
// behaviour, checked against std::set.
#include <gtest/gtest.h>

#include <memory_resource>
#include <new>
#include <optional>
#include <set>
#include <stdexcept>
#include <vector>

#include "generator.hpp"
#include "llme/level_store_map.hpp"
#include "llme/level_store_sorted_vec.hpp"
#include "llme/price_ladder.hpp"

namespace llme {
namespace {

EngineConfig band(Price low, Price high, std::uint32_t orders = 4096) {
  return {.price_min = low, .price_max = high, .max_orders = orders};
}

template <class Store>
class LevelStoreTest : public testing::Test {
 protected:
  void occupy(Side side, Price price) { store_.level(side, price).count = 1; }
  void vacate(Side side, Price price) {
    store_.level(side, price).count = 0;
    store_.on_level_empty(side, price);
  }
  std::vector<Price> walk(Side side) const {
    std::vector<Price> prices;
    for (auto p = store_.best(side); p; p = store_.next_after(side, *p)) {
      prices.push_back(*p);
    }
    return prices;
  }

  Store store_{band(0, 9999)};
};

using Stores = testing::Types<LadderLevelStore, MapLevelStore, SortedVecLevelStore>;
TYPED_TEST_SUITE(LevelStoreTest, Stores);

TYPED_TEST(LevelStoreTest, StartsEmpty) {
  EXPECT_FALSE(this->store_.best(Side::Buy).has_value());
  EXPECT_FALSE(this->store_.best(Side::Sell).has_value());
  EXPECT_EQ(this->store_.find(Side::Buy, 5), nullptr);
  EXPECT_GT(this->store_.bytes(), 0U);
}

TYPED_TEST(LevelStoreTest, BidsWalkDownAsksWalkUp) {
  for (const Price p : {5, 900, 70}) {
    this->occupy(Side::Buy, p);
    this->occupy(Side::Sell, p);
  }
  EXPECT_EQ(this->walk(Side::Buy), (std::vector<Price>{900, 70, 5}));
  EXPECT_EQ(this->walk(Side::Sell), (std::vector<Price>{5, 70, 900}));
  this->store_.check_consistency();
}

TYPED_TEST(LevelStoreTest, LevelKeepsItsContents) {
  this->store_.level(Side::Sell, 42).qty = 17;
  this->store_.level(Side::Sell, 42).count = 2;
  this->occupy(Side::Sell, 41);
  this->occupy(Side::Sell, 43);
  const Level* level = this->store_.find(Side::Sell, 42);
  ASSERT_NE(level, nullptr);
  EXPECT_EQ(level->qty, 17U);
  EXPECT_EQ(level->count, 2U);
}

TYPED_TEST(LevelStoreTest, AgreesWithStdSet) {
  std::set<Price> bids;
  std::set<Price> asks;
  workload::Rng rng(5);
  for (int step = 0; step < 50000; ++step) {
    const Side side = rng.below(2) == 0 ? Side::Buy : Side::Sell;
    auto& model = side == Side::Buy ? bids : asks;
    const auto price = static_cast<Price>(rng.below(3000));
    if (model.contains(price)) {
      model.erase(price);
      this->vacate(side, price);
    } else {
      model.insert(price);
      this->occupy(side, price);
    }
    const std::optional<Price> want_bid =
        bids.empty() ? std::nullopt : std::optional<Price>(*bids.rbegin());
    const std::optional<Price> want_ask =
        asks.empty() ? std::nullopt : std::optional<Price>(*asks.begin());
    ASSERT_EQ(this->store_.best(Side::Buy), want_bid);
    ASSERT_EQ(this->store_.best(Side::Sell), want_ask);
    if (!asks.empty()) {
      const Price any = *asks.begin();
      const auto next = asks.upper_bound(any);
      ASSERT_EQ(this->store_.next_after(Side::Sell, any),
                next == asks.end() ? std::nullopt : std::optional<Price>(*next));
    }
  }
  this->store_.check_consistency();
}

TYPED_TEST(LevelStoreTest, CheckCatchesAnEmptyLevelLeftBehind) {
  this->occupy(Side::Buy, 10);
  this->store_.level(Side::Buy, 10).count = 0;  // emptied without on_level_empty
  EXPECT_THROW(this->store_.check_consistency(), std::logic_error);
}

TEST(FixedBlockResourceTest, RecyclesBlocksAndRefusesOversizeOrOverflow) {
  FixedBlockResource pool(2);
  void* a = pool.allocate(32, 8);
  void* b = pool.allocate(64, 8);
  EXPECT_NE(a, b);
  EXPECT_THROW((void)pool.allocate(8, 8), std::bad_alloc);
  pool.deallocate(a, 32, 8);
  EXPECT_EQ(pool.allocate(16, 8), a);
  pool.deallocate(b, 64, 8);
  EXPECT_THROW((void)pool.allocate(FixedBlockResource::kBlockBytes + 1, 8), std::bad_alloc);
  EXPECT_TRUE(pool.is_equal(pool));
  FixedBlockResource other(1);
  EXPECT_FALSE(pool.is_equal(other));
}

TEST(MapLevelStoreTest, HoldsAsManyLevelsAsTheBandAllows) {
  MapLevelStore store(band(0, 63, 1024));
  for (Price p = 0; p <= 63; ++p) {
    store.level(Side::Buy, p).count = 1;
    store.level(Side::Sell, p).count = 1;
  }
  EXPECT_EQ(store.best(Side::Buy), 63);
}

TEST(SortedVecLevelStoreTest, HoldsAsManyLevelsAsThePoolAllows) {
  SortedVecLevelStore store(band(0, 100000, 16));
  for (Price p = 0; p < 16; ++p) {
    store.level(Side::Sell, p * 1000).count = 1;
  }
  EXPECT_EQ(store.best(Side::Sell), 0);
  EXPECT_EQ(store.next_after(Side::Sell, 0), 1000);
  EXPECT_FALSE(store.next_after(Side::Sell, 15000).has_value());
}

}  // namespace
}  // namespace llme
