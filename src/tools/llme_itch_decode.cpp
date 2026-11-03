// llme-itch-decode: fallback ITCH 5.0 decoder to one .mlog per symbol, plus sidecars (FR-21).
//
//   llme-itch-decode --in "$LLME_ITCH_FILE" --symbols AAPL,MSFT --top 50 --out-dir
//   data/processed/mlog/
//
// The first pass ranks symbols by order messages in the session (D-05) and the second converts.
#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "cli_common.hpp"
#include "experiment_log.hpp"
#include "itch_decoder.hpp"
#include "llme/mlog.hpp"
#include "replay.hpp"
#include "sidecar.hpp"

namespace {

using llme::workload::ItchEvent;
using nlohmann::json;

struct Options {
  std::string in;
  std::string symbols;
  std::string out_dir;
  std::string date;
  std::uint32_t top = 0;
  bool json_output = false;
};

struct Decoded {
  std::uint64_t frames = 0;
  std::uint64_t malformed = 0;
  bool truncated = false;
};

template <class Visitor>
Decoded for_each_event(const std::string& path, const Visitor& visit) {
  llme::workload::FileSource source(path);
  llme::workload::ItchFrameReader reader(source);
  Decoded decoded;
  std::span<const std::uint8_t> message;
  ItchEvent event;
  while (reader.next(message)) {
    ++decoded.frames;
    bool malformed = false;
    if (llme::workload::parse_itch(message, event, malformed)) {
      visit(event);
    } else if (malformed) {
      ++decoded.malformed;
    }
  }
  decoded.truncated = reader.truncated();
  return decoded;
}

struct Ranked {
  std::string symbol;
  std::uint16_t locate = 0;
  std::uint64_t order_messages = 0;
};

// Order messages per symbol inside the session, most active first; ties go by name.
std::vector<Ranked> rank_symbols(const std::string& path) {
  std::vector<std::uint64_t> counts(std::size_t{1} << 16U, 0);
  std::map<std::uint16_t, std::string> names;
  (void)for_each_event(path, [&](const ItchEvent& event) {
    if (event.type == 'R') {
      names[event.locate] = llme::workload::stock_name(event.stock);
    } else if (llme::workload::is_order_message(event.type) &&
               event.timestamp_ns >= llme::workload::kSessionStartNs &&
               event.timestamp_ns < llme::workload::kSessionEndNs) {
      ++counts[event.locate];
    }
  });
  std::vector<Ranked> ranked;
  ranked.reserve(names.size());
  for (const auto& [locate, name] : names) {
    ranked.push_back({name, locate, counts[locate]});
  }
  std::sort(ranked.begin(), ranked.end(), [](const Ranked& a, const Ranked& b) {
    return a.order_messages != b.order_messages ? a.order_messages > b.order_messages
                                                : a.symbol < b.symbol;
  });
  return ranked;
}

std::vector<Ranked> choose(const std::vector<Ranked>& ranked, const Options& options) {
  std::vector<std::string> wanted;
  std::stringstream names(options.symbols);
  for (std::string name; std::getline(names, name, ',');) {
    wanted.push_back(name);
  }
  std::vector<Ranked> chosen;
  for (std::size_t i = 0; i < ranked.size(); ++i) {
    const bool named = std::find(wanted.begin(), wanted.end(), ranked[i].symbol) != wanted.end();
    if (i < options.top || named) {
      chosen.push_back(ranked[i]);
    }
  }
  for (const auto& name : wanted) {
    if (std::none_of(chosen.begin(), chosen.end(),
                     [&](const Ranked& r) { return r.symbol == name; })) {
      throw std::runtime_error("symbol not in the stock directory: " + name);
    }
  }
  return chosen;
}

// 01302019.NASDAQ_ITCH50 to 2019-01-30.
std::string date_of(const std::string& path) {
  const std::string name = std::filesystem::path(path).filename().string();
  if (name.size() < 8) {
    throw std::runtime_error("cannot read a date from the file name; pass --date");
  }
  return name.substr(4, 4) + "-" + name.substr(0, 2) + "-" + name.substr(2, 2);
}

struct SymbolLog {
  std::string symbol;
  std::filesystem::path path;
  std::uint64_t order_messages = 0;
  llme::MlogWriter writer;
  llme::workload::ReplayBuilder builder;

  // order collects the symbol index of every message across all logs, in emission order.
  SymbolLog(Ranked ranked, std::filesystem::path out, llme::MlogWriter log, std::uint16_t index,
            std::vector<std::uint16_t>* order)
      : symbol(std::move(ranked.symbol)),
        path(std::move(out)),
        order_messages(ranked.order_messages),
        writer(std::move(log)),
        builder([this, index, order](const llme::InMsg& m) {
          if (!writer.write(m)) {
            throw std::runtime_error("write failed: " + path.string());
          }
          order->push_back(index);
        }) {}
};

// <dir>/itch_top_<date>.interleave: one little-endian u16 per message, the index of its symbol's
// log, in the day's order. llme-bench-throughput replays the top 50 in that order (FR-25).
void write_interleave(const std::filesystem::path& dir, const std::string& date,
                      const std::vector<std::unique_ptr<SymbolLog>>& logs,
                      const std::vector<std::uint16_t>& order) {
  const auto path = dir / ("itch_top_" + date + ".interleave");
  std::ofstream bytes(path, std::ios::binary);
  bytes.write(reinterpret_cast<const char*>(order.data()),  // NOLINT: raw u16 file
              static_cast<std::streamsize>(order.size() * sizeof(std::uint16_t)));
  json index = {{"count", order.size()}, {"logs", json::array()}, {"symbols", json::array()}};
  for (const auto& log : logs) {
    index["logs"].push_back(log->path.filename().string());
    index["symbols"].push_back(log->symbol);
  }
  std::ofstream(path.string() + ".json") << index.dump(2) << '\n';
  if (!bytes) {
    throw std::runtime_error("cannot write " + path.string());
  }
}

int execute(const Options& options) {
  const std::string date = options.date.empty() ? date_of(options.in) : options.date;
  const auto ranked = rank_symbols(options.in);
  const auto chosen = choose(ranked, options);
  std::filesystem::create_directories(options.out_dir);

  if (chosen.size() > std::size_t{1} << 16U) {
    throw std::runtime_error("at most 65536 symbols per decode");
  }
  std::vector<std::unique_ptr<SymbolLog>> logs;
  std::vector<std::uint16_t> order;
  llme::workload::ItchConverter converter;
  for (std::uint32_t i = 0; i < chosen.size(); ++i) {
    const auto path = std::filesystem::path(options.out_dir) /
                      ("itch_" + chosen[i].symbol + "_" + date + ".mlog");
    auto writer =
        llme::MlogWriter::create(path, {.symbol_id = i, .source = llme::LogSource::ItchFallback});
    if (!writer) {
      throw std::runtime_error(std::string(llme::to_string(writer.error())) + ": " + path.string());
    }
    logs.push_back(std::make_unique<SymbolLog>(chosen[i], path, std::move(*writer),
                                               static_cast<std::uint16_t>(i), &order));
    converter.select(chosen[i].locate, &logs.back()->builder);
  }

  const Decoded decoded =
      for_each_event(options.in, [&](const ItchEvent& event) { converter.on_event(event); });
  if (decoded.truncated) {
    throw std::runtime_error("input ends inside a message: " + options.in);
  }

  const std::string sha = llme::tools::sha256_of(options.in);
  const std::string source = std::filesystem::path(options.in).filename().string();
  json summary = {{"frames", decoded.frames}, {"malformed", decoded.malformed}};
  json symbols = json::array();
  for (const auto& log : logs) {
    if (auto closed = log->writer.close(); !closed) {
      throw std::runtime_error(std::string(llme::to_string(closed.error())) + ": " +
                               log->path.string());
    }
    const auto reader = llme::MlogReader::open(log->path);
    if (!reader) {
      throw std::runtime_error(std::string(llme::to_string(reader.error())) + ": " +
                               log->path.string());
    }
    const auto& stats = log->builder.stats();
    const auto config = llme::workload::replay_config(stats);
    const auto outcome = llme::workload::replay_log(reader->records(), config);
    json sidecar = llme::tools::sidecar_json(source, sha, log->symbol, stats, config, outcome);
    sidecar["order_messages"] = log->order_messages;
    llme::tools::write_sidecar(log->path, sidecar);
    symbols.push_back({{"symbol", log->symbol},
                       {"order_messages", log->order_messages},
                       {"msgs", outcome.msgs},
                       {"exec_contra_match_rate", outcome.exec_contra_match_rate()},
                       {"crossed_add", outcome.crossed_add}});
  }
  summary["symbols"] = symbols;
  if (logs.size() > 1) {
    write_interleave(options.out_dir, date, logs, order);
    summary["interleave"] = order.size();
  }
  if (options.json_output) {
    std::cout << summary.dump() << '\n';
  } else {
    std::cout << logs.size() << " logs written to " << options.out_dir << '\n';
  }
  return 0;
}

int run(int argc, char** argv) {
  CLI::App app{"Decode ITCH 5.0 order messages to one .mlog per symbol", "llme-itch-decode"};
  Options options;
  app.add_option("--in", options.in, "ITCH 5.0 file, gzipped or plain")->required();
  app.add_option("--symbols", options.symbols, "Comma-separated symbols to include");
  app.add_option("--top", options.top, "Also include the N most active symbols");
  app.add_option("--out-dir", options.out_dir, "Output directory")->required();
  app.add_option("--date", options.date, "Date for file names (default: from the input name)");
  app.add_flag("--json", options.json_output, "Print a JSON summary");
  return llme::tools::parse_and_run(app, argc, argv, [&] {
    if (options.symbols.empty() && options.top == 0) {
      throw std::runtime_error("nothing selected: pass --symbols or --top");
    }
    return execute(options);
  });
}

}  // namespace

int main(int argc, char** argv) {
  return llme::tools::guarded_main([&] { return run(argc, argv); });
}
