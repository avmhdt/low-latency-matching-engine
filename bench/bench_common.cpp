#include "bench_common.hpp"

#include <sys/mman.h>
#include <sys/resource.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <fstream>

#if defined(__x86_64__)
#include <x86intrin.h>
#endif

#include "llme/pipeline.hpp"
#include "sidecar.hpp"

namespace llme::bench {

std::uint64_t tsc_begin() noexcept {
#if defined(__x86_64__)
  _mm_lfence();
  return __rdtsc();
#else
  return static_cast<std::uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count());
#endif
}

std::uint64_t tsc_end() noexcept {
#if defined(__x86_64__)
  unsigned aux = 0;
  const std::uint64_t t = __rdtscp(&aux);
  _mm_lfence();
  return t;
#else
  return static_cast<std::uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count());
#endif
}

double calibrate_ticks_per_ns() {
#if defined(__x86_64__)
  using Clock = std::chrono::steady_clock;
  const auto start_time = Clock::now();
  const std::uint64_t start_tsc = tsc_begin();
  while (Clock::now() - start_time < std::chrono::milliseconds(200)) {
  }
  const std::uint64_t ticks = tsc_end() - start_tsc;
  const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - start_time);
  return static_cast<double>(ticks) / static_cast<double>(ns.count());
#else
  return 1.0;
#endif
}

Histogram::Histogram() {
  constexpr std::int64_t kHighestNs = 10000000000;  // 10 s
  if (hdr_init(1, kHighestNs, 3, &h_) != 0) {
    throw std::runtime_error("cannot allocate a histogram");
  }
}

Histogram::~Histogram() {
  if (h_ != nullptr) {
    hdr_close(h_);
  }
}

void Histogram::record(std::int64_t ns) noexcept {
  hdr_record_value(h_, std::max<std::int64_t>(ns, 1));
}

void Histogram::add(const Histogram& other) noexcept { (void)hdr_add(h_, other.h_); }

std::int64_t Histogram::count() const noexcept { return h_->total_count; }

std::int64_t Histogram::percentile(double p) const noexcept {
  return hdr_value_at_percentile(h_, p);
}

std::int64_t Histogram::max() const noexcept { return hdr_max(h_); }

double Histogram::mean() const noexcept { return hdr_mean(h_); }

void Histogram::write_hgrm(const std::filesystem::path& path) const {
  std::FILE* file = std::fopen(path.c_str(), "w");
  if (file == nullptr) {
    throw std::runtime_error("cannot write " + path.string());
  }
  hdr_percentiles_print(h_, file, 5, 1.0, CLASSIC);
  std::fclose(file);
}

nlohmann::json Histogram::summary() const {
  return {{"count", count()},
          {"p50_ns", percentile(50.0)},
          {"p90_ns", percentile(90.0)},
          {"p99_ns", percentile(99.0)},
          {"p999_ns", percentile(99.9)},
          {"p9999_ns", percentile(99.99)},
          {"max_ns", max()},
          {"mean_ns", mean()}};
}

double timer_overhead_p50_ns(double ticks_per_ns) {
  Histogram overhead;
  for (int i = 0; i < 1000000; ++i) {
    const std::uint64_t t0 = tsc_begin();
    const std::uint64_t t1 = tsc_end();
    overhead.record(static_cast<std::int64_t>(static_cast<double>(t1 - t0) / ticks_per_ns));
  }
  return static_cast<double>(overhead.percentile(50.0));
}

nlohmann::json prepare_thread(std::optional<int> core) {
  nlohmann::json state = {{"core", core ? nlohmann::json(*core) : nlohmann::json(nullptr)}};
  state["pinned"] = core ? pin_current_thread(*core) : false;
  // Locking future mappings under a finite RLIMIT_MEMLOCK makes every later large allocation fail,
  // so memory is locked only when the limit allows it all. Prefaulting already keeps page faults
  // out of the timed loop; locking only keeps the pages from being swapped out.
  rlimit limit{};
  const bool unlimited =
      ::getrlimit(RLIMIT_MEMLOCK, &limit) == 0 && limit.rlim_cur == RLIM_INFINITY;
  state["mlockall"] = unlimited ? (::mlockall(MCL_CURRENT | MCL_FUTURE) == 0 ? "locked" : "failed")
                                : "skipped: RLIMIT_MEMLOCK is finite";
  return state;
}

bool on_wsl() noexcept {
  std::ifstream version("/proc/version");
  std::string text;
  std::getline(version, text);
  return text.find("microsoft") != std::string::npos || text.find("Microsoft") != std::string::npos;
}

void refuse_wsl_publish(const std::filesystem::path& results_dir) {
  if (!on_wsl()) {
    return;
  }
  std::error_code ignored;
  const auto resolved = std::filesystem::weakly_canonical(results_dir, ignored);
  const auto repo_results = std::filesystem::weakly_canonical("results", ignored);
  if (resolved == repo_results) {
    throw std::runtime_error(
        "WSL2 runs are smoke checks and must not reach results/: set LLME_RESULTS_DIR to a "
        "directory outside the repository");
  }
}

std::string environment_label() { return on_wsl() ? "WSL2 smoke" : "native"; }

LoadedLog load_log(const std::filesystem::path& path) {
  auto reader = MlogReader::open(path);
  if (!reader) {
    throw std::runtime_error(std::string(to_string(reader.error())) + ": " + path.string());
  }
  EngineConfig config = tools::load_sidecar_config(path).value_or(EngineConfig{});
  return {std::move(*reader), config, path.stem().string()};
}

double median(std::vector<double> values) {
  if (values.empty()) {
    return 0.0;
  }
  std::sort(values.begin(), values.end());
  const std::size_t mid = values.size() / 2;
  return values.size() % 2 == 1 ? values[mid] : (values[mid - 1] + values[mid]) / 2.0;
}

void append_csv(const std::filesystem::path& path, const std::string& header,
                const std::string& row) {
  std::filesystem::create_directories(path.parent_path());
  const bool fresh = !std::filesystem::exists(path);
  std::ofstream csv(path, std::ios::app);
  if (fresh) {
    csv << header << '\n';
  }
  csv << row << '\n';
}

std::string make_run_id(const std::string& design, const std::string& workload) {
  const std::time_t now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
  std::tm tm{};
  gmtime_r(&now, &tm);
  std::array<char, 32> stamp{};
  const std::size_t n = std::strftime(stamp.data(), stamp.size(), "%Y%m%dT%H%M%SZ", &tm);
  return std::string(stamp.data(), n) + "_" + design + "_" + workload;
}

}  // namespace llme::bench
