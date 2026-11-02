#include "experiment_log.hpp"

#include <unistd.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>

namespace llme::tools {

namespace {

std::string utc_now(const char* format) {
  const std::time_t now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
  std::tm tm{};
  gmtime_r(&now, &tm);
  std::array<char, 32> text{};
  const std::size_t n = std::strftime(text.data(), text.size(), format, &tm);
  return {text.data(), n};
}

// Commas in free-text fields would shift the columns.
std::string field(std::string text) {
  std::replace(text.begin(), text.end(), ',', ' ');
  return text;
}

std::string cpu_model() {
  std::ifstream info("/proc/cpuinfo");
  std::string line;
  while (std::getline(info, line)) {
    if (const auto colon = line.find(':'); line.starts_with("model name") && colon != line.npos) {
      return line.substr(colon + 2);
    }
  }
  return "unknown";
}

bool is_wsl() {
  std::ifstream version("/proc/version");
  std::string text;
  std::getline(version, text);
  return text.find("microsoft") != text.npos || text.find("Microsoft") != text.npos;
}

}  // namespace

std::string capture_command(const std::string& command) {
  struct Closer {
    void operator()(std::FILE* f) const noexcept { pclose(f); }
  };
  const std::unique_ptr<std::FILE, Closer> pipe(popen(command.c_str(), "r"));
  if (!pipe) {
    return {};
  }
  std::string out;
  std::array<char, 256> buffer{};
  while (std::fgets(buffer.data(), static_cast<int>(buffer.size()), pipe.get()) != nullptr) {
    out += buffer.data();
  }
  while (!out.empty() && (out.back() == '\n' || out.back() == ' ')) {
    out.pop_back();
  }
  return out;
}

std::string hostname() {
  std::array<char, 256> name{};
  return gethostname(name.data(), name.size()) == 0 ? std::string(name.data()) : "unknown";
}

std::filesystem::path results_dir() {
  // NOLINTNEXTLINE(concurrency-mt-unsafe): single-threaded tool start-up
  const char* configured = std::getenv("LLME_RESULTS_DIR");
  return configured != nullptr && *configured != '\0' ? configured : "results";
}

std::string git_sha() {
  std::string sha = capture_command("git rev-parse HEAD 2>/dev/null");
  return sha.empty() ? "unknown" : sha;
}

void append_experiment_row(const ExperimentRow& row) {
  const auto path = results_dir() / "experiment_log.csv";
  std::ofstream csv(path, std::ios::app);
  if (!csv) {
    return;
  }
  // results/ is excluded: a run's own output must not mark the run as dirty.
  const bool dirty =
      !capture_command("git status --porcelain -- . ':!results' 2>/dev/null | head -1").empty();
  csv << row.tool << '_' << utc_now("%Y%m%dT%H%M%SZ") << '_' << getpid() << ','
      << utc_now("%Y-%m-%dT%H:%M:%SZ") << ',' << git_sha() << ',' << (dirty ? "true" : "false")
      << ',' << row.tool << ',' << field(row.args) << ',' << row.design << ',' << row.workload
      << ',' << row.input_hash << ',' << row.output_hash << ',' << row.n_msgs << ','
#if defined(__clang__)
      << "clang-" << __clang_major__
#else
      << "gcc-" << __GNUC__
#endif
      << ",," << hostname() << ',' << field(cpu_model()) << ",,,,," << (is_wsl() ? "true" : "false")
      << ',' << row.headline_metric << ',' << row.headline_value << ',' << row.result_path << '\n';
}

}  // namespace llme::tools
