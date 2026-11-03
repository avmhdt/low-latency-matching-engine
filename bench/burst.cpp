// llme-bench-burst (FR-28): what a burst of cancels does to the tail. The same workload runs once
// without bursts and then with bursts of each length, aimed at uniform and at near-touch orders.
// Every message is timed; tails are reported per op class inside and outside the bursts, with the
// levels emptied and the ID-map probe lengths, sampled between phases.
//
//   llme-bench-burst --profile configs/workloads/burst.toml --burst 1000,10000,100000 --out <dir>
#include <array>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include "bench_common.hpp"
#include "cli_common.hpp"
#include "experiment_log.hpp"
#include "generator.hpp"

namespace {

using llme::InMsg;
using llme::OutMsg;
using llme::bench::Histogram;
using nlohmann::json;

struct Options {
  std::string profile = "configs/workloads/burst.toml";
  std::string bursts = "1000,10000,100000";
  std::string engine = "d2";
  std::string out;
  std::uint64_t ops = 4000000;
  std::uint64_t every = 1000000;
  std::optional<int> core;
};

constexpr std::array<const char*, 3> kClasses = {"add", "cancel", "match"};

struct Seen {
  bool fill = false;
  bool refused = false;
  bool canceled = false;
};

struct SeenSink {
  Seen* seen;
  void operator()(const OutMsg& r) const noexcept {
    const auto type = static_cast<llme::OutType>(r.type);
    seen->fill |= type == llme::OutType::Fill;
    seen->canceled |= type == llme::OutType::Canceled;
    seen->refused |= type == llme::OutType::Reject || type == llme::OutType::CancelReject;
  }
};

// Index into kClasses, or -1 for a message outside them.
int classify(const InMsg& m, const Seen& seen) {
  if (seen.refused) {
    return -1;
  }
  if (m.type == llme::to_wire(llme::MsgType::New)) {
    return seen.fill ? 2 : (seen.canceled ? -1 : 0);
  }
  return m.type == llme::to_wire(llme::MsgType::Cancel) ? 1 : -1;
}

struct Phase {
  std::array<Histogram, 3> inside;
  std::array<Histogram, 3> outside;
  std::uint64_t levels_emptied_inside = 0;
  std::uint64_t levels_emptied_outside = 0;
  double probe_mean_max = 0.0;
  std::uint32_t probe_max = 0;
};

template <class Store>
Phase run_phase(const llme::workload::Profile& profile, std::uint64_t ops, double ticks_per_ns) {
  // Messages first, so the timed loop runs on a ready array.
  llme::workload::Generator generator(profile, profile.seed);
  std::vector<InMsg> msgs;
  std::vector<bool> in_burst;
  msgs.reserve(ops);
  in_burst.reserve(ops);
  for (std::uint64_t i = 0; i < ops; ++i) {
    msgs.push_back(generator.next());
    in_burst.push_back(generator.last_was_burst());
  }

  Seen seen;
  llme::MatchingEngine<Store, SeenSink> engine(profile.engine, SeenSink{&seen});
  Phase phase;
  bool was_inside = false;
  for (std::uint64_t i = 0; i < msgs.size(); ++i) {
    const bool inside = in_burst[i];
    if (inside != was_inside) {
      const auto probes = engine.id_map_probe_stats();
      phase.probe_mean_max = std::max(phase.probe_mean_max, probes.mean);
      phase.probe_max = std::max(phase.probe_max, probes.max);
      was_inside = inside;
    }
    seen = {};
    const std::uint64_t emptied = engine.levels_emptied();
    const std::uint64_t t0 = llme::bench::tsc_begin();
    engine.on_message(msgs[i]);
    const std::uint64_t t1 = llme::bench::tsc_end();
    (inside ? phase.levels_emptied_inside : phase.levels_emptied_outside) +=
        engine.levels_emptied() - emptied;
    const int c = classify(msgs[i], seen);
    if (c >= 0) {
      auto& histograms = inside ? phase.inside : phase.outside;
      histograms[static_cast<std::size_t>(c)].record(
          static_cast<std::int64_t>(static_cast<double>(t1 - t0) / ticks_per_ns));
    }
  }
  const auto probes = engine.id_map_probe_stats();  // the end state, sampled for every phase
  phase.probe_mean_max = std::max(phase.probe_mean_max, probes.mean);
  phase.probe_max = std::max(phase.probe_max, probes.max);
  return phase;
}

int execute(const Options& options) {
  const auto results = llme::tools::results_dir();
  llme::bench::refuse_wsl_publish(results);
  const auto base = llme::workload::load_profile(options.profile);
  const auto thread = llme::bench::prepare_thread(options.core);
  const double ticks_per_ns = llme::bench::calibrate_ticks_per_ns();
  const std::string run_id = llme::bench::make_run_id(options.engine, "burst");
  const std::filesystem::path out =
      options.out.empty() ? results / "burst" / run_id : std::filesystem::path(options.out);
  std::filesystem::create_directories(out);

  struct Config {
    std::string label;
    std::uint64_t length = 0;
    llme::workload::CancelTarget target = llme::workload::CancelTarget::Uniform;
  };
  std::vector<Config> configs = {{"baseline", 0, llme::workload::CancelTarget::Uniform}};
  std::stringstream lengths(options.bursts);
  for (std::string item; std::getline(lengths, item, ',');) {
    const auto length = std::stoull(item);
    configs.push_back({"uniform", length, llme::workload::CancelTarget::Uniform});
    configs.push_back({"near_touch", length, llme::workload::CancelTarget::NearTouch});
  }

  const std::string header =
      "run_id,design,burst_length,target,op_class,window,count,p99_ns,p999_ns,max_ns,"
      "levels_emptied,probe_mean_max,probe_max,manifest_id";
  json summary = {{"run_id", run_id},
                  {"design", options.engine},
                  {"profile", base.name},
                  {"ops", options.ops},
                  {"every", options.every},
                  {"thread", thread},
                  {"environment", llme::bench::environment_label()},
                  {"manifest_id", llme::tools::hostname()},
                  {"git_sha", llme::tools::git_sha()},
                  {"phases", json::array()}};

  for (const Config& config : configs) {
    llme::workload::Profile profile = base;
    profile.burst_enabled = config.length != 0;
    profile.burst_length = config.length;
    profile.burst_every = options.every;
    profile.burst_target = config.target;
    const Phase phase = llme::bench::with_design(options.engine, [&]<class Store>() {
      return run_phase<Store>(profile, options.ops, ticks_per_ns);
    });
    json entry = {{"target", config.label},
                  {"burst_length", config.length},
                  {"levels_emptied_inside", phase.levels_emptied_inside},
                  {"levels_emptied_outside", phase.levels_emptied_outside},
                  {"probe_mean_max", phase.probe_mean_max},
                  {"probe_max", phase.probe_max}};
    for (std::size_t c = 0; c < kClasses.size(); ++c) {
      for (const bool inside : {false, true}) {
        const Histogram& h = inside ? phase.inside[c] : phase.outside[c];
        const std::string window =
            config.length == 0 ? "baseline" : (inside ? "inside" : "outside");
        if (h.count() == 0) {
          continue;
        }
        entry[std::string(kClasses[c]) + "_" + window] = h.summary();
        llme::bench::append_csv(
            results / "burst" / "summary.csv", header,
            run_id + "," + options.engine + "," + std::to_string(config.length) + "," +
                config.label + "," + kClasses[c] + "," + window + "," + std::to_string(h.count()) +
                "," + std::to_string(h.percentile(99.0)) + "," +
                std::to_string(h.percentile(99.9)) + "," + std::to_string(h.max()) + "," +
                std::to_string(inside ? phase.levels_emptied_inside
                                      : phase.levels_emptied_outside) +
                "," + std::to_string(phase.probe_mean_max) + "," + std::to_string(phase.probe_max) +
                "," + llme::tools::hostname());
      }
    }
    summary["phases"].push_back(entry);
  }

  std::ofstream(out / "summary.json") << summary.dump(2) << '\n';
  llme::tools::append_experiment_row({.tool = "llme-bench-burst",
                                      .args = "--burst " + options.bursts,
                                      .design = options.engine,
                                      .workload = base.name,
                                      .input_hash = {},
                                      .output_hash = {},
                                      .n_msgs = options.ops * configs.size(),
                                      .headline_metric = "environment",
                                      .headline_value = llme::bench::environment_label(),
                                      .result_path = out.string()});
  std::cout << summary.dump() << '\n';
  return 0;
}

int run(int argc, char** argv) {
  CLI::App app{"Cancel bursts and the latency tail (FR-28)", "llme-bench-burst"};
  Options options;
  int core = -1;
  app.add_option("--profile", options.profile, "Workload profile");
  app.add_option("--burst", options.bursts, "Burst lengths, comma-separated");
  app.add_option("--engine", options.engine, "Design: d1, d2 or d3");
  app.add_option("--ops", options.ops, "Messages per phase");
  app.add_option("--every", options.every, "Messages between bursts");
  app.add_option("--core", core, "Pin the timed thread to this core");
  app.add_option("--out", options.out, "Output directory");
  return llme::tools::parse_and_run(app, argc, argv, [&] {
    if (core >= 0) {
      options.core = core;
    }
    return execute(options);
  });
}

}  // namespace

int main(int argc, char** argv) {
  return llme::tools::guarded_main([&] { return run(argc, argv); });
}
