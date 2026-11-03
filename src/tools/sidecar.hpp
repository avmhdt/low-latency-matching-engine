// <name>.meta.json, the sidecar every converter writes next to its .mlog (DQ-06). A replay takes
// its band and pool from it (DQ-07).
#pragma once

#include <filesystem>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>

#include "llme/types.hpp"
#include "replay.hpp"

namespace llme::tools {

// "x.mlog" to "x.meta.json".
[[nodiscard]] std::filesystem::path sidecar_path(const std::filesystem::path& log);

// The engine config in a log's sidecar, if it has one.
[[nodiscard]] std::optional<EngineConfig> load_sidecar_config(const std::filesystem::path& log);

[[nodiscard]] nlohmann::json outcome_json(const workload::ReplayOutcome& outcome);

// Everything DQ-06 lists, and the replay figures of FR-21 when an outcome is given.
[[nodiscard]] nlohmann::json sidecar_json(const std::string& source_file,
                                          const std::string& source_sha256,
                                          const std::string& symbol,
                                          const workload::ConversionStats& stats,
                                          const EngineConfig& config,
                                          const workload::ReplayOutcome& outcome);

void write_sidecar(const std::filesystem::path& log, const nlohmann::json& sidecar);

// SHA-256 of a file by sha256sum: converters are offline, not worth a crypto dependency.
[[nodiscard]] std::string sha256_of(const std::filesystem::path& file);

}  // namespace llme::tools
