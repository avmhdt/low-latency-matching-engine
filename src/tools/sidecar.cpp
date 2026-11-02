#include "sidecar.hpp"

#include <fstream>
#include <stdexcept>

#include "experiment_log.hpp"
#include "llme/output_hash.hpp"

namespace llme::tools {

using nlohmann::json;

std::filesystem::path sidecar_path(const std::filesystem::path& log) {
  auto path = log;
  path.replace_extension(".meta.json");
  return path;
}

std::optional<EngineConfig> load_sidecar_config(const std::filesystem::path& log) {
  std::ifstream file(sidecar_path(log));
  if (!file) {
    return std::nullopt;
  }
  const json sidecar = json::parse(file);
  EngineConfig config;
  config.price_min = sidecar.at("price_min").get<Price>();
  config.price_max = sidecar.at("price_max").get<Price>();
  config.max_orders = sidecar.at("max_orders").get<std::uint32_t>();
  return config;
}

json outcome_json(const workload::ReplayOutcome& outcome) {
  return {{"msgs", outcome.msgs},
          {"fills", outcome.fills},
          {"crossed_add", outcome.crossed_add},
          {"exec_total", outcome.exec_total},
          {"exec_contra_match", outcome.exec_contra_match},
          {"exec_contra_match_rate", outcome.exec_contra_match_rate()},
          {"exec_unfilled", outcome.exec_unfilled},
          {"rejects", outcome.rejects},
          {"pool_full", outcome.pool_full},
          {"price_out_of_band", outcome.price_out_of_band},
          {"cancel_rejects", outcome.cancel_rejects},
          {"modify_rejects", outcome.modify_rejects},
          {"output_hash", format_hash(outcome.output_hash)}};
}

json sidecar_json(const std::string& source_file, const std::string& source_sha256,
                  const std::string& symbol, const workload::ConversionStats& stats,
                  const EngineConfig& config, const workload::ReplayOutcome& outcome) {
  const json events = {{"events", stats.events},   {"adds", stats.adds},
                       {"execs", stats.execs},     {"partial_cancels", stats.partial_cancels},
                       {"deletes", stats.deletes}, {"replaces", stats.replaces}};
  const json mapped = {{"new", stats.out_new},
                       {"ioc", stats.out_ioc},
                       {"modify", stats.out_modify},
                       {"cancel", stats.out_cancel}};
  const json dropped = {{"price", stats.dropped_price},
                        {"qty", stats.dropped_qty},
                        {"unknown_id", stats.dropped_unknown_id}};
  const json skipped = {{"session", stats.skipped_session},
                        {"hidden", stats.skipped_hidden},
                        {"cross", stats.skipped_cross},
                        {"halt", stats.skipped_halt}};
  return {{"source_file", source_file},
          {"source_sha256", source_sha256},
          {"symbol", symbol},
          {"tick_size_e9", workload::kTickSizeE9},
          {"price_min", config.price_min},
          {"price_max", config.price_max},
          {"max_orders", config.max_orders},
          {"min_ticks_observed", stats.min_ticks.value_or(0)},
          {"max_ticks_observed", stats.max_ticks.value_or(0)},
          {"max_live_orders", stats.max_live_orders},
          {"events", events},
          {"mapped", mapped},
          {"dropped", dropped},
          {"skipped", skipped},
          {"exec_contra_encoding", "IOC order_id with the top bit cleared is the expected contra"},
          {"replay", outcome_json(outcome)}};
}

void write_sidecar(const std::filesystem::path& log, const json& sidecar) {
  const auto path = sidecar_path(log);
  std::ofstream file(path);
  file << sidecar.dump(2) << '\n';
  if (!file) {
    throw std::runtime_error("cannot write " + path.string());
  }
}

std::string sha256_of(const std::filesystem::path& file) {
  const std::string sum = capture_command("sha256sum -- '" + file.string() + "' 2>/dev/null");
  return sum.substr(0, sum.find(' '));
}

}  // namespace llme::tools
