// Shared pieces of the benchmark tools: TSC timing, HdrHistogram wrapper, design dispatch, and
// the guard that keeps WSL2 smoke runs out of the published results.
#pragma once

#include <hdr/hdr_histogram.h>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <nlohmann/json.hpp>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "llme/engine.hpp"
#include "llme/level_store_map.hpp"
#include "llme/level_store_sorted_vec.hpp"
#include "llme/mlog.hpp"

namespace llme::bench {

// Serialising timestamps (FR-23): lfence; rdtsc before, rdtscp; lfence after, so that the timed
// code can neither start before the first read nor finish after the second. Elsewhere,
// steady_clock nanoseconds stand in, as NFR-13 allows.
[[nodiscard]] std::uint64_t tsc_begin() noexcept;
[[nodiscard]] std::uint64_t tsc_end() noexcept;

// Ticks per nanosecond, measured against steady_clock over a short busy window.
[[nodiscard]] double calibrate_ticks_per_ns();

// Latency histogram in nanoseconds: 1 ns to 10 s at 3 significant digits (FR-23).
class Histogram {
 public:
  Histogram();
  Histogram(const Histogram&) = delete;
  Histogram& operator=(const Histogram&) = delete;
  Histogram(Histogram&& other) noexcept : h_(std::exchange(other.h_, nullptr)) {}
  Histogram& operator=(Histogram&& other) noexcept {
    std::swap(h_, other.h_);
    return *this;
  }
  ~Histogram();

  void record(std::int64_t ns) noexcept;
  void add(const Histogram& other) noexcept;
  [[nodiscard]] std::int64_t count() const noexcept;
  [[nodiscard]] std::int64_t percentile(double p) const noexcept;
  [[nodiscard]] std::int64_t max() const noexcept;
  [[nodiscard]] double mean() const noexcept;
  void write_hgrm(const std::filesystem::path& path) const;
  [[nodiscard]] nlohmann::json summary() const;

 private:
  hdr_histogram* h_ = nullptr;
};

// p50 of back-to-back timestamp pairs with nothing between them, in nanoseconds.
[[nodiscard]] double timer_overhead_p50_ns(double ticks_per_ns);

// Pins the calling thread and locks memory, best effort; says what worked.
[[nodiscard]] nlohmann::json prepare_thread(std::optional<int> core);

// True on WSL2, where results are smoke checks only.
[[nodiscard]] bool on_wsl() noexcept;

// Throws unless a WSL2 run writes outside the repository's results/: no number from WSL2 may
// reach a published table.
void refuse_wsl_publish(const std::filesystem::path& results_dir);

// "WSL2 smoke" or "native".
[[nodiscard]] std::string environment_label();

struct LoadedLog {
  MlogReader reader;
  EngineConfig config;
  std::string name;
};

// Opens a log and takes its band and pool from the sidecar when there is one (DQ-07).
[[nodiscard]] LoadedLog load_log(const std::filesystem::path& path);

// Calls f.template operator()<Store>() for the named design.
template <class F>
decltype(auto) with_design(std::string_view design, F&& f) {
  if (design == "d2") {
    return std::forward<F>(f).template operator()<LadderLevelStore>();
  }
  if (design == "d1") {
    return std::forward<F>(f).template operator()<MapLevelStore>();
  }
  if (design == "d3") {
    return std::forward<F>(f).template operator()<SortedVecLevelStore>();
  }
  throw std::invalid_argument("unknown design: " + std::string(design) + " (d1, d2 or d3)");
}

// Median of values; the input is copied and sorted.
[[nodiscard]] double median(std::vector<double> values);

// Appends one row, writing the header first when the file is new.
void append_csv(const std::filesystem::path& path, const std::string& header,
                const std::string& row);

// <utc time>_<design>_<workload>, unique enough for one host.
[[nodiscard]] std::string make_run_id(const std::string& design, const std::string& workload);

}  // namespace llme::bench
