// Seeded properties of the fast engine: full invariants after every message, 100k ops per seed
// over 20 seeds (spec section 11), plus agreement with the reference at every step.
#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

#include "engine_adapters.hpp"
#include "generator.hpp"

namespace llme {
namespace {

constexpr std::uint64_t kOpsPerSeed = 100000;

class EnginePropertyTest : public testing::TestWithParam<std::uint64_t> {};

TEST_P(EnginePropertyTest, FullInvariantsHoldAfterEveryMessage) {
  auto profile = workload::load_profile("configs/workloads/adversarial.toml");
  // A wider pool than the profile's, so the checker sees real queues as well as a full pool.
  profile.engine.max_orders = 128;
  profile.target_live = 96;
  profile.mix.new_limit = 0.54;
  profile.mix.market = 0.01;
  profile.mix.fok = 0.11;
  workload::Generator generator(profile, GetParam());
  const auto fast = make_adapter("d2", profile.engine);
  const auto reference = make_adapter("ref", profile.engine);

  std::vector<OutMsg> out;
  std::vector<OutMsg> expected;
  for (std::uint64_t i = 0; i < kOpsPerSeed; ++i) {
    const InMsg m = generator.next();
    out.clear();
    expected.clear();
    fast->on_message(m, out);
    reference->on_message(m, expected);
    ASSERT_EQ(out, expected) << "message " << i;
    ASSERT_NO_THROW(fast->check_full()) << "message " << i;
  }
  EXPECT_EQ(fast->snapshot(), reference->snapshot());
  EXPECT_EQ(fast->live_orders(), reference->live_orders());
}

TEST_P(EnginePropertyTest, AgreesOnRegularFlow) {
  for (const char* name : {"configs/workloads/balanced.toml", "configs/workloads/cancel_heavy.toml",
                           "configs/workloads/aggressive.toml"}) {
    const auto profile = workload::load_profile(name);
    workload::Generator generator(profile, GetParam());
    const auto fast = make_adapter("d2", profile.engine);
    const auto reference = make_adapter("ref", profile.engine);
    std::vector<OutMsg> out;
    std::vector<OutMsg> expected;
    for (std::uint64_t i = 0; i < 20000; ++i) {
      const InMsg m = generator.next();
      out.clear();
      expected.clear();
      fast->on_message(m, out);
      reference->on_message(m, expected);
      ASSERT_EQ(out, expected) << name << " message " << i;
      ASSERT_NO_THROW(fast->check_touched(out)) << name << " message " << i;
    }
    ASSERT_NO_THROW(fast->check_full()) << name;
    EXPECT_EQ(fast->snapshot(), reference->snapshot()) << name;
  }
}

INSTANTIATE_TEST_SUITE_P(Seeds, EnginePropertyTest, testing::Range<std::uint64_t>(1, 21));

}  // namespace
}  // namespace llme
