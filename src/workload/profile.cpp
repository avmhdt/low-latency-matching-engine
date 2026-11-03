// TOML loading for workload profiles and the engine config. Tools only; never on the hot path.
#include <array>
#include <cmath>
#include <stdexcept>
#include <string>
#include <toml++/toml.hpp>

#include "generator.hpp"

namespace llme::workload {

namespace {

[[noreturn]] void fail(const std::filesystem::path& file, const std::string& why) {
  throw std::runtime_error(file.string() + ": " + why);
}

template <class T>
void read(const toml::node_view<const toml::node>& node, T& value) {
  if constexpr (std::is_same_v<T, double>) {
    if (const auto v = node.value<double>()) {
      value = *v;
    }
  } else if constexpr (std::is_same_v<T, bool>) {
    if (const auto v = node.value<bool>()) {
      value = *v;
    }
  } else {
    if (const auto v = node.value<std::int64_t>()) {
      value = static_cast<T>(*v);
    }
  }
}

void read_engine(const toml::table& root, EngineConfig& cfg, const std::filesystem::path& file) {
  const auto engine = root["engine"];
  read(engine["price_min"], cfg.price_min);
  read(engine["price_max"], cfg.price_max);
  read(engine["max_orders"], cfg.max_orders);
  read(engine["id_map_load"], cfg.id_map_load);
  const std::string stp = engine["stp"].value_or(std::string("off"));
  if (stp != "off") {
    fail(file, "only stp = \"off\" is implemented");
  }
  if (cfg.price_min > cfg.price_max || cfg.max_orders == 0) {
    fail(file, "invalid [engine] band or capacity");
  }
}

Mix read_mix(const toml::table& root, const std::filesystem::path& file) {
  const bool rates = root["rates"].is_table();
  const auto table = rates ? root["rates"] : root["mix"];
  Mix mix;
  if (!table.is_table()) {
    return mix;
  }
  mix.invalid = 0.0;
  std::array<double*, 7> fields = {&mix.new_limit, &mix.cancel, &mix.modify, &mix.ioc,
                                   &mix.fok,       &mix.market, &mix.invalid};
  constexpr std::array<const char*, 7> kNames = {"new_limit", "cancel", "modify", "ioc",
                                                 "fok",       "market", "invalid"};
  double total = 0.0;
  for (std::size_t i = 0; i < fields.size(); ++i) {
    *fields[i] = table[kNames[i]].value_or(0.0);
    if (*fields[i] < 0.0) {
      fail(file, std::string("negative share: ") + kNames[i]);
    }
    total += *fields[i];
  }
  if (rates) {
    // Independent Poisson streams merge into one whose next event has type i with probability
    // rate_i / total, and an .mlog stores order but no times, so rates reduce to shares.
    for (double* field : fields) {
      *field /= total;
    }
  } else if (std::abs(total - 1.0) > 1e-9) {
    fail(file, "[mix] shares must sum to 1.0");
  }
  return mix;
}

}  // namespace

EngineConfig load_engine_config(const std::filesystem::path& file) {
  try {
    const toml::table root = toml::parse_file(file.string());
    EngineConfig cfg;
    read_engine(root, cfg, file);
    return cfg;
  } catch (const toml::parse_error& e) {
    fail(file, e.what());
  }
}

Profile load_profile(const std::filesystem::path& file) {
  toml::table root;
  try {
    root = toml::parse_file(file.string());
  } catch (const toml::parse_error& e) {
    fail(file, e.what());
  }

  Profile p;
  p.name = file.stem().string();
  read_engine(root, p.engine, file);
  p.mix = read_mix(root, file);

  const toml::table& view = root;
  const auto generator = view["generator"];
  read(generator["seed"], p.seed);
  read(generator["mid_start"], p.mid_start);
  read(generator["mid_walk_sigma"], p.mid_walk_sigma);
  read(generator["half_spread"], p.half_spread);
  read(generator["target_live"], p.target_live);
  read(generator["traders"], p.traders);

  const auto price = view["price"];
  const std::string price_dist = price["dist"].value_or(std::string("geometric"));
  if (price_dist == "geometric") {
    p.price_dist = PriceDist::Geometric;
  } else if (price_dist == "power_law") {
    p.price_dist = PriceDist::PowerLaw;
  } else if (price_dist == "uniform") {
    p.price_dist = PriceDist::Uniform;
  } else {
    fail(file, "unknown price dist: " + price_dist);
  }
  read(price["p"], p.price_p);
  read(price["alpha"], p.price_alpha);
  read(price["max_offset"], p.price_max_offset);
  read(price["cross_prob"], p.cross_prob);
  p.price_floor = price["floor"].value<std::int64_t>();
  p.price_ceil = price["ceil"].value<std::int64_t>();

  const auto size = view["size"];
  const std::string size_dist = size["dist"].value_or(std::string("geometric"));
  if (size_dist == "geometric") {
    p.size_dist = SizeDist::Geometric;
  } else if (size_dist == "uniform") {
    p.size_dist = SizeDist::Uniform;
  } else {
    fail(file, "unknown size dist: " + size_dist);
  }
  read(size["mean_lots"], p.size_mean);
  read(size["min"], p.size_min);
  read(size["max"], p.size_max);

  const std::string target = view["cancel"]["target"].value_or(std::string("uniform"));
  if (target == "uniform") {
    p.cancel_target = CancelTarget::Uniform;
  } else if (target == "oldest") {
    p.cancel_target = CancelTarget::Oldest;
  } else if (target == "newest") {
    p.cancel_target = CancelTarget::Newest;
  } else if (target == "near_touch") {
    p.cancel_target = CancelTarget::NearTouch;
  } else {
    fail(file, "unknown cancel target: " + target);
  }

  const auto modify = view["modify"];
  read(modify["reduce"], p.modify_reduce);
  read(modify["increase"], p.modify_increase);
  read(modify["reprice"], p.modify_reprice);

  const auto burst = view["burst"];
  read(burst["enabled"], p.burst_enabled);
  read(burst["length"], p.burst_length);
  read(burst["every"], p.burst_every);
  if (const auto burst_target = burst["target"].value<std::string>()) {
    if (*burst_target == "uniform") {
      p.burst_target = CancelTarget::Uniform;
    } else if (*burst_target == "near_touch") {
      p.burst_target = CancelTarget::NearTouch;
    } else if (*burst_target == "oldest") {
      p.burst_target = CancelTarget::Oldest;
    } else if (*burst_target == "newest") {
      p.burst_target = CancelTarget::Newest;
    } else {
      fail(file, "unknown burst target: " + *burst_target);
    }
  }

  if (p.size_mean < 1.0 || p.size_min == 0 || p.size_min > p.size_max) {
    fail(file, "invalid [size] section");
  }
  if (p.price_p <= 0.0 || p.price_p > 1.0 || p.traders == 0 || p.burst_every == 0) {
    fail(file, "invalid profile parameter");
  }
  if (p.modify_reduce + p.modify_increase + p.modify_reprice <= 0.0) {
    fail(file, "[modify] shares must not all be zero");
  }
  if (p.mid_start < p.engine.price_min || p.mid_start > p.engine.price_max) {
    fail(file, "mid_start outside the engine band");
  }
  return p;
}

}  // namespace llme::workload
