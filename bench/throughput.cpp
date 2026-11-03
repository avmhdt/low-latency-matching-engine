// llme-bench-throughput (FR-25): messages per second per core, one engine per symbol in one thread.
//
//   llme-bench-throughput --in <mlog> --engine d2 --mode inproc|pipeline --runs 5 --out <dir>
//   llme-bench-throughput --interleave <dir>/itch_top_<date>.interleave --engine d2 ...
//
// With --interleave, the symbols' messages are replayed in their original order across symbols,
// as llme-itch-decode recorded it, so all books stay live together as they did on the day.
// Several --in logs without it run one after another, which is labelled in the output.
// --perf-control ctl,ack lets perf stat count only the timed loop (scripts/perf_stat.sh).
#include <fcntl.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <sstream>
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
using nlohmann::json;
using Clock = std::chrono::steady_clock;

struct Options {
  std::string in;
  std::string interleave;
  std::string engine = "d2";
  std::string mode = "inproc";
  std::string out;
  std::string perf_control;
  std::uint64_t limit = 0;
  int runs = 5;
  std::optional<int> core;
};

struct Workload {
  std::vector<llme::bench::LoadedLog> logs;
  std::vector<std::uint16_t> order;  // symbol per message; empty means one log after another
  std::uint64_t msgs = 0;
  std::string name;
};

Workload load(const Options& options) {
  Workload w;
  if (!options.interleave.empty()) {
    const std::filesystem::path path = options.interleave;
    std::ifstream meta(path.string() + ".json");
    if (!meta) {
      throw std::runtime_error("no " + path.string() + ".json next to the interleave file");
    }
    const json index = json::parse(meta);
    for (const auto& name : index.at("logs")) {
      w.logs.push_back(llme::bench::load_log(path.parent_path() / name.get<std::string>()));
    }
    const auto count = index.at("count").get<std::uint64_t>();
    w.order.resize(count);
    std::ifstream bytes(path, std::ios::binary);
    bytes.read(reinterpret_cast<char*>(w.order.data()),  // NOLINT: raw u16 file
               static_cast<std::streamsize>(count * sizeof(std::uint16_t)));
    if (!bytes) {
      throw std::runtime_error("short interleave file: " + path.string());
    }
    w.name = "top" + std::to_string(w.logs.size());
  } else {
    std::stringstream list(options.in);
    for (std::string item; std::getline(list, item, ',');) {
      w.logs.push_back(llme::bench::load_log(item));
    }
    w.name = w.logs.size() == 1 ? w.logs[0].name : "concat" + std::to_string(w.logs.size());
  }
  if (w.logs.empty()) {
    throw std::runtime_error("no input: pass --in or --interleave");
  }
  for (const auto& log : w.logs) {
    w.msgs += log.reader.records().size();
  }
  if (!w.order.empty() && w.order.size() != w.msgs) {
    throw std::runtime_error("interleave count differs from the logs");
  }
  if (options.limit != 0) {
    w.msgs = std::min(w.msgs, options.limit);
  }
  return w;
}

// perf stat --control: counting starts and stops around the timed loop only.
class PerfControl {
 public:
  explicit PerfControl(const std::string& spec) {
    if (spec.empty()) {
      return;
    }
    const auto comma = spec.find(',');
    ctl_ = ::open(spec.substr(0, comma).c_str(), O_WRONLY | O_CLOEXEC);   // NOLINT: POSIX
    ack_ = ::open(spec.substr(comma + 1).c_str(), O_RDONLY | O_CLOEXEC);  // NOLINT: POSIX
    if (ctl_ < 0 || ack_ < 0) {
      throw std::runtime_error("cannot open the perf control fifos: " + spec);
    }
  }
  PerfControl(const PerfControl&) = delete;
  PerfControl& operator=(const PerfControl&) = delete;
  PerfControl(PerfControl&&) = delete;
  PerfControl& operator=(PerfControl&&) = delete;
  ~PerfControl() {
    if (ctl_ >= 0) {
      ::close(ctl_);
    }
    if (ack_ >= 0) {
      ::close(ack_);
    }
  }

  void send(const char* command) const {
    if (ctl_ < 0) {
      return;
    }
    const std::string line = std::string(command) + "\n";
    std::array<char, 16> reply{};
    if (::write(ctl_, line.data(), line.size()) < 0 ||
        ::read(ack_, reply.data(), reply.size()) < 0) {
      throw std::runtime_error("perf control failed");
    }
  }

 private:
  int ctl_ = -1;
  int ack_ = -1;
};

struct CountSink {
  std::uint64_t* records;
  void operator()(const OutMsg& /*r*/) const noexcept { ++*records; }
};

// Calls f(symbol, message) for each message in replay order.
template <class F>
void for_each_message(const Workload& w, const F& f) {
  std::uint64_t left = w.msgs;
  if (w.order.empty()) {
    for (std::uint16_t s = 0; s < w.logs.size() && left != 0; ++s) {
      for (const InMsg& m : w.logs[s].reader.records()) {
        if (left-- == 0) {
          return;
        }
        f(s, m);
      }
    }
    return;
  }
  std::vector<std::size_t> cursor(w.logs.size(), 0);
  const std::vector<std::span<const InMsg>> records = [&] {
    std::vector<std::span<const InMsg>> out;
    for (const auto& log : w.logs) {
      out.push_back(log.reader.records());
    }
    return out;
  }();
  for (std::uint64_t i = 0; i < w.msgs; ++i) {
    const std::uint16_t s = w.order[i];
    f(s, records[s][cursor[s]++]);
  }
}

template <class Store>
double run_inproc(const Workload& w, const PerfControl& perf) {
  std::uint64_t records = 0;
  using Engine = llme::MatchingEngine<Store, CountSink>;
  std::vector<std::unique_ptr<Engine>> engines;
  for (const auto& log : w.logs) {
    engines.push_back(std::make_unique<Engine>(log.config, CountSink{&records}));
  }
  perf.send("enable");
  const auto start = Clock::now();
  for_each_message(w, [&](std::uint16_t s, const InMsg& m) { engines[s]->on_message(m); });
  const std::chrono::duration<double> elapsed = Clock::now() - start;
  perf.send("disable");
  if (records == 0 && w.msgs != 0) {
    throw std::runtime_error("no output records");
  }
  return elapsed.count();
}

struct Routed {
  InMsg msg;
  std::uint32_t symbol = 0;
};
constexpr std::uint32_t kEndSymbol = ~std::uint32_t{0};
using RoutedRing = llme::SpscRing<Routed, llme::kInRingCapacity>;

struct RingSink {
  llme::OutRing* ring;
  void operator()(const OutMsg& r) const noexcept {
    while (!ring->try_push(r)) {
    }
  }
};

// Gateway, core and publisher threads, as in FR-14, with one engine per symbol on the core.
template <class Store>
double run_pipeline(const Workload& w, const PerfControl& perf) {
  const auto in = std::make_unique<RoutedRing>();
  const auto out = std::make_unique<llme::OutRing>();
  using Engine = llme::MatchingEngine<Store, RingSink>;
  std::vector<std::unique_ptr<Engine>> engines;
  for (const auto& log : w.logs) {
    engines.push_back(std::make_unique<Engine>(log.config, RingSink{out.get()}));
  }
  const llme::PipelineCores cores = llme::cores_from_env();
  std::atomic<int> ready{0};
  std::atomic<bool> go{false};
  const auto enter = [&](std::optional<int> core) {
    if (core) {
      (void)llme::pin_current_thread(*core);
    }
    ready.fetch_add(1);
    while (!go.load(std::memory_order_acquire)) {
    }
  };

  std::thread core_thread([&] {
    enter(cores.core);
    Routed item;
    while (true) {
      if (!in->try_pop(item)) {
        continue;
      }
      if (item.symbol == kEndSymbol) {
        break;
      }
      engines[item.symbol]->on_message(item.msg);
    }
    OutMsg end;
    while (!out->try_push(end)) {
    }
  });
  std::uint64_t published = 0;
  std::thread publisher([&] {
    enter(cores.publisher);
    std::array<OutMsg, 1024> batch;
    while (true) {
      const std::size_t n = out->pop_batch(batch.data(), batch.size());
      for (std::size_t i = 0; i < n; ++i) {
        if (batch[i].type == 0) {
          return;
        }
        ++published;
      }
    }
  });

  if (cores.gateway) {
    (void)llme::pin_current_thread(*cores.gateway);
  }
  while (ready.load() != 2) {
  }
  perf.send("enable");
  const auto start = Clock::now();
  go.store(true, std::memory_order_release);
  for_each_message(w, [&](std::uint16_t s, const InMsg& m) {
    while (!in->try_push({m, s})) {
    }
  });
  while (!in->try_push({InMsg{}, kEndSymbol})) {
  }
  core_thread.join();
  publisher.join();
  const std::chrono::duration<double> elapsed = Clock::now() - start;
  perf.send("disable");
  if (published == 0 && w.msgs != 0) {
    throw std::runtime_error("no output records");
  }
  return elapsed.count();
}

int execute(const Options& options) {
  const auto results = llme::tools::results_dir();
  llme::bench::refuse_wsl_publish(results);
  const Workload w = load(options);
  const PerfControl perf(options.perf_control);
  const auto thread = llme::bench::prepare_thread(options.core);

  std::vector<double> seconds;
  for (int run = 0; run < options.runs; ++run) {
    seconds.push_back(llme::bench::with_design(options.engine, [&]<class Store>() {
      return options.mode == "pipeline" ? run_pipeline<Store>(w, perf) : run_inproc<Store>(w, perf);
    }));
  }
  const double med = llme::bench::median(seconds);
  const double per_core = med > 0.0 ? static_cast<double>(w.msgs) / med : 0.0;
  const std::string run_id = llme::bench::make_run_id(options.engine, w.name + "_" + options.mode);

  std::vector<double> rates;
  for (const double s : seconds) {
    rates.push_back(s > 0.0 ? static_cast<double>(w.msgs) / s : 0.0);
  }
  const json summary = {
      {"run_id", run_id},
      {"design", options.engine},
      {"workload", w.name},
      {"mode", options.mode},
      {"symbols", w.logs.size()},
      {"order", w.order.empty() ? (w.logs.size() == 1 ? "single" : "concatenated") : "interleaved"},
      {"msgs", w.msgs},
      {"runs_seconds", seconds},
      {"median_seconds", med},
      {"msgs_per_sec_per_core", per_core},
      {"msgs_per_sec_min", *std::min_element(rates.begin(), rates.end())},
      {"msgs_per_sec_max", *std::max_element(rates.begin(), rates.end())},
      {"matching_cores", 1},
      {"thread", thread},
      {"environment", llme::bench::environment_label()},
      {"manifest_id", llme::tools::hostname()},
      {"git_sha", llme::tools::git_sha()}};

  const std::filesystem::path out =
      options.out.empty() ? results / "throughput" / run_id : std::filesystem::path(options.out);
  std::filesystem::create_directories(out);
  std::ofstream(out / "summary.json") << summary.dump(2) << '\n';
  llme::bench::append_csv(
      results / "throughput" / "summary.csv",
      "run_id,design,workload,mode,symbols,msgs,seconds,msgs_per_sec_per_core,manifest_id",
      run_id + "," + options.engine + "," + w.name + "," + options.mode + "," +
          std::to_string(w.logs.size()) + "," + std::to_string(w.msgs) + "," + std::to_string(med) +
          "," + std::to_string(per_core) + "," + llme::tools::hostname());
  llme::tools::append_experiment_row({.tool = "llme-bench-throughput",
                                      .args = "--mode " + options.mode,
                                      .design = options.engine,
                                      .workload = w.name,
                                      .input_hash = {},
                                      .output_hash = {},
                                      .n_msgs = w.msgs,
                                      .headline_metric = "environment",
                                      .headline_value = llme::bench::environment_label(),
                                      .result_path = out.string()});
  std::cout << summary.dump() << '\n';
  return 0;
}

int run(int argc, char** argv) {
  CLI::App app{"Messages per second per core (FR-25)", "llme-bench-throughput"};
  Options options;
  int core = -1;
  app.add_option("--in", options.in, "Input .mlog, or several separated by commas");
  app.add_option("--interleave", options.interleave, "Interleave file from llme-itch-decode");
  app.add_option("--engine", options.engine, "Design: d1, d2 or d3");
  app.add_option("--mode", options.mode, "inproc or pipeline");
  app.add_option("--runs", options.runs, "Repeated runs; the median is reported");
  app.add_option("--limit", options.limit, "Use only the first N messages");
  app.add_option("--core", core, "Pin the in-process thread to this core");
  app.add_option("--out", options.out, "Output directory");
  app.add_option("--perf-control", options.perf_control, "perf stat control fifos: ctl,ack");
  return llme::tools::parse_and_run(app, argc, argv, [&] {
    if (core >= 0) {
      options.core = core;
    }
    if (options.mode != "inproc" && options.mode != "pipeline") {
      throw std::runtime_error("--mode is inproc or pipeline");
    }
    if (options.runs < 1) {
      throw std::runtime_error("--runs must be at least 1");
    }
    return execute(options);
  });
}

}  // namespace

int main(int argc, char** argv) {
  return llme::tools::guarded_main([&] { return run(argc, argv); });
}
