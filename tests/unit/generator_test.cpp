#include "generator.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

#include "llme/output_hash.hpp"

namespace llme::workload {
namespace {

constexpr std::array<const char*, 6> kProfiles = {"balanced",  "cancel_heavy", "aggressive",
                                                  "deep_book", "adversarial",  "burst"};

std::filesystem::path profile_path(const std::string& name) {
  return std::filesystem::path("configs/workloads") / (name + ".toml");
}

std::vector<InMsg> generate(const Profile& profile, std::uint64_t seed, std::size_t ops) {
  Generator generator(profile, seed);
  std::vector<InMsg> msgs;
  msgs.reserve(ops);
  for (std::size_t i = 0; i < ops; ++i) {
    msgs.push_back(generator.next());
  }
  return msgs;
}

std::uint64_t hash_of(const std::vector<InMsg>& msgs) {
  return xxh3_bytes(std::as_bytes(std::span(msgs)));
}

TEST(RngTest, IsDeterministicAndSeedSensitive) {
  Rng a(1);
  Rng b(1);
  Rng c(2);
  for (int i = 0; i < 100; ++i) {
    const auto x = a.next();
    EXPECT_EQ(x, b.next());
    EXPECT_NE(x, c.next());
  }
}

TEST(RngTest, FirstOutputsArePinned) {
  // Reference values of xoshiro256** seeded through splitmix64; a change here breaks DQ-08.
  Rng rng(0);
  EXPECT_EQ(rng.next(), 0x99EC5F36CB75F2B4ULL);
  EXPECT_EQ(rng.next(), 0xBF6E1F784956452AULL);
}

TEST(RngTest, BelowStaysInRange) {
  Rng rng(7);
  std::set<std::uint64_t> seen;
  for (int i = 0; i < 1000; ++i) {
    const auto v = rng.below(5);
    ASSERT_LT(v, 5U);
    seen.insert(v);
  }
  EXPECT_EQ(seen.size(), 5U);
}

TEST(ProbabilityThresholdTest, Saturates) {
  EXPECT_EQ(probability_threshold(0.0), 0U);
  EXPECT_EQ(probability_threshold(-1.0), 0U);
  EXPECT_EQ(probability_threshold(1.0), ~std::uint64_t{0});
  EXPECT_EQ(probability_threshold(0.5), std::uint64_t{1} << 63U);
}

TEST(GeneratorTest, SameSeedSameBytes) {
  for (const auto* name : kProfiles) {
    const auto profile = load_profile(profile_path(name));
    EXPECT_EQ(hash_of(generate(profile, 9, 5000)), hash_of(generate(profile, 9, 5000))) << name;
    EXPECT_NE(hash_of(generate(profile, 9, 5000)), hash_of(generate(profile, 10, 5000))) << name;
  }
}

TEST(GeneratorTest, StreamsArePinned) {
  // A changed value here means every log and every expected hash has changed: DQ-08.
  const std::map<std::string, std::string> expected = {
      {"balanced", "0xfd43a1049c4195d7"},    {"cancel_heavy", "0x38d112fc0027b8a0"},
      {"aggressive", "0xd114caf69c7ccfa5"},  {"deep_book", "0xff08ab73a4d827a2"},
      {"adversarial", "0xe0793f3c5e046d9c"}, {"burst", "0xfd43a1049c4195d7"}};
  for (const auto& [name, hash] : expected) {
    const auto profile = load_profile(profile_path(name));
    EXPECT_EQ(format_hash(hash_of(generate(profile, profile.seed, 20000))), hash) << name;
  }
}

TEST(GeneratorTest, MixFollowsTheProfile) {
  const auto profile = load_profile(profile_path("balanced"));
  const auto msgs = generate(profile, 1, 200000);
  std::uint64_t cancels = 0;
  std::uint64_t modifies = 0;
  std::uint64_t markets = 0;
  std::uint64_t iocs = 0;
  std::uint64_t foks = 0;
  for (const InMsg& m : msgs) {
    cancels += m.type == to_wire(MsgType::Cancel) ? 1U : 0U;
    modifies += m.type == to_wire(MsgType::Modify) ? 1U : 0U;
    const bool is_new = m.type == to_wire(MsgType::New);
    markets += is_new && m.ord_type == to_wire(OrdType::Market) ? 1U : 0U;
    iocs += is_new && m.ord_type == 0 && m.tif == to_wire(Tif::Ioc) ? 1U : 0U;
    foks += is_new && m.ord_type == 0 && m.tif == to_wire(Tif::Fok) ? 1U : 0U;
  }
  const auto share = [&](std::uint64_t n) {
    return static_cast<double>(n) / static_cast<double>(msgs.size());
  };
  EXPECT_NEAR(share(cancels), 0.38, 0.02);
  EXPECT_NEAR(share(modifies), 0.08, 0.01);
  EXPECT_NEAR(share(markets), 0.02, 0.005);
  EXPECT_NEAR(share(iocs), 0.03, 0.005);
  EXPECT_NEAR(share(foks), 0.01, 0.005);
}

TEST(GeneratorTest, CancelsHitLiveOrders) {
  const auto profile = load_profile(profile_path("balanced"));
  Generator generator(profile, 3);
  ReferenceEngine engine(profile.engine);
  std::vector<OutMsg> out;
  std::uint64_t rejects = 0;
  for (int i = 0; i < 100000; ++i) {
    out.clear();
    engine.on_message(generator.next(), out);
    for (const OutMsg& r : out) {
      rejects += r.type == to_wire(OutType::CancelReject) ? 1U : 0U;
      rejects += r.type == to_wire(OutType::ModifyReject) ? 1U : 0U;
      rejects += r.type == to_wire(OutType::Reject) ? 1U : 0U;
    }
  }
  EXPECT_EQ(rejects, 0U);
}

TEST(GeneratorTest, TargetLiveRegulatesTheBook) {
  auto profile = load_profile(profile_path("balanced"));
  profile.target_live = 500;
  Generator generator(profile, 3);
  for (int i = 0; i < 50000; ++i) {
    (void)generator.next();
    ASSERT_LE(generator.shadow().live_orders(), 501U);
  }
}

TEST(GeneratorTest, AdversarialReachesEveryRejectReason) {
  const auto profile = load_profile(profile_path("adversarial"));
  Generator generator(profile, 5);
  ReferenceEngine engine(profile.engine);
  std::vector<OutMsg> out;
  std::set<int> reject_reasons;
  std::set<int> cancel_reasons;
  std::set<int> out_types;
  for (int i = 0; i < 200000; ++i) {
    out.clear();
    engine.on_message(generator.next(), out);
    for (const OutMsg& r : out) {
      out_types.insert(r.type);
      if (r.type == to_wire(OutType::Reject)) {
        reject_reasons.insert(r.reason);
      } else if (r.type == to_wire(OutType::Canceled)) {
        cancel_reasons.insert(r.reason);
      }
    }
  }
  EXPECT_EQ(out_types.size(), 9U);
  EXPECT_EQ(reject_reasons, (std::set<int>{1, 2, 3, 4, 6, 7}));
  EXPECT_EQ(cancel_reasons, (std::set<int>{1, 2, 3, 4}));
}

TEST(GeneratorTest, ReportsWhichMessagesBelongToABurst) {
  auto profile = load_profile(profile_path("burst"));
  profile.burst_length = 20;
  profile.burst_every = 5000;
  Generator generator(profile, 1);
  std::uint64_t flagged = 0;
  for (int i = 0; i < 5100; ++i) {
    (void)generator.next();
    if (generator.last_was_burst()) {
      ++flagged;
      EXPECT_GE(i, 5000);
      EXPECT_LT(i, 5020);
    }
  }
  EXPECT_EQ(flagged, 20U);
}

TEST(GeneratorTest, BurstTargetOverridesTheCancelTarget) {
  auto profile = load_profile(profile_path("burst"));
  profile.burst_length = 50;
  profile.burst_every = 5000;
  auto oldest = profile;
  oldest.burst_target = CancelTarget::Oldest;
  // Same flow up to the burst, different choices inside it.
  EXPECT_NE(hash_of(generate(profile, 1, 5100)), hash_of(generate(oldest, 1, 5100)));
  EXPECT_EQ(hash_of(generate(profile, 1, 5000)), hash_of(generate(oldest, 1, 5000)));
}

TEST(ProfileTest, ReadsBurstTarget) {
  const auto path = std::filesystem::temp_directory_path() / "llme_burst_target.toml";
  std::ofstream(path) << "[burst]\nenabled = true\ntarget = \"near_touch\"\n";
  EXPECT_EQ(load_profile(path).burst_target, CancelTarget::NearTouch);
  std::ofstream(path) << "[burst]\ntarget = \"sideways\"\n";
  EXPECT_THROW((void)load_profile(path), std::runtime_error);
  std::filesystem::remove(path);
}

TEST(GeneratorTest, BurstEmitsConsecutiveCancels) {
  auto profile = load_profile(profile_path("burst"));
  profile.burst_length = 20;
  profile.burst_every = 5000;
  const auto msgs = generate(profile, 1, 5100);
  EXPECT_TRUE(std::all_of(msgs.begin() + 5000, msgs.begin() + 5020,
                          [](const InMsg& m) { return m.type == to_wire(MsgType::Cancel); }));
}

TEST(GeneratorTest, EveryCancelTargetProducesFlow) {
  for (const auto target : {CancelTarget::Uniform, CancelTarget::Oldest, CancelTarget::Newest,
                            CancelTarget::NearTouch}) {
    auto profile = load_profile(profile_path("balanced"));
    profile.cancel_target = target;
    EXPECT_EQ(generate(profile, 1, 20000).size(), 20000U);
  }
}

TEST(ProfileTest, RatesNormaliseToShares) {
  const auto path = std::filesystem::temp_directory_path() / "llme_rates_profile.toml";
  std::ofstream(path) << "[rates]\nnew_limit = 300.0\ncancel = 100.0\n";
  const auto profile = load_profile(path);
  EXPECT_DOUBLE_EQ(profile.mix.new_limit, 0.75);
  EXPECT_DOUBLE_EQ(profile.mix.cancel, 0.25);
  EXPECT_DOUBLE_EQ(profile.mix.market, 0.0);
  std::filesystem::remove(path);
}

TEST(ProfileTest, RejectsBadFiles) {
  const auto path = std::filesystem::temp_directory_path() / "llme_bad_profile.toml";
  const std::array<std::string, 9> bad = {"[mix]\nnew_limit = 0.5\n",
                                          "[mix]\nnew_limit = 1.5\ncancel = -0.5\n",
                                          "[price]\ndist = \"cauchy\"\n",
                                          "[size]\ndist = \"zipf\"\n",
                                          "[cancel]\ntarget = \"random\"\n",
                                          "[engine]\nstp = \"cancel_newest\"\n",
                                          "[engine]\nprice_min = 5\nprice_max = 4\n",
                                          "[generator]\nmid_start = -1\n",
                                          "not toml ["};
  for (const auto& text : bad) {
    std::ofstream(path) << text;
    EXPECT_THROW((void)load_profile(path), std::runtime_error) << text;
  }
  std::filesystem::remove(path);
  EXPECT_THROW((void)load_profile("does_not_exist.toml"), std::runtime_error);
}

TEST(ProfileTest, EngineConfigLoads) {
  const auto cfg = load_engine_config("configs/engine.toml");
  EXPECT_EQ(cfg.price_max, 1048575);
  EXPECT_EQ(cfg.max_orders, 1048576U);
  EXPECT_DOUBLE_EQ(cfg.id_map_load, 0.5);
}

}  // namespace
}  // namespace llme::workload
