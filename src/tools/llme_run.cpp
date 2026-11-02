// llme-run: run one log through an engine and print the output hash. --pipeline runs the
// three-thread pipeline of FR-14; a run report goes to run.json (spec section 10).
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
#include "llme/pipeline.hpp"

namespace {

using nlohmann::json;

struct Options {
  std::string in;
  std::string engine = "d2";
  std::string out;
  std::string config;
  std::string run_json;
  bool pipeline = false;
  bool json_output = false;
};

// Reference and other adapter-driven engines, one message at a time.
llme::PipelineResult run_adapter(const Options& options, std::span<const llme::InMsg> msgs,
                                 const llme::EngineConfig& config) {
  const auto engine = llme::make_adapter(options.engine, config);
  llme::OutputHasher hasher;
  std::optional<llme::OlogWriter> olog;
  if (!options.out.empty()) {
    auto writer = llme::OlogWriter::create(options.out);
    if (!writer) {
      throw std::runtime_error(std::string(llme::to_string(writer.error())) + ": " + options.out);
    }
    olog.emplace(std::move(*writer));
  }
  llme::PipelineResult result;
  std::vector<llme::OutMsg> out;
  for (const llme::InMsg& m : msgs) {
    out.clear();
    engine->on_message(m, out);
    for (const llme::OutMsg& r : out) {
      hasher.update(r);
      if (olog && !olog->write(r)) {
        throw std::runtime_error("write failed: " + options.out);
      }
    }
    result.out_records += out.size();
  }
  if (olog) {
    if (auto closed = olog->close(); !closed) {
      throw std::runtime_error(std::string(llme::to_string(closed.error())) + ": " + options.out);
    }
  }
  result.output_hash = hasher.digest();
  result.counters.msgs = msgs.size();
  return result;
}

json counters_json(const llme::PipelineResult& r) {
  return {{"msgs", r.counters.msgs},
          {"acks", r.counters.acks},
          {"rejects", r.counters.rejects},
          {"fills", r.counters.fills},
          {"trades", r.counters.trades},
          {"canceled", r.counters.canceled},
          {"modified", r.counters.modified},
          {"cancel_rejects", r.counters.cancel_rejects},
          {"modify_rejects", r.counters.modify_rejects},
          {"level_updates", r.counters.level_updates},
          {"in_ring_full_spins", r.in_ring_full_spins},
          {"out_ring_full_spins", r.out_ring_full_spins},
          {"max_in_ring_depth", r.max_in_depth},
          {"max_out_ring_depth", r.max_out_depth}};
}

std::string sha256_of(const std::string& path) {
  // Tool start-up only: hashing a data file is not worth a crypto dependency.
  const std::string sum = llme::tools::capture_command("sha256sum -- '" + path + "' 2>/dev/null");
  return sum.substr(0, sum.find(' '));
}

int execute(const Options& options) {
  auto reader = llme::MlogReader::open(options.in);
  if (!reader) {
    throw std::runtime_error(std::string(llme::to_string(reader.error())) + ": " + options.in);
  }
  if (options.pipeline && options.engine != "d2") {
    throw std::runtime_error("--pipeline runs the d2 engine only");
  }
  const llme::EngineConfig config = options.config.empty()
                                        ? llme::EngineConfig{}
                                        : llme::workload::load_engine_config(options.config);

  llme::PipelineResult result;
  if (options.pipeline) {
    llme::PipelineOptions pipeline;
    pipeline.cores = llme::cores_from_env();
    if (!options.out.empty()) {
      pipeline.olog = options.out;
    }
    result = llme::run_pipeline(reader->records(), config, pipeline);
  } else {
    result = run_adapter(options, reader->records(), config);
  }

  const auto output_hash = llme::format_hash(result.output_hash);
  const json report = {{"input", options.in},
                       {"input_sha256", options.run_json.empty() ? "" : sha256_of(options.in)},
                       {"engine", options.engine},
                       {"mode", options.pipeline ? "pipeline" : "inproc"},
                       {"msgs", reader->header().record_count},
                       {"out_records", result.out_records},
                       {"output_hash", output_hash},
                       {"counters", counters_json(result)},
                       {"git_sha", llme::tools::git_sha()},
                       {"manifest_id", llme::tools::hostname()}};
  if (!options.run_json.empty()) {
    std::ofstream(options.run_json) << report.dump(2) << '\n';
  }
  if (options.json_output) {
    std::cout << report.dump() << '\n';
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
  app.add_flag("--pipeline", options.pipeline, "Run gateway, core and publisher threads");
  app.add_option("--out", options.out, "Write the output records to this .olog");
  app.add_option("--config", options.config, "Engine config TOML");
  app.add_option("--run-json", options.run_json, "Write the run report to this path");
  app.add_flag("--json", options.json_output, "Print a JSON summary");
  return llme::tools::parse_and_run(app, argc, argv, [&] { return execute(options); });
}

}  // namespace

int main(int argc, char** argv) {
  return llme::tools::guarded_main([&] { return run(argc, argv); });
}
