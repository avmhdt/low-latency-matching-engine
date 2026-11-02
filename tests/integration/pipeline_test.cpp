// The pipeline end to end, compared with an in-process run of the same log (FR-14, FR-15).
#include "llme/pipeline.hpp"

#include <gtest/gtest.h>

#include <cstdlib>
#include <filesystem>
#include <stdexcept>
#include <vector>

#include "generator.hpp"
#include "llme/mlog.hpp"
#include "llme/output_hash.hpp"

namespace llme {
namespace {

constexpr std::uint64_t kOps = 200000;

struct Flow {
  std::vector<InMsg> msgs;
  EngineConfig config;
};

Flow flow(const char* profile_path, std::uint64_t seed) {
  const auto profile = workload::load_profile(profile_path);
  workload::Generator generator(profile, seed);
  Flow out{.msgs = {}, .config = profile.engine};
  out.msgs.reserve(kOps);
  for (std::uint64_t i = 0; i < kOps; ++i) {
    out.msgs.push_back(generator.next());
  }
  return out;
}

TEST(PipelineTest, MatchesInProcessRun) {
  for (const char* path : {"configs/workloads/balanced.toml", "configs/workloads/adversarial.toml",
                           "configs/workloads/aggressive.toml"}) {
    const Flow f = flow(path, 21);
    const auto direct = run_in_process(f.msgs, f.config);
    const auto piped = run_pipeline(f.msgs, f.config, {});
    EXPECT_EQ(format_hash(piped.output_hash), format_hash(direct.output_hash)) << path;
    EXPECT_EQ(piped.out_records, direct.out_records) << path;
    EXPECT_EQ(piped.counters.msgs, kOps) << path;
    EXPECT_EQ(piped.counters.fills, direct.counters.fills) << path;
    EXPECT_EQ(piped.counters.rejects, direct.counters.rejects) << path;
  }
}

TEST(PipelineTest, EmptyLogShutsDownCleanly) {
  const auto result = run_pipeline({}, EngineConfig{.price_max = 15, .max_orders = 4}, {});
  EXPECT_EQ(result.out_records, 0U);
  EXPECT_EQ(result.counters.msgs, 0U);
  EXPECT_EQ(result.output_hash, OutputHasher{}.digest());
}

TEST(PipelineTest, ReservedBytesInTheLogCannotEndTheRun) {
  // The sentinel lives in InMsg::reserved; a log carrying that value must not stop the core.
  const Flow f = flow("configs/workloads/adversarial.toml", 22);
  std::vector<InMsg> hostile = f.msgs;
  for (InMsg& m : hostile) {
    m.reserved = 0xE0F0E0F0U;
  }
  const auto result = run_pipeline(hostile, f.config, {});
  EXPECT_EQ(result.counters.msgs, kOps);
  EXPECT_EQ(result.output_hash, run_in_process(f.msgs, f.config).output_hash);
}

TEST(PipelineTest, WritesAnOutputLogMatchingTheHash) {
  const Flow f = flow("configs/workloads/adversarial.toml", 23);
  const auto path = std::filesystem::temp_directory_path() / "llme_pipeline_test.olog";
  PipelineOptions options;
  options.olog = path;
  const auto result = run_pipeline(f.msgs, f.config, options);

  const auto reader = OlogReader::open(path);
  ASSERT_TRUE(reader.has_value());
  ASSERT_EQ(reader->records().size(), result.out_records);
  OutputHasher hasher;
  std::uint64_t seq = 0;
  for (const OutMsg& r : reader->records()) {
    ASSERT_EQ(r.seq, ++seq);
    hasher.update(r);
  }
  EXPECT_EQ(hasher.digest(), result.output_hash);
  std::filesystem::remove(path);
}

TEST(PipelineTest, UnwritableOutputLogIsAnError) {
  PipelineOptions options;
  options.olog = "/nonexistent-directory/x.olog";
  EXPECT_THROW((void)run_pipeline({}, EngineConfig{.price_max = 15, .max_orders = 4}, options),
               std::runtime_error);
}

TEST(PipelineTest, PinsWhenAsked) {
  const EngineConfig config{.price_max = 15, .max_orders = 4};
  PipelineOptions options;
  options.cores = {.gateway = 0, .core = 0, .publisher = 0};
  EXPECT_NO_THROW((void)run_pipeline({}, config, options));

  options.cores.core = 1 << 20;
  EXPECT_THROW((void)run_pipeline({}, config, options), std::runtime_error);
}

TEST(PipelineTest, ReportsItsRingCounters) {
  const Flow f = flow("configs/workloads/balanced.toml", 24);
  const auto result = run_pipeline(f.msgs, f.config, {});
  EXPECT_GT(result.seconds, 0.0);
  EXPECT_LE(result.max_in_depth, kInRingCapacity);
  EXPECT_LE(result.max_out_depth, kOutRingCapacity);
}

TEST(PipelineTest, CoresFromEnvironment) {
  ASSERT_EQ(setenv("LLME_PRODUCER_CORE", "3", 1), 0);
  ASSERT_EQ(setenv("LLME_BENCH_CORE", "", 1), 0);
  ASSERT_EQ(unsetenv("LLME_CONSUMER_CORE"), 0);
  const auto cores = cores_from_env();
  EXPECT_EQ(cores.gateway, 3);
  EXPECT_FALSE(cores.core.has_value());
  EXPECT_FALSE(cores.publisher.has_value());
  ASSERT_EQ(unsetenv("LLME_PRODUCER_CORE"), 0);
  ASSERT_EQ(unsetenv("LLME_BENCH_CORE"), 0);
}

}  // namespace
}  // namespace llme
