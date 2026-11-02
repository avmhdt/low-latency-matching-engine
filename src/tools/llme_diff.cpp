// llme-diff: differential harness (FR-17). Streams generator output or an .mlog through an engine
// and the reference, compares every record and the final books, and on a mismatch writes a shrunk
// reproducer to tests/fixtures/regressions/.
//
//   llme-diff --campaign configs/diff_campaign.toml [--check-invariants] [--shrink] --out x.json
//   llme-diff --in x.mlog --engines d2,ref
//   llme-diff --profile configs/workloads/balanced.toml --ops 10000000 --seed 7 --engines d2,ref
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <toml++/toml.hpp>
#include <vector>

#include "cli_common.hpp"
#include "diff.hpp"
#include "engine_adapters.hpp"
#include "experiment_log.hpp"
#include "generator.hpp"
#include "llme/mlog.hpp"

namespace {

using nlohmann::json;
using std::chrono::steady_clock;

// With --check-invariants, how often the O(book + band) sweep runs (docs/decisions.md).
constexpr std::uint64_t kFullCheckEvery = 4096;

struct Options {
  std::string campaign;
  std::string in;
  std::string profile;
  std::string out;
  std::string config;
  std::string regression_dir = "tests/fixtures/regressions";
  std::vector<std::string> engines;
  std::uint64_t ops = 1000000;
  std::uint64_t seed = 42;
  std::uint32_t shrink_seconds = 300;
  bool check_invariants = false;
  bool shrink_failures = false;
  bool resume = false;
  bool json_output = false;
};

struct RunSpec {
  std::filesystem::path profile;
  std::uint64_t ops = 0;
  std::uint64_t seed = 0;
};

llme::EngineFactory factory(const std::string& name, const llme::EngineConfig& cfg) {
  return [name, cfg] { return llme::make_adapter(name, cfg); };
}

void save_regression(const std::vector<llme::InMsg>& msgs, const llme::EngineConfig& cfg,
                     const Options& options, const std::string& name) {
  const std::filesystem::path dir = options.regression_dir;
  std::filesystem::create_directories(dir);
  auto writer = llme::MlogWriter::create(dir / (name + ".mlog"));
  if (!writer) {
    throw std::runtime_error("cannot write regression fixture");
  }
  for (const llme::InMsg& m : msgs) {
    if (!writer->write(m)) {
      throw std::runtime_error("cannot write regression fixture");
    }
  }
  if (!writer->close()) {
    throw std::runtime_error("cannot write regression fixture");
  }
  std::ofstream(dir / (name + ".toml"))
      << "[engine]\nprice_min = " << cfg.price_min << "\nprice_max = " << cfg.price_max
      << "\nmax_orders = " << cfg.max_orders << '\n';
  std::cerr << "reproducer: " << (dir / (name + ".mlog")).string() << " (" << msgs.size()
            << " messages)\n";
}

// Shrinks the first failing prefix; the message source is replayed by the caller.
void handle_failure(const std::vector<llme::InMsg>& prefix, const llme::EngineConfig& cfg,
                    const std::string& name_a, const std::string& name_b, const Options& options,
                    const std::string& tag) {
  const auto a = factory(name_a, cfg);
  const auto b = factory(name_b, cfg);
  const std::uint64_t every = options.check_invariants ? kFullCheckEvery : 0;
  const auto minimal = llme::shrink(
      prefix,
      [&](const std::vector<llme::InMsg>& candidate) {
        return llme::first_mismatch(candidate, a, b, every).has_value();
      },
      std::chrono::seconds(options.shrink_seconds));
  save_regression(minimal, cfg, options, tag);
}

json summarize(const llme::DiffStats& stats, const std::string& workload, std::uint64_t seed,
               std::uint64_t ops, const std::string& engines, double elapsed) {
  return {{"profile", workload},
          {"seed", seed},
          {"ops", ops},
          {"fills", stats.fills},
          {"rejects", stats.rejects},
          {"mismatches", stats.mismatches},
          {"invariant_failures", stats.invariant_failures},
          {"full_checks", stats.full_checks},
          {"first_mismatch_index",
           stats.first_mismatch_index ? json(*stats.first_mismatch_index) : json(nullptr)},
          {"first_mismatch", stats.first_mismatch},
          {"elapsed_s", elapsed},
          {"engines", engines}};
}

// One engine against the reference over a generated stream.
json run_generated(const RunSpec& spec, const std::string& engine, const Options& options) {
  const auto profile = llme::workload::load_profile(spec.profile);
  llme::workload::Generator generator(profile, spec.seed);
  llme::DiffRunner runner(llme::make_adapter(engine, profile.engine),
                          llme::make_adapter("ref", profile.engine));
  runner.enable_invariants(options.check_invariants ? kFullCheckEvery : 0);

  const auto start = steady_clock::now();
  bool clean = true;
  for (std::uint64_t i = 0; i < spec.ops && clean; ++i) {
    clean = runner.step(generator.next());
  }
  if (clean) {
    runner.finish();
  }
  const std::chrono::duration<double> elapsed = steady_clock::now() - start;
  const auto& stats = runner.stats();

  if (stats.first_mismatch_index) {
    std::cerr << "MISMATCH " << profile.name << " seed " << spec.seed << ": "
              << stats.first_mismatch << '\n';
    if (options.shrink_failures) {
      std::vector<llme::InMsg> prefix;
      llme::workload::Generator replay(profile, spec.seed);
      const std::uint64_t last = stats.first_mismatch_index.value_or(spec.ops);
      for (std::uint64_t i = 0; i <= last && i < spec.ops; ++i) {
        prefix.push_back(replay.next());
      }
      handle_failure(prefix, profile.engine, engine, "ref", options,
                     "diff_" + profile.name + "_seed" + std::to_string(spec.seed));
    }
  }
  return summarize(stats, profile.name, spec.seed, spec.ops, engine + ",ref", elapsed.count());
}

std::vector<RunSpec> load_campaign(const Options& options, std::vector<std::string>& engines) {
  const toml::table root = toml::parse_file(options.campaign);
  if (const auto* list = root["engines"].as_array()) {
    for (const auto& node : *list) {
      engines.push_back(node.value_or(std::string{}));
    }
  }
  std::vector<RunSpec> runs;
  if (const auto* list = root["run"].as_array()) {
    for (const auto& node : *list) {
      const auto* run = node.as_table();
      if (run == nullptr) {
        throw std::runtime_error("run entries must be tables: " + options.campaign);
      }
      const std::int64_t ops = (*run)["ops"].value_or(std::int64_t{0});
      const std::int64_t seed = (*run)["seed"].value_or(std::int64_t{0});
      runs.push_back({(*run)["profile"].value_or(std::string{}), static_cast<std::uint64_t>(ops),
                      static_cast<std::uint64_t>(seed)});
    }
  }
  return runs;
}

bool already_done(const json& report, const RunSpec& spec, const std::string& engine) {
  const auto workload = spec.profile.stem().string();
  for (const auto& run : report["runs"]) {
    if (run["profile"] == workload && run["seed"] == spec.seed && run["ops"] == spec.ops &&
        run["engines"] == engine + ",ref" && run["mismatches"] == 0) {
      return true;
    }
  }
  return false;
}

void write_report(const json& report, const std::string& path) {
  if (path.empty()) {
    return;
  }
  const std::string partial = path + ".tmp";
  std::ofstream(partial) << report.dump(2) << '\n';
  std::filesystem::rename(partial, path);
}

int run_campaign(const Options& options) {
  std::vector<std::string> engines;
  const auto runs = load_campaign(options, engines);
  json report = {{"campaign", options.campaign},
                 {"git_sha", llme::tools::git_sha()},
                 {"check_invariants", options.check_invariants},
                 {"runs", json::array()}};
  if (options.resume && !options.out.empty() && std::filesystem::exists(options.out)) {
    std::ifstream(options.out) >> report;
  }

  std::uint64_t mismatches = 0;
  std::uint64_t total_ops = 0;
  for (const auto& engine : engines) {
    for (const auto& spec : runs) {
      if (already_done(report, spec, engine)) {
        total_ops += spec.ops;
        continue;
      }
      json result = run_generated(spec, engine, options);
      mismatches += result["mismatches"].get<std::uint64_t>();
      mismatches += result["invariant_failures"].get<std::uint64_t>();
      total_ops += spec.ops;
      std::cerr << result.dump() << '\n';
      report["runs"].push_back(std::move(result));
      write_report(report, options.out);
    }
  }

  if (!options.out.empty()) {
    llme::tools::append_experiment_row({.tool = "llme-diff",
                                        .args = "--campaign " + options.campaign,
                                        .design = "campaign",
                                        .workload = "all",
                                        .input_hash = {},
                                        .output_hash = {},
                                        .n_msgs = total_ops,
                                        .headline_metric = "mismatches",
                                        .headline_value = std::to_string(mismatches),
                                        .result_path = options.out});
  }
  if (options.json_output) {
    std::cout << report.dump() << '\n';
  }
  return mismatches == 0 ? 0 : 1;
}

int run_file(const Options& options) {
  auto reader = llme::MlogReader::open(options.in);
  if (!reader) {
    throw std::runtime_error(std::string(llme::to_string(reader.error())) + ": " + options.in);
  }
  const llme::EngineConfig config = options.config.empty()
                                        ? llme::EngineConfig{}
                                        : llme::workload::load_engine_config(options.config);
  const std::string name_a = options.engines.at(0);
  const std::string name_b = options.engines.at(1);
  llme::DiffRunner runner(llme::make_adapter(name_a, config), llme::make_adapter(name_b, config));
  runner.enable_invariants(options.check_invariants ? kFullCheckEvery : 0);
  const auto start = steady_clock::now();
  bool clean = true;
  std::uint64_t index = 0;
  for (const llme::InMsg& m : reader->records()) {
    clean = runner.step(m);
    if (!clean) {
      break;
    }
    ++index;
  }
  if (clean) {
    runner.finish();
  }
  const std::chrono::duration<double> elapsed = steady_clock::now() - start;
  const auto& stats = runner.stats();
  if (stats.first_mismatch_index) {
    std::cerr << "MISMATCH: " << stats.first_mismatch << '\n';
    if (options.shrink_failures) {
      const auto records = reader->records();
      const std::size_t count = std::min<std::size_t>(index + 1, records.size());
      handle_failure({records.begin(), records.begin() + static_cast<std::ptrdiff_t>(count)},
                     config, name_a, name_b, options,
                     "diff_" + std::filesystem::path(options.in).stem().string());
    }
  }
  const json summary =
      summarize(stats, std::filesystem::path(options.in).stem().string(), 0,
                reader->header().record_count, name_a + "," + name_b, elapsed.count());
  std::cout << summary.dump() << '\n';
  return stats.first_mismatch_index ? 1 : 0;
}

int run_profile(const Options& options) {
  const std::filesystem::path profile =
      options.profile.empty() ? "configs/workloads/balanced.toml" : options.profile;
  const json summary =
      run_generated({profile, options.ops, options.seed}, options.engines.at(0), options);
  std::cout << summary.dump() << '\n';
  return summary["first_mismatch_index"].is_null() ? 0 : 1;
}

int run(int argc, char** argv) {
  CLI::App app{"Compare engines record by record", "llme-diff"};
  Options options;
  std::string engines = "ref,ref";
  app.add_option("--campaign", options.campaign, "Campaign TOML");
  app.add_option("--in", options.in, "Input .mlog");
  app.add_option("--profile", options.profile, "Workload profile for a single generated run");
  app.add_option("--ops", options.ops, "Messages in a single generated run");
  app.add_option("--seed", options.seed, "Seed of a single generated run");
  app.add_option("--engines", engines, "Comma-separated pair; generated runs use the first vs ref");
  app.add_option("--config", options.config, "Engine config TOML for --in");
  app.add_option("--out", options.out, "Write the JSON report here");
  app.add_option("--regression-dir", options.regression_dir, "Where shrunk reproducers go");
  app.add_option("--shrink-seconds", options.shrink_seconds, "Time budget for one shrink");
  app.add_flag("--check-invariants", options.check_invariants, "Check book invariants");
  app.add_flag("--shrink", options.shrink_failures, "Shrink a failure to a minimal reproducer");
  app.add_flag("--resume", options.resume, "Skip campaign runs already clean in --out");
  app.add_flag("--json", options.json_output, "Print the JSON report on stdout");

  return llme::tools::parse_and_run(app, argc, argv, [&] {
    std::stringstream names(engines);
    for (std::string name; std::getline(names, name, ',');) {
      options.engines.push_back(name);
    }
    if (options.engines.size() != 2) {
      throw std::runtime_error("--engines takes two names");
    }
    if (!options.campaign.empty()) {
      return run_campaign(options);
    }
    if (!options.in.empty()) {
      return run_file(options);
    }
    return run_profile(options);
  });
}

}  // namespace

int main(int argc, char** argv) {
  return llme::tools::guarded_main([&] { return run(argc, argv); });
}
