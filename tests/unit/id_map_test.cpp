#include "llme/id_map.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <unordered_map>
#include <vector>

#include "generator.hpp"

namespace llme {
namespace {

TEST(IdMapTest, CapacityIsPowerOfTwoAboveEntriesOverLoad) {
  EXPECT_EQ(IdMap(1024, 0.5).capacity(), 4096U);
  EXPECT_EQ(IdMap(1000, 0.5).capacity(), 2048U);
  EXPECT_EQ(IdMap(1, 0.5).capacity(), 4U);
  EXPECT_EQ(IdMap(4, 1.0).capacity(), 8U);
  EXPECT_GE(IdMap(1048576, 0.5).capacity(), 2U * 1048576U);
}

TEST(IdMapTest, FindsWhatWasInserted) {
  IdMap map(16, 0.5);
  EXPECT_EQ(map.find(7), kNil);
  map.insert(7, 70);
  map.insert(8, 80);
  EXPECT_EQ(map.find(7), 70U);
  EXPECT_EQ(map.find(8), 80U);
  EXPECT_EQ(map.find(9), kNil);
  EXPECT_EQ(map.size(), 2U);
}

TEST(IdMapTest, KeyZeroIsAnOrdinaryKey) {
  IdMap map(16, 0.5);
  EXPECT_EQ(map.find(0), kNil);
  map.insert(0, 5);
  EXPECT_EQ(map.find(0), 5U);
  map.erase(0);
  EXPECT_EQ(map.find(0), kNil);
}

TEST(IdMapTest, LargestKeyWorks) {
  IdMap map(16, 0.5);
  map.insert(~std::uint64_t{0}, 1);
  EXPECT_EQ(map.find(~std::uint64_t{0}), 1U);
}

// Full table of colliding entries: every key probes through the wrap at the end of the array.
TEST(IdMapTest, ProbeStatsMeasureDistanceFromHome) {
  IdMap map(7, 1.0);
  EXPECT_EQ(map.probe_stats().entries, 0U);
  std::vector<OrderId> keys;
  for (OrderId key = 1; keys.size() < 3; ++key) {
    if ((key * 0x9E3779B97F4A7C15ULL) >> 61U == 2) {
      keys.push_back(key);
    }
  }
  for (std::uint32_t i = 0; i < keys.size(); ++i) {
    map.insert(keys[i], i);
  }
  // Three keys with one home slot sit at distances 0, 1 and 2.
  const auto stats = map.probe_stats();
  EXPECT_EQ(stats.entries, 3U);
  EXPECT_EQ(stats.max, 2U);
  EXPECT_DOUBLE_EQ(stats.mean, 1.0);
}

TEST(IdMapTest, BackwardShiftAcrossWrapAround) {
  IdMap map(7, 1.0);
  ASSERT_EQ(map.capacity(), 8U);
  std::vector<OrderId> keys;
  // Fibonacci hash of the key, top 3 bits; collect keys that all land on the last slot.
  for (OrderId key = 1; keys.size() < 7; ++key) {
    if ((key * 0x9E3779B97F4A7C15ULL) >> 61U == 7) {
      keys.push_back(key);
    }
  }
  for (std::uint32_t i = 0; i < keys.size(); ++i) {
    map.insert(keys[i], i);
  }
  for (std::uint32_t i = 0; i < keys.size(); ++i) {
    EXPECT_EQ(map.find(keys[i]), i);
  }
  // Erase from the front of the cluster: later entries must shift back past the wrap.
  map.erase(keys[0]);
  EXPECT_EQ(map.find(keys[0]), kNil);
  for (std::uint32_t i = 1; i < keys.size(); ++i) {
    EXPECT_EQ(map.find(keys[i]), i) << "lost after shift";
  }
  map.erase(keys[3]);
  for (std::uint32_t i = 1; i < keys.size(); ++i) {
    EXPECT_EQ(map.find(keys[i]), i == 3 ? kNil : i);
  }
}

// Random workload against std::unordered_map at a load high enough for long clusters.
TEST(IdMapTest, AgreesWithStdUnorderedMap) {
  IdMap map(900, 0.9);
  std::unordered_map<OrderId, std::uint32_t> model;
  std::vector<OrderId> keys;
  workload::Rng rng(11);
  for (std::uint32_t step = 0; step < 300000; ++step) {
    const bool may_insert = model.size() < 900;
    if (may_insert && (keys.empty() || rng.below(2) == 0)) {
      const OrderId key = rng.below(1U << 14U);
      if (!model.contains(key)) {
        map.insert(key, step);
        model[key] = step;
        keys.push_back(key);
      }
    } else if (!keys.empty()) {
      const std::size_t pick = rng.below(keys.size());
      const OrderId key = keys[pick];
      map.erase(key);
      model.erase(key);
      keys[pick] = keys.back();
      keys.pop_back();
    }
    ASSERT_EQ(map.size(), model.size());
    const OrderId probe = rng.below(1U << 14U);
    const auto found = model.find(probe);
    ASSERT_EQ(map.find(probe), found == model.end() ? kNil : found->second);
  }
  for (const auto& [key, value] : model) {
    ASSERT_EQ(map.find(key), value);
  }
}

}  // namespace
}  // namespace llme
