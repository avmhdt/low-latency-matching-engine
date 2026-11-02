// Replays every shrunk reproducer in tests/fixtures/regressions/ through each engine and expects
// equal hashes. llme-diff writes <name>.mlog with its band and pool in <name>.toml.
#include <gtest/gtest.h>

#include <filesystem>
#include <vector>

#include "engine_adapters.hpp"
#include "generator.hpp"
#include "llme/mlog.hpp"
#include "llme/output_hash.hpp"

namespace llme {
namespace {

TEST(RegressionTest, EveryReproducerNowAgrees) {
  int replayed = 0;
  for (const auto& entry : std::filesystem::directory_iterator("tests/fixtures/regressions")) {
    if (entry.path().extension() != ".mlog") {
      continue;
    }
    const auto reader = MlogReader::open(entry.path());
    ASSERT_TRUE(reader.has_value()) << entry.path();
    auto config_path = entry.path();
    config_path.replace_extension(".toml");
    const EngineConfig config = workload::load_engine_config(config_path);

    std::optional<std::uint64_t> agreed;
    for (const auto& name : adapter_names()) {
      const auto engine = make_adapter(name, config);
      OutputHasher hasher;
      std::vector<OutMsg> out;
      for (const InMsg& m : reader->records()) {
        out.clear();
        engine->on_message(m, out);
        for (const OutMsg& r : out) {
          hasher.update(r);
        }
      }
      if (agreed) {
        EXPECT_EQ(hasher.digest(), *agreed) << entry.path() << " on " << name;
      }
      agreed = hasher.digest();
    }
    ++replayed;
  }
  RecordProperty("replayed", replayed);
}

}  // namespace
}  // namespace llme
