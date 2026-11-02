// Runs every tests/golden/*.scn through an engine and compares records one by one.
// LLME_GOLDEN_UPDATE=1 rewrites only the `= hash` line of each scenario; the expected records
// themselves are hand-written and never generated.
#include <gtest/gtest.h>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "engine_adapters.hpp"
#include "llme/output_hash.hpp"
#include "scenario.hpp"

namespace llme::golden {
namespace {

std::vector<std::filesystem::path> scenario_files() {
  std::vector<std::filesystem::path> files;
  for (const auto& entry : std::filesystem::directory_iterator(LLME_GOLDEN_DIR)) {
    if (entry.path().extension() == ".scn") {
      files.push_back(entry.path());
    }
  }
  std::sort(files.begin(), files.end());
  return files;
}

void rewrite_hash_line(const std::filesystem::path& file, std::uint64_t hash) {
  std::ifstream in(file);
  std::ostringstream body;
  std::string line;
  while (std::getline(in, line)) {
    if (!line.starts_with("= hash")) {
      body << line << '\n';
    }
  }
  in.close();
  std::ofstream(file) << body.str() << "= hash " << format_hash(hash) << '\n';
}

struct Param {
  std::string engine;
  std::filesystem::path file;
};

class GoldenTest : public testing::TestWithParam<Param> {};

TEST_P(GoldenTest, Matches) {
  const auto scenario = parse_scenario(GetParam().file);
  auto engine = make_adapter(GetParam().engine, scenario.config);

  OutputHasher hasher;
  std::vector<OutMsg> out;
  for (std::size_t i = 0; i < scenario.steps.size(); ++i) {
    const Step& step = scenario.steps[i];
    out.clear();
    engine->on_message(step.input, out);
    ASSERT_EQ(out.size(), step.expected.size()) << "input step " << i;
    for (std::size_t r = 0; r < out.size(); ++r) {
      EXPECT_EQ(out[r], step.expected[r])
          << "input step " << i << ", record " << r << ": got " << testing::PrintToString(out[r]);
      hasher.update(out[r]);
    }
  }

  const auto snapshot = engine->snapshot();
  if (scenario.live) {
    EXPECT_EQ(engine->live_orders(), *scenario.live);
  }
  for (const auto& check : scenario.depth_checks) {
    const auto& side = check.side == Side::Buy ? snapshot.bids : snapshot.asks;
    const auto level = std::find_if(side.begin(), side.end(),
                                    [&](const auto& l) { return l.price == check.price; });
    ASSERT_NE(level, side.end()) << "no level at " << check.price;
    EXPECT_EQ(level->qty, check.qty);
    EXPECT_EQ(level->orders.size(), check.orders);
  }

  if (std::getenv("LLME_GOLDEN_UPDATE") != nullptr) {
    rewrite_hash_line(GetParam().file, hasher.digest());
    return;
  }
  ASSERT_TRUE(scenario.hash.has_value()) << "scenario has no `= hash` line";
  EXPECT_EQ(format_hash(hasher.digest()), format_hash(*scenario.hash));
}

std::vector<Param> all_params() {
  std::vector<Param> params;
  for (const auto& engine : adapter_names()) {
    for (const auto& file : scenario_files()) {
      params.push_back({engine, file});
    }
  }
  return params;
}

INSTANTIATE_TEST_SUITE_P(All, GoldenTest, testing::ValuesIn(all_params()),
                         [](const testing::TestParamInfo<Param>& param) {
                           return param.param.engine + "_" + param.param.file.stem().string();
                         });

TEST(GoldenSuite, HasAtLeastThirtyScenarios) { EXPECT_GE(scenario_files().size(), 30U); }

}  // namespace
}  // namespace llme::golden
