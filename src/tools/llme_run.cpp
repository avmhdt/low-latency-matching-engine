// llme-run: run one log through an engine and print the output hash.
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

#include "cli_common.hpp"
#include "engine_adapters.hpp"
#include "experiment_log.hpp"
#include "generator.hpp"
#include "llme/mlog.hpp"
#include "llme/output_hash.hpp"

namespace {

struct Options {
  std::string in;
  std::string engine = "ref";
  std::string out;
  std::string config;
  bool json = false;
};

int execute(const Options& options) {
  auto reader = llme::MlogReader::open(options.in);
  if (!reader) {
    throw std::runtime_error(std::string(llme::to_string(reader.error())) + ": " + options.in);
  }
  const llme::EngineConfig config = options.config.empty()
                                        ? llme::EngineConfig{}
                                        : llme::workload::load_engine_config(options.config);
  const auto engine = llme::make_adapter(options.engine, config);

  std::optional<llme::OlogWriter> olog;
  if (!options.out.empty()) {
    auto writer = llme::OlogWriter::create(options.out, {.seed = reader->header().seed});
    if (!writer) {
      throw std::runtime_error(std::string(llme::to_string(writer.error())) + ": " + options.out);
    }
    olog.emplace(std::move(*writer));
  }

  llme::OutputHasher hasher;
  std::vector<llme::OutMsg> out;
  std::uint64_t out_records = 0;
  for (const llme::InMsg& m : reader->records()) {
    out.clear();
    engine->on_message(m, out);
    for (const llme::OutMsg& r : out) {
      hasher.update(r);
      if (olog && !olog->write(r)) {
        throw std::runtime_error("write failed: " + options.out);
      }
    }
    out_records += out.size();
  }
  if (olog) {
    if (auto closed = olog->close(); !closed) {
      throw std::runtime_error(std::string(llme::to_string(closed.error())) + ": " + options.out);
    }
  }

  const auto output_hash = llme::format_hash(hasher.digest());
  if (options.json) {
    std::cout << nlohmann::json{{"input", options.in},
                                {"engine", options.engine},
                                {"mode", "inproc"},
                                {"msgs", reader->header().record_count},
                                {"out_records", out_records},
                                {"output_hash", output_hash},
                                {"git_sha", llme::tools::git_sha()}}
                     .dump()
              << '\n';
  } else {
    std::cout << output_hash << '\n';
  }
  return 0;
}

int run(int argc, char** argv) {
  CLI::App app{"Run an input log through an engine", "llme-run"};
  Options options;
  app.add_option("--in", options.in, "Input .mlog path")->required();
  app.add_option("--engine", options.engine, "Engine name");
  app.add_option("--out", options.out, "Write the output records to this .olog");
  app.add_option("--config", options.config, "Engine config TOML");
  app.add_flag("--json", options.json, "Print a JSON summary");
  return llme::tools::parse_and_run(app, argc, argv, [&] { return execute(options); });
}

}  // namespace

int main(int argc, char** argv) {
  return llme::tools::guarded_main([&] { return run(argc, argv); });
}
