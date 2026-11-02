// SPSC ring under real concurrency (AC-W-09). Items are numbered, so any loss, duplicate or
// reorder shows up as a value mismatch. 10M items by default, which is what runs under TSan;
// LLME_RING_STRESS_ITEMS=1000000000 is the Release run recorded in results/.
#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <string>
#include <thread>

#include "llme/messages.hpp"
#include "llme/spsc_ring.hpp"

namespace llme {
namespace {

std::uint64_t stress_items() {
  const char* configured = std::getenv("LLME_RING_STRESS_ITEMS");
  return configured == nullptr ? 10000000 : std::stoull(configured);
}

// A record larger than one word, so a torn copy cannot pass the check by accident.
struct Item {
  std::uint64_t seq = 0;
  std::uint64_t complement = 0;
  std::array<std::uint64_t, 2> padding{};
};

template <std::size_t N, bool UseBatch>
void run_stress(std::uint64_t items) {
  const auto ring = std::make_unique<SpscRing<Item, N>>();

  std::thread producer([&] {
    for (std::uint64_t i = 0; i < items; ++i) {
      const Item item{.seq = i, .complement = ~i, .padding = {i, i}};
      while (!ring->try_push(item)) {
        std::this_thread::yield();
      }
    }
  });

  std::uint64_t expected = 0;
  std::uint64_t bad = 0;
  const auto check = [&](const Item& item) {
    if (item.seq != expected || item.complement != ~expected || item.padding[1] != expected) {
      ++bad;
    }
    ++expected;
  };

  if constexpr (UseBatch) {
    std::array<Item, 256> batch;
    while (expected < items) {
      const std::size_t count = ring->pop_batch(batch.data(), batch.size());
      if (count == 0) {
        std::this_thread::yield();
      }
      for (std::size_t i = 0; i < count; ++i) {
        check(batch[i]);
      }
    }
  } else {
    Item item;
    while (expected < items) {
      if (ring->try_pop(item)) {
        check(item);
      } else {
        std::this_thread::yield();
      }
    }
  }

  producer.join();
  EXPECT_EQ(bad, 0U);
  EXPECT_EQ(expected, items);
  EXPECT_EQ(ring->approx_size(), 0U);
}

TEST(RingStress, OrderedNoLoss) { run_stress<65536, false>(stress_items()); }

TEST(RingStress, OrderedNoLossThroughPopBatch) { run_stress<65536, true>(stress_items() / 4); }

// A tiny ring forces constant full and empty transitions, where the cached indices refresh.
TEST(RingStress, OrderedNoLossOnATinyRing) { run_stress<4, false>(stress_items() / 16); }

}  // namespace
}  // namespace llme
