#include "llme/spsc_ring.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <memory>

namespace llme {
namespace {

TEST(SpscRingTest, StartsEmpty) {
  SpscRing<int, 4> ring;
  int value = 0;
  EXPECT_FALSE(ring.try_pop(value));
  EXPECT_EQ(ring.approx_size(), 0U);
  EXPECT_EQ(ring.capacity(), 4U);
}

TEST(SpscRingTest, IsFifo) {
  SpscRing<int, 4> ring;
  EXPECT_TRUE(ring.try_push(1));
  EXPECT_TRUE(ring.try_push(2));
  int value = 0;
  EXPECT_TRUE(ring.try_pop(value));
  EXPECT_EQ(value, 1);
  EXPECT_TRUE(ring.try_pop(value));
  EXPECT_EQ(value, 2);
  EXPECT_FALSE(ring.try_pop(value));
}

TEST(SpscRingTest, HoldsExactlyNItems) {
  SpscRing<int, 4> ring;
  for (int i = 0; i < 4; ++i) {
    EXPECT_TRUE(ring.try_push(i));
  }
  EXPECT_FALSE(ring.try_push(99));
  EXPECT_EQ(ring.approx_size(), 4U);
  int value = 0;
  EXPECT_TRUE(ring.try_pop(value));
  EXPECT_TRUE(ring.try_push(4));
  EXPECT_FALSE(ring.try_push(99));
}

TEST(SpscRingTest, WrapsAroundManyTimes) {
  SpscRing<std::uint64_t, 8> ring;
  std::uint64_t next_in = 0;
  std::uint64_t next_out = 0;
  for (int round = 0; round < 100; ++round) {
    while (ring.try_push(next_in)) {
      ++next_in;
    }
    std::uint64_t value = 0;
    for (int i = 0; i < 5; ++i) {
      ASSERT_TRUE(ring.try_pop(value));
      ASSERT_EQ(value, next_out++);
    }
  }
}

TEST(SpscRingTest, PopBatchTakesWhatIsThere) {
  SpscRing<int, 8> ring;
  std::array<int, 16> out{};
  EXPECT_EQ(ring.pop_batch(out.data(), out.size()), 0U);
  for (int i = 0; i < 5; ++i) {
    EXPECT_TRUE(ring.try_push(i));
  }
  EXPECT_EQ(ring.pop_batch(out.data(), 3), 3U);
  EXPECT_EQ(out[2], 2);
  EXPECT_EQ(ring.pop_batch(out.data(), out.size()), 2U);
  EXPECT_EQ(out[0], 3);
  EXPECT_EQ(out[1], 4);
  EXPECT_EQ(ring.pop_batch(out.data(), out.size()), 0U);
}

TEST(SpscRingTest, PopBatchAcrossTheWrap) {
  SpscRing<int, 4> ring;
  std::array<int, 4> out{};
  for (int round = 0; round < 10; ++round) {
    EXPECT_TRUE(ring.try_push(round));
    EXPECT_TRUE(ring.try_push(round + 100));
    EXPECT_TRUE(ring.try_push(round + 200));
    ASSERT_EQ(ring.pop_batch(out.data(), out.size()), 3U);
    EXPECT_EQ(out[0], round);
    EXPECT_EQ(out[1], round + 100);
    EXPECT_EQ(out[2], round + 200);
  }
}

TEST(SpscRingTest, IndicesLiveOnSeparateCacheLines) {
  EXPECT_EQ(alignof(SpscRing<int, 4>), 64U);
  EXPECT_GE(sizeof(SpscRing<int, 4>), 5U * 64U);
}

}  // namespace
}  // namespace llme
