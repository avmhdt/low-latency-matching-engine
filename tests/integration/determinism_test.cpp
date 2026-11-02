// Determinism matrix (AC-W-05, FR-15): the same log gives the same hash across repeated runs,
// pipeline vs in-process, and fast engine vs reference. Every build runs this against the hashes
// pinned in tests/fixtures/expected_hashes.toml, which is what extends the matrix across
// Debug vs Release and GCC vs Clang.
//
// tests/fixtures/synth_small_balanced.mlog is committed. The other profiles are generated here
// with their fixed seeds and pinned by input hash, because six 10k-message logs would exceed
// DQ-10's 1 MB limit.
#include <gtest/gtest.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <toml++/toml.hpp>
#include <vector>

#include "engine_adapters.hpp"
#include "generator.hpp"
#include "llme/mlog.hpp"
#include "llme/output_hash.hpp"
#include "llme/pipeline.hpp"

namespace llme {
namespace {

constexpr int kRepeats = 5;
const std::filesystem::path kExpected = "tests/fixtures/expected_hashes.toml";

struct Fixture {
  std::string name;
  std::vector<InMsg> msgs;
  EngineConfig config;
  std::uint64_t seed = 0;
};

std::vector<Fixture> load_fixtures() {
  const toml::table table = toml::parse_file(kExpected.string());
  std::vector<Fixture> fixtures;
  for (const auto& [key, value] : table) {
    const auto* entry = value.as_table();
    const auto ops = static_cast<std::uint64_t>((*entry)["ops"].value_or(std::int64_t{0}));
    const auto seed = static_cast<std::uint64_t>((*entry)["seed"].value_or(std::int64_t{0}));
    const std::string name(key.str());
    const auto profile = workload::load_profile("configs/workloads/" + name + ".toml");
    workload::Generator generator(profile, seed);
    Fixture fixture{.name = name, .msgs = {}, .config = profile.engine, .seed = seed};
    for (std::uint64_t i = 0; i < ops; ++i) {
      fixture.msgs.push_back(generator.next());
    }
    fixtures.push_back(std::move(fixture));
  }
  return fixtures;
}

std::uint64_t hash_with_adapter(const char* engine, const Fixture& fixture) {
  const auto adapter = make_adapter(engine, fixture.config);
  OutputHasher hasher;
  std::vector<OutMsg> out;
  for (const InMsg& m : fixture.msgs) {
    out.clear();
    adapter->on_message(m, out);
    for (const OutMsg& r : out) {
      hasher.update(r);
    }
  }
  return hasher.digest();
}

std::string expected(const std::string& fixture, const char* field) {
  const toml::table table = toml::parse_file(kExpected.string());
  return table[fixture][field].value_or(std::string("missing"));
}

TEST(DeterminismTest, InputLogsArePinned) {
  for (const auto& fixture : load_fixtures()) {
    const auto hash = xxh3_bytes(std::as_bytes(std::span(fixture.msgs)));
    EXPECT_EQ(format_hash(hash), expected(fixture.name, "input")) << fixture.name;
  }
}

TEST(DeterminismTest, CommittedFixtureMatchesTheGenerator) {
  const auto fixtures = load_fixtures();
  const auto reader = MlogReader::open("tests/fixtures/synth_small_balanced.mlog");
  ASSERT_TRUE(reader.has_value()) << to_string(reader.error());
  for (const auto& fixture : fixtures) {
    if (fixture.name == "balanced") {
      ASSERT_EQ(reader->records().size(), fixture.msgs.size());
      EXPECT_EQ(reader->header().seed, fixture.seed);
      EXPECT_EQ(xxh3_bytes(std::as_bytes(reader->records())),
                xxh3_bytes(std::as_bytes(std::span(fixture.msgs))));
    }
  }
}

TEST(DeterminismTest, RepeatedInProcessRunsAgree) {
  for (const auto& fixture : load_fixtures()) {
    const std::string want = expected(fixture.name, "output");
    for (int run = 0; run < kRepeats; ++run) {
      EXPECT_EQ(format_hash(run_in_process(fixture.msgs, fixture.config).output_hash), want)
          << fixture.name << " run " << run;
    }
  }
}

TEST(DeterminismTest, RepeatedPipelineRunsAgree) {
  for (const auto& fixture : load_fixtures()) {
    const std::string want = expected(fixture.name, "output");
    for (int run = 0; run < kRepeats; ++run) {
      EXPECT_EQ(format_hash(run_pipeline(fixture.msgs, fixture.config, {}).output_hash), want)
          << fixture.name << " run " << run;
    }
  }
}

TEST(DeterminismTest, EnginesAgree) {
  for (const auto& fixture : load_fixtures()) {
    const std::string want = expected(fixture.name, "output");
    for (const auto& engine : adapter_names()) {
      EXPECT_EQ(format_hash(hash_with_adapter(engine.c_str(), fixture)), want)
          << fixture.name << " on " << engine;
    }
  }
}

}  // namespace
}  // namespace llme
