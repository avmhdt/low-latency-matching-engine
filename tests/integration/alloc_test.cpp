// Zero heap allocation after construction (FR-12, AC-W-06). This test is never disabled or
// weakened. The hook replaces operator new and the malloc family for this binary; messages are
// generated up front so that only the matching loop runs while the hook is armed.
#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <cstdlib>
#include <new>
#include <vector>

#include "alloc_hook.hpp"
#include "generator.hpp"
#include "llme/engine.hpp"
#include "llme/level_store_map.hpp"
#include "llme/level_store_sorted_vec.hpp"
#include "llme/output_hash.hpp"
#include "llme/pipeline.hpp"

namespace llme {
namespace {

constexpr std::uint64_t kWarmupMessages = 1000000;
constexpr std::uint64_t kArmedMessages = 10000000;

struct HashSink {
  OutputHasher* hasher;
  void operator()(const OutMsg& m) const noexcept { hasher->update(m); }
};

TEST(AllocTest, HookCountsEveryKindOfAllocation) {
  // Volatile stores keep the compiler from eliding a matched allocation and free.
  std::array<void* volatile, 8> ptrs{};
  testing::arm_alloc_hook();
  ptrs[0] = std::malloc(16);
  ptrs[1] = std::calloc(2, 16);
  ptrs[0] = std::realloc(ptrs[0], 32);
  ptrs[2] = std::aligned_alloc(64, 64);
  void* aligned = nullptr;
  ASSERT_EQ(posix_memalign(&aligned, 64, 64), 0);
  ptrs[3] = aligned;
  ptrs[4] = ::operator new(16);
  ptrs[5] = ::operator new[](16);
  ptrs[6] = ::operator new(16, std::align_val_t{64});
  ptrs[7] = ::operator new[](16, std::align_val_t{64});
  testing::disarm_alloc_hook();
  EXPECT_EQ(testing::allocations_while_armed(), 9U);
  for (int i = 0; i < 4; ++i) {
    std::free(ptrs[static_cast<std::size_t>(i)]);
  }
  ::operator delete(ptrs[4]);
  ::operator delete[](ptrs[5]);
  ::operator delete(ptrs[6], std::align_val_t{64});
  ::operator delete[](ptrs[7], std::align_val_t{64});
}

TEST(AllocTest, NothingCountedWhileDisarmed) {
  testing::arm_alloc_hook();
  testing::disarm_alloc_hook();
  void* volatile ptr = std::malloc(16);
  std::free(ptr);
  EXPECT_EQ(testing::allocations_while_armed(), 0U);
}

struct Flow {
  EngineConfig config;
  std::vector<InMsg> msgs;
};

// One generated stream shared by both tests, built before anything is armed.
const Flow& flow() {
  static const Flow generated = [] {
    const auto profile = workload::load_profile("configs/workloads/balanced.toml");
    workload::Generator generator(profile, 7);
    Flow out{.config = profile.engine, .msgs = {}};
    out.msgs.reserve(kWarmupMessages + kArmedMessages);
    for (std::uint64_t i = 0; i < kWarmupMessages + kArmedMessages; ++i) {
      out.msgs.push_back(generator.next());
    }
    return out;
  }();
  return generated;
}

// D1 and D3 promise the same, through a fixed node pool and reserved vectors.
template <class Store>
void expect_no_allocations(const char* name) {
  const auto& [config, msgs] = flow();
  OutputHasher hasher;
  MatchingEngine<Store, HashSink> engine(config, HashSink{&hasher});
  for (std::uint64_t i = 0; i < kWarmupMessages; ++i) {
    engine.on_message(msgs[i]);
  }
  testing::arm_alloc_hook();
  for (std::uint64_t i = kWarmupMessages; i < msgs.size(); ++i) {
    engine.on_message(msgs[i]);
  }
  testing::disarm_alloc_hook();
  EXPECT_EQ(testing::allocations_while_armed(), 0U) << name;
}

TEST(AllocTest, OtherDesignsAreAllocationFree) {
  expect_no_allocations<MapLevelStore>("d1");
  expect_no_allocations<SortedVecLevelStore>("d3");
}

TEST(AllocTest, HotPathIsAllocationFree) {
  const auto& [config, msgs] = flow();

  OutputHasher hasher;
  MatchingEngine<LadderLevelStore, HashSink> engine(config, HashSink{&hasher});
  for (std::uint64_t i = 0; i < kWarmupMessages; ++i) {
    engine.on_message(msgs[i]);
  }

  testing::arm_alloc_hook();
  for (std::uint64_t i = kWarmupMessages; i < msgs.size(); ++i) {
    engine.on_message(msgs[i]);
  }
  testing::disarm_alloc_hook();

  EXPECT_EQ(testing::allocations_while_armed(), 0U);
  EXPECT_EQ(engine.counters().msgs, msgs.size());
  EXPECT_GT(engine.counters().fills, 0U);
}

// All three pipeline threads are spinning at the start line when the hook is armed, and the hook
// counts on every thread, so this covers the gateway and publisher loops as well as the core.
TEST(AllocTest, PipelineCoreLoopIsAllocationFree) {
  const auto& [config, msgs] = flow();
  PipelineOptions options;
  options.on_threads_ready = [] { testing::arm_alloc_hook(); };
  options.on_threads_done = [] { testing::disarm_alloc_hook(); };
  const auto result = run_pipeline(msgs, config, options);
  EXPECT_EQ(testing::allocations_while_armed(), 0U);
  EXPECT_EQ(result.counters.msgs, msgs.size());
}

}  // namespace
}  // namespace llme
