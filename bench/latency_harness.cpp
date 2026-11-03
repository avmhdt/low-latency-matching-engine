// llme-bench-latency (FR-23). In-process: each message is timed on its own with serialising TSC
// reads and filed under add, cancel, match or modify. Pipeline: messages enter at a fixed open-loop
// rate, and latency runs from each message's intended send time to the core finishing it, so a
// stall delays the clock of every message behind it instead of hiding them (coordinated omission).
//
//   llme-bench-latency --in <mlog> --engine d2 --warmup 1000000 --runs 5 --core 2 --out <dir>
//   llme-bench-latency --in <mlog> --engine d2 --pipeline --rate 1000000 --out <dir>
#include <array>
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "bench_common.hpp"
#include "cli_common.hpp"
#include "experiment_log.hpp"
#include "llme/pipeline.hpp"
#include "llme/spsc_ring.hpp"

namespace {

using llme::InMsg;
using llme::OutMsg;
using llme::bench::Histogram;
using nlohmann::json;

struct Options {
  std::string in;
  std::string engine = "d2";
  std::string out;
  std::uint64_t warmup = 1000000;
  std::uint64_t limit = 0;
  int runs = 5;
  std::optional<int> core;
  bool pipeline = false;
  double rate = 1000000.0;
};

enum class OpClass : std::uint8_t { Add, Cancel, Match, Modify, Other };
constexpr std::array<const char*, 4> kClassNames = {"add", "cancel", "match", "modify"};

// What one message produced, enough to file it under an op class.
struct Seen {
  bool fill = false;
  bool refused = false;  // REJECT, CANCEL_REJECT or MODIFY_REJECT
  bool canceled = false;
};

struct SeenSink {
  Seen* seen;
  void operator()(const OutMsg& r) const noexcept {
    const auto type = static_cast<llme::OutType>(r.type);
    seen->fill |= type == llme::OutType::Fill;
    seen->canceled |= type == llme::OutType::Canceled;
    seen->refused |= type == llme::OutType::Reject || type == llme::OutType::CancelReject ||
                     type == llme::OutType::ModifyReject;
  }
};

OpClass classify(const InMsg& m, const Seen& seen) noexcept {
  if (seen.refused) {
    return OpClass::Other;
  }
  switch (static_cast<llme::MsgType>(m.type)) {
    case llme::MsgType::New:
      if (seen.fill) {
        return OpClass::Match;
      }
      return seen.canceled ? OpClass::Other : OpClass::Add;
    case llme::MsgType::Cancel:
      return OpClass::Cancel;
    case llme::MsgType::Modify:
      return OpClass::Modify;
  }
  return OpClass::Other;
}

// One run in-process; histograms per op class, in nanoseconds.
template <class Store>
std::array<Histogram, 4> run_inproc(std::span<const InMsg> msgs, const llme::EngineConfig& cfg,
                                    std::uint64_t warmup, double ticks_per_ns) {
  Seen seen;
  llme::MatchingEngine<Store, SeenSink> engine(cfg, SeenSink{&seen});
  std::array<Histogram, 4> histograms;
  const std::uint64_t first = std::min<std::uint64_t>(warmup, msgs.size());
  for (std::uint64_t i = 0; i < first; ++i) {
    engine.on_message(msgs[i]);
  }
  for (std::uint64_t i = first; i < msgs.size(); ++i) {
    seen = {};
    const std::uint64_t t0 = llme::bench::tsc_begin();
    engine.on_message(msgs[i]);
    const std::uint64_t t1 = llme::bench::tsc_end();
    const OpClass op = classify(msgs[i], seen);
    if (op != OpClass::Other) {
      histograms[static_cast<std::size_t>(op)].record(
          static_cast<std::int64_t>(static_cast<double>(t1 - t0) / ticks_per_ns));
    }
  }
  return histograms;
}

struct Stamped {
  InMsg msg;
  std::uint64_t intended = 0;  // TSC
  std::uint64_t index = 0;
};
using StampedRing = llme::SpscRing<Stamped, llme::kInRingCapacity>;
constexpr std::uint64_t kEnd = ~std::uint64_t{0};

struct NullOut {
  void operator()(const OutMsg& /*r*/) const noexcept {}
};

// Open-loop pipeline: the gateway releases message i at start + i / rate, whether or not the core
// has caught up.
template <class Store>
Histogram run_pipeline(std::span<const InMsg> msgs, const llme::EngineConfig& cfg, double rate,
                       double ticks_per_ns, std::optional<int> core) {
  const auto ring = std::make_unique<StampedRing>();
  std::vector<std::uint64_t> done(msgs.size(), 0);
  std::vector<std::uint64_t> intended(msgs.size(), 0);
  llme::MatchingEngine<Store, NullOut> engine(cfg, NullOut{});
  std::atomic<bool> go{false};
  const double ticks_per_msg = ticks_per_ns * 1e9 / rate;

  std::thread core_thread([&] {
    (void)llme::bench::prepare_thread(core);
    while (!go.load(std::memory_order_acquire)) {
    }
    Stamped item;
    while (true) {
      if (!ring->try_pop(item)) {
        continue;
      }
      if (item.index == kEnd) {
        break;
      }
      engine.on_message(item.msg);
      done[item.index] = llme::bench::tsc_end();
    }
  });

  go.store(true, std::memory_order_release);
  const std::uint64_t start =
      llme::bench::tsc_begin() + static_cast<std::uint64_t>(ticks_per_ns * 1e6);
  for (std::uint64_t i = 0; i < msgs.size(); ++i) {
    const auto due = start + static_cast<std::uint64_t>(static_cast<double>(i) * ticks_per_msg);
    while (llme::bench::tsc_begin() < due) {
    }
    intended[i] = due;
    while (!ring->try_push({msgs[i], due, i})) {
    }
  }
  while (!ring->try_push({InMsg{}, 0, kEnd})) {
  }
  core_thread.join();

  Histogram latency;
  for (std::uint64_t i = 0; i < msgs.size(); ++i) {
    latency.record(
        static_cast<std::int64_t>(static_cast<double>(done[i] - intended[i]) / ticks_per_ns));
  }
  return latency;
}

int execute(const Options& options) {
  const auto results = llme::tools::results_dir();
  llme::bench::refuse_wsl_publish(results);
  auto log = llme::bench::load_log(options.in);
  auto msgs = log.reader.records();
  if (options.limit != 0 && options.limit < msgs.size()) {
    msgs = msgs.first(options.limit);
  }
  const auto thread = llme::bench::prepare_thread(options.core);
  const double ticks_per_ns = llme::bench::calibrate_ticks_per_ns();
  const double overhead = llme::bench::timer_overhead_p50_ns(ticks_per_ns);
  const std::string run_id = llme::bench::make_run_id(options.engine, log.name);
  const std::filesystem::path out =
      options.out.empty() ? results / "latency" / run_id : std::filesystem::path(options.out);
  std::filesystem::create_directories(out);

  json summary = {{"run_id", run_id},
                  {"design", options.engine},
                  {"workload", log.name},
                  {"mode", options.pipeline ? "pipeline" : "inproc"},
                  {"msgs", msgs.size()},
                  {"warmup", options.warmup},
                  {"runs", options.runs},
                  {"ticks_per_ns", ticks_per_ns},
                  {"timer_overhead_p50_ns", overhead},
                  {"thread", thread},
                  {"environment", llme::bench::environment_label()},
                  {"manifest_id", llme::tools::hostname()},
                  {"git_sha", llme::tools::git_sha()}};

  const std::string header =
      "run_id,design,workload,op_class,count,p50_ns,p90_ns,p99_ns,p999_ns,p9999_ns,max_ns,"
      "mean_ns,timer_overhead_p50_ns,manifest_id";
  const auto csv_row = [&](const std::string& op, const json& s) {
    return run_id + "," + options.engine + "," + log.name + "," + op + "," +
           std::to_string(s["count"].get<std::int64_t>()) + "," + s["p50_ns"].dump() + "," +
           s["p90_ns"].dump() + "," + s["p99_ns"].dump() + "," + s["p999_ns"].dump() + "," +
           s["p9999_ns"].dump() + "," + s["max_ns"].dump() + "," + s["mean_ns"].dump() + "," +
           std::to_string(overhead) + "," + llme::tools::hostname();
  };

  if (options.pipeline) {
    const Histogram latency = llme::bench::with_design(options.engine, [&]<class Store>() {
      return run_pipeline<Store>(msgs, log.config, options.rate, ticks_per_ns, options.core);
    });
    latency.write_hgrm(out / "pipeline.hgrm");
    summary["rate"] = options.rate;
    summary["pipeline"] = latency.summary();
    llme::bench::append_csv(results / "latency" / "summary.csv", header,
                            csv_row("pipeline", latency.summary()));
  } else {
    // Per class: each run's percentiles, the median across runs, and the merged histogram.
    std::array<std::vector<json>, 4> per_run;
    std::array<Histogram, 4> merged;
    for (int run = 0; run < options.runs; ++run) {
      const auto histograms = llme::bench::with_design(options.engine, [&]<class Store>() {
        return run_inproc<Store>(msgs, log.config, options.warmup, ticks_per_ns);
      });
      for (std::size_t c = 0; c < kClassNames.size(); ++c) {
        per_run[c].push_back(histograms[c].summary());
        merged[c].add(histograms[c]);
      }
    }
    for (std::size_t c = 0; c < kClassNames.size(); ++c) {
      json med;
      for (const char* key :
           {"count", "p50_ns", "p90_ns", "p99_ns", "p999_ns", "p9999_ns", "max_ns", "mean_ns"}) {
        std::vector<double> values;
        for (const auto& s : per_run[c]) {
          values.push_back(s[key].get<double>());
        }
        med[key] = llme::bench::median(values);
      }
      med["count"] = static_cast<std::int64_t>(med["count"].get<double>());
      std::vector<double> p50s;
      for (const auto& s : per_run[c]) {
        p50s.push_back(s["p50_ns"].get<double>());
      }
      const auto [lo, hi] = std::minmax_element(p50s.begin(), p50s.end());
      const double mid = med["p50_ns"].get<double>();
      // NFR-09: a p50 spread over 5% of the median is flagged, not hidden.
      const bool unstable = mid > 0.0 && (*hi - *lo) / mid > 0.05;
      summary[kClassNames[c]] = {{"median", med},
                                 {"runs", per_run[c]},
                                 {"p50_min_ns", *lo},
                                 {"p50_max_ns", *hi},
                                 {"unstable", unstable}};
      merged[c].write_hgrm(out / (std::string(kClassNames[c]) + ".hgrm"));
      llme::bench::append_csv(results / "latency" / "summary.csv", header,
                              csv_row(kClassNames[c], med));
    }
  }
  std::ofstream(out / "summary.json") << summary.dump(2) << '\n';
  llme::tools::append_experiment_row(
      {.tool = "llme-bench-latency",
       .args = "--in " + log.name + (options.pipeline ? " --pipeline" : ""),
       .design = options.engine,
       .workload = log.name,
       .input_hash = {},
       .output_hash = {},
       .n_msgs = msgs.size(),
       .headline_metric = "environment",
       .headline_value = llme::bench::environment_label(),
       .result_path = out.string()});
  std::cout << summary.dump() << '\n';
  return 0;
}

int run(int argc, char** argv) {
  CLI::App app{"Per-message latency by op class (FR-23)", "llme-bench-latency"};
  Options options;
  int core = -1;
  app.add_option("--in", options.in, "Input .mlog")->required();
  app.add_option("--engine", options.engine, "Design: d1, d2 or d3");
  app.add_option("--warmup", options.warmup, "Messages before timing starts");
  app.add_option("--runs", options.runs, "Repeated runs; the median is reported");
  app.add_option("--limit", options.limit, "Use only the first N messages");
  app.add_option("--core", core, "Pin the timed thread to this core");
  app.add_option("--out", options.out, "Output directory");
  app.add_flag("--pipeline", options.pipeline, "Open-loop pipeline latency instead");
  app.add_option("--rate", options.rate, "Pipeline input rate, messages per second");
  return llme::tools::parse_and_run(app, argc, argv, [&] {
    if (core >= 0) {
      options.core = core;
    }
    if (options.runs < 1 || options.rate <= 0.0) {
      throw std::runtime_error("--runs must be at least 1 and --rate positive");
    }
    return execute(options);
  });
}

}  // namespace

int main(int argc, char** argv) {
  return llme::tools::guarded_main([&] { return run(argc, argv); });
}
