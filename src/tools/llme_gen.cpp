// llme-gen: synthetic generator to .mlog (FR-18).
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>

#include "cli_common.hpp"
#include "generator.hpp"
#include "llme/mlog.hpp"
#include "llme/output_hash.hpp"

namespace {

int run(int argc, char** argv) {
  CLI::App app{"Generate a synthetic .mlog input log", "llme-gen"};
  std::string profile_path;
  std::string out;
  std::uint64_t ops = 10000;
  std::optional<std::uint64_t> seed;
  bool json = false;
  app.add_option("--profile", profile_path, "Workload profile TOML")->required();
  app.add_option("--ops", ops, "Number of messages");
  app.add_option("--seed", seed, "Generator seed (default: the profile's)");
  app.add_option("--out", out, "Output .mlog path")->required();
  app.add_flag("--json", json, "Print a JSON summary");

  return llme::tools::parse_and_run(app, argc, argv, [&] {
    const auto profile = llme::workload::load_profile(profile_path);
    const std::uint64_t used_seed = seed.value_or(profile.seed);
    auto writer = llme::MlogWriter::create(out, {.seed = used_seed});
    if (!writer) {
      throw std::runtime_error(std::string(llme::to_string(writer.error())) + ": " + out);
    }
    llme::workload::Generator generator(profile, used_seed);
    for (std::uint64_t i = 0; i < ops; ++i) {
      if (!writer->write(generator.next())) {
        throw std::runtime_error("write failed: " + out);
      }
    }
    if (auto closed = writer->close(); !closed) {
      throw std::runtime_error(std::string(llme::to_string(closed.error())) + ": " + out);
    }
    const auto reader = llme::MlogReader::open(out);
    if (!reader) {
      throw std::runtime_error(std::string(llme::to_string(reader.error())) + ": " + out);
    }
    const auto input_hash = llme::format_hash(llme::xxh3_bytes(std::as_bytes(reader->records())));
    if (json) {
      std::cout << nlohmann::json{{"out", out},
                                  {"profile", profile.name},
                                  {"ops", ops},
                                  {"seed", used_seed},
                                  {"input_hash", input_hash}}
                       .dump()
                << '\n';
    } else {
      std::cout << ops << " messages, input hash " << input_hash << '\n';
    }
    return 0;
  });
}

}  // namespace

int main(int argc, char** argv) {
  return llme::tools::guarded_main([&] { return run(argc, argv); });
}
