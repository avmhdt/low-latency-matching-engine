#include "llme/order_pool.hpp"

#include <gtest/gtest.h>

#include <set>
#include <vector>

namespace llme {
namespace {

std::vector<OrderId> queue_of(const OrderPool& pool, const Level& level) {
  std::vector<OrderId> ids;
  for (std::uint32_t i = level.head; i != kNil; i = pool[i].next) {
    ids.push_back(pool[i].id);
  }
  return ids;
}

std::uint32_t add(OrderPool& pool, Level& level, OrderId id, Qty leaves) {
  const std::uint32_t index = pool.allocate();
  pool[index].id = id;
  pool[index].leaves = leaves;
  level_push_back(pool, level, index);
  return index;
}

TEST(OrderPoolTest, AllocatesEverySlotOnce) {
  OrderPool pool(4);
  EXPECT_EQ(pool.capacity(), 4U);
  std::set<std::uint32_t> seen;
  for (int i = 0; i < 4; ++i) {
    ASSERT_FALSE(pool.full());
    seen.insert(pool.allocate());
  }
  EXPECT_EQ(seen.size(), 4U);
  EXPECT_TRUE(pool.full());
  EXPECT_EQ(pool.live(), 4U);
  EXPECT_EQ(pool.free_count(), 0U);
}

TEST(OrderPoolTest, ReleasedSlotsComeBack) {
  OrderPool pool(2);
  const auto a = pool.allocate();
  const auto b = pool.allocate();
  pool.release(a);
  EXPECT_FALSE(pool.full());
  EXPECT_EQ(pool.free_count(), 1U);
  EXPECT_EQ(pool.allocate(), a);
  pool.release(b);
  EXPECT_EQ(pool.live(), 1U);
}

TEST(OrderPoolTest, ZeroCapacityIsFull) {
  const OrderPool pool(0);
  EXPECT_TRUE(pool.full());
  EXPECT_EQ(pool.free_count(), 0U);
}

TEST(LevelListTest, KeepsFifoOrderAndAggregates) {
  OrderPool pool(4);
  Level level;
  add(pool, level, 1, 10);
  add(pool, level, 2, 20);
  add(pool, level, 3, 30);
  EXPECT_EQ(queue_of(pool, level), (std::vector<OrderId>{1, 2, 3}));
  EXPECT_EQ(level.qty, 60U);
  EXPECT_EQ(level.count, 3U);
}

TEST(LevelListTest, UnlinksFromHeadMiddleAndTail) {
  OrderPool pool(4);
  Level level;
  const auto a = add(pool, level, 1, 1);
  const auto b = add(pool, level, 2, 2);
  const auto c = add(pool, level, 3, 3);
  const auto d = add(pool, level, 4, 4);

  level_unlink(pool, level, b);
  EXPECT_EQ(queue_of(pool, level), (std::vector<OrderId>{1, 3, 4}));
  level_unlink(pool, level, a);
  EXPECT_EQ(queue_of(pool, level), (std::vector<OrderId>{3, 4}));
  EXPECT_EQ(level.head, c);
  level_unlink(pool, level, d);
  EXPECT_EQ(queue_of(pool, level), (std::vector<OrderId>{3}));
  EXPECT_EQ(level.tail, c);
  EXPECT_EQ(level.qty, 3U);
  level_unlink(pool, level, c);
  EXPECT_EQ(level.head, kNil);
  EXPECT_EQ(level.tail, kNil);
  EXPECT_EQ(level.count, 0U);
  EXPECT_EQ(level.qty, 0U);
}

}  // namespace
}  // namespace llme
