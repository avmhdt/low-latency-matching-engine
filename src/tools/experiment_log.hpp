// Appends one row per run to results/experiment_log.csv, the honest trial count of FR-30.
#pragma once

#include <cstdint>
#include <filesystem>
#include <string>

namespace llme::tools {

struct ExperimentRow {
  std::string tool;
  std::string args;
  std::string design;
  std::string workload;
  std::string input_hash;
  std::string output_hash;
  std::uint64_t n_msgs = 0;
  std::string headline_metric;
  std::string headline_value;
  std::string result_path;
};

void append_experiment_row(const ExperimentRow& row);

// Directory for results: $LLME_RESULTS_DIR, or results/ under the working directory.
[[nodiscard]] std::filesystem::path results_dir();

// "<sha>" of HEAD in the working directory, or "unknown" outside a repository.
[[nodiscard]] std::string git_sha();

// Standard output of a shell command with trailing whitespace removed, or empty on failure.
[[nodiscard]] std::string capture_command(const std::string& command);

// This machine's name; results/manifests/<host>.json is its manifest.
[[nodiscard]] std::string hostname();

}  // namespace llme::tools
