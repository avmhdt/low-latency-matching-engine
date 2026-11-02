// llme-convert-lobster: LOBSTER message CSV to .mlog plus sidecar (FR-22). With an orderbook file,
// it also compares the engine's top of book with LOBSTER's after every message.
#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include "cli_common.hpp"
#include "experiment_log.hpp"
#include "llme/engine.hpp"
#include "llme/mlog.hpp"
#include "lobster_adapter.hpp"
#include "replay.hpp"
#include "sidecar.hpp"

namespace {

using nlohmann::json;

struct Options {
  std::string messages;
  std::string orderbook;
  std::string symbol;
  std::string out;
  bool json_output = false;
};

// One input line: how many messages the log held after it, and whether it was dropped.
struct LineMark {
  std::uint64_t messages_after = 0;
  bool dropped = false;
};

struct Comparison {
  std::uint64_t compared = 0;
  std::uint64_t price_agree = 0;
  std::uint64_t full_agree = 0;
  std::uint64_t skipped_dropped = 0;
};

// Replays the whole log and compares the first level with the orderbook file, line by line.
// A sample reports only events inside its visible levels, so this drifts for reasons that have
// nothing to do with matching; docs/decisions.md has the numbers.
Comparison compare_top_of_book(const std::vector<llme::InMsg>& msgs,
                               const std::vector<LineMark>& marks, const llme::EngineConfig& config,
                               const std::string& orderbook) {
  std::ifstream book(orderbook);
  if (!book) {
    throw std::runtime_error("cannot open " + orderbook);
  }
  llme::MatchingEngine<llme::LadderLevelStore, llme::NullSink> engine(config, llme::NullSink{});
  Comparison result;
  std::size_t next = 0;
  std::string line;
  for (const LineMark& mark : marks) {
    if (!std::getline(book, line)) {
      throw std::runtime_error("orderbook file is shorter than the message file");
    }
    for (; next < mark.messages_after; ++next) {
      engine.on_message(msgs[next]);
    }
    if (mark.dropped) {
      ++result.skipped_dropped;
      continue;
    }
    llme::workload::TopOfBook row;
    if (!llme::workload::parse_lobster_top(line, row)) {
      throw std::runtime_error("bad orderbook line: " + line);
    }
    const auto side_agrees = [&](std::optional<llme::Price> best, llme::Side side,
                                 std::int64_t price, std::int64_t size, bool& price_ok) {
      if (size == 0) {
        price_ok = !best.has_value();
        return price_ok;
      }
      price_ok = best && *best * llme::workload::kRawPerTick == price;
      return price_ok && static_cast<std::int64_t>(engine.depth_at(side, *best)) == size;
    };
    bool ask_price = false;
    bool bid_price = false;
    const bool ask =
        side_agrees(engine.best_ask(), llme::Side::Sell, row.ask_price, row.ask_size, ask_price);
    const bool bid =
        side_agrees(engine.best_bid(), llme::Side::Buy, row.bid_price, row.bid_size, bid_price);
    ++result.compared;
    result.price_agree += ask_price && bid_price ? 1U : 0U;
    result.full_agree += ask && bid ? 1U : 0U;
  }
  return result;
}

// One event at a time from the book the file itself shows: see OneStepChecker.
json one_step_check(const std::string& messages, const std::string& orderbook) {
  std::vector<std::vector<std::int64_t>> rows;
  {
    std::ifstream book(orderbook);
    std::string line;
    std::vector<std::int64_t> row;
    while (std::getline(book, line)) {
      if (!llme::workload::parse_lobster_row(line, row)) {
        throw std::runtime_error("bad orderbook line: " + line);
      }
      rows.push_back(row);
    }
  }
  std::int64_t max_raw = 0;
  for (const auto& row : rows) {
    for (std::size_t i = 0; i + 1 < row.size(); i += 2) {
      if (row[i + 1] > 0) {
        max_raw = std::max(max_raw, row[i]);
      }
    }
  }

  llme::workload::OneStepChecker checker(max_raw / llme::workload::kRawPerTick * 2 + 1);
  std::ifstream input(messages);
  std::string line;
  std::size_t index = 0;
  while (std::getline(input, line) && index < rows.size()) {
    llme::workload::LobsterEvent event;
    const bool parsed = llme::workload::parse_lobster_message(line, event);
    if (index > 0) {
      checker.check(rows[index - 1], parsed ? &event : nullptr, rows[index]);
    }
    ++index;
  }
  const auto& result = checker.result();
  return {{"compared", result.compared},
          {"agree", result.agree},
          {"events_outside_window", result.events_outside_window},
          {"agreement_rate", result.compared == 0 ? 0.0
                                                  : static_cast<double>(result.agree) /
                                                        static_cast<double>(result.compared)}};
}

int execute(const Options& options) {
  std::ifstream input(options.messages);
  if (!input) {
    throw std::runtime_error("cannot open " + options.messages);
  }
  auto writer = llme::MlogWriter::create(options.out, {.source = llme::LogSource::Lobster});
  if (!writer) {
    throw std::runtime_error(std::string(llme::to_string(writer.error())) + ": " + options.out);
  }

  std::vector<llme::InMsg> msgs;
  std::vector<LineMark> marks;
  llme::workload::ReplayBuilder builder([&](const llme::InMsg& m) {
    msgs.push_back(m);
    if (!writer->write(m)) {
      throw std::runtime_error("write failed: " + options.out);
    }
  });

  std::uint64_t malformed = 0;
  std::string line;
  while (std::getline(input, line)) {
    const std::uint64_t dropped_before = builder.stats().dropped_unknown_id +
                                         builder.stats().dropped_price +
                                         builder.stats().dropped_qty;
    llme::workload::LobsterEvent event;
    const bool applied = llme::workload::parse_lobster_message(line, event) &&
                         llme::workload::apply_lobster_event(event, builder);
    malformed += applied ? 0U : 1U;
    const std::uint64_t dropped_after = builder.stats().dropped_unknown_id +
                                        builder.stats().dropped_price + builder.stats().dropped_qty;
    marks.push_back({msgs.size(), !applied || dropped_after != dropped_before});
  }
  if (auto closed = writer->close(); !closed) {
    throw std::runtime_error(std::string(llme::to_string(closed.error())) + ": " + options.out);
  }

  const llme::EngineConfig config = llme::workload::replay_config(builder.stats());
  const auto outcome = llme::workload::replay_log(msgs, config);
  json sidecar = llme::tools::sidecar_json(
      std::filesystem::path(options.messages).filename().string(),
      llme::tools::sha256_of(options.messages), options.symbol, builder.stats(), config, outcome);
  sidecar["dropped"]["malformed"] = malformed;

  if (!options.orderbook.empty()) {
    const Comparison top = compare_top_of_book(msgs, marks, config, options.orderbook);
    const auto rate = [&](std::uint64_t n) {
      return top.compared == 0 ? 0.0 : static_cast<double>(n) / static_cast<double>(top.compared);
    };
    sidecar["top_of_book"] = {{"compared", top.compared},
                              {"skipped_dropped", top.skipped_dropped},
                              {"price_agree", top.price_agree},
                              {"full_agree", top.full_agree},
                              {"price_agreement_rate", rate(top.price_agree)},
                              {"full_agreement_rate", rate(top.full_agree)}};
    sidecar["one_step_top_of_book"] = one_step_check(options.messages, options.orderbook);
  }
  llme::tools::write_sidecar(options.out, sidecar);

  if (options.json_output) {
    std::cout << sidecar.dump() << '\n';
  } else {
    std::cout << msgs.size() << " messages from " << marks.size() << " lines\n";
  }
  return 0;
}

int run(int argc, char** argv) {
  CLI::App app{"Convert a LOBSTER message file to .mlog", "llme-convert-lobster"};
  Options options;
  app.add_option("--messages", options.messages, "LOBSTER message CSV")->required();
  app.add_option("--orderbook", options.orderbook, "LOBSTER orderbook CSV to validate against");
  app.add_option("--symbol", options.symbol, "Symbol name")->required();
  app.add_option("--out", options.out, "Output .mlog path")->required();
  app.add_flag("--json", options.json_output, "Print the sidecar on stdout");
  return llme::tools::parse_and_run(app, argc, argv, [&] { return execute(options); });
}

}  // namespace

int main(int argc, char** argv) {
  return llme::tools::guarded_main([&] { return run(argc, argv); });
}
