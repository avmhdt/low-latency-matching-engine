#include "scenario.hpp"

#include <charconv>
#include <fstream>
#include <map>
#include <sstream>
#include <stdexcept>

namespace llme::golden {
namespace {

[[noreturn]] void fail(const std::string& file, const std::string& line, const std::string& why) {
  throw std::runtime_error(file + ": " + why + ": " + line);
}

// Splits "key=value" words into a map; bare words map to an empty value.
std::map<std::string, std::string> keyed(std::istringstream& in) {
  std::map<std::string, std::string> out;
  std::string word;
  while (in >> word) {
    const auto eq = word.find('=');
    if (eq == std::string::npos) {
      out[word] = "";
    } else {
      out[word.substr(0, eq)] = word.substr(eq + 1);
    }
  }
  return out;
}

Side parse_side(const std::string& word) {
  if (word == "buy") {
    return Side::Buy;
  }
  if (word == "sell") {
    return Side::Sell;
  }
  throw std::runtime_error("bad side: " + word);
}

RejectReason parse_reject(const std::string& name) {
  static const std::map<std::string, RejectReason> kNames = {
      {"INVALID_QTY", RejectReason::InvalidQty},
      {"PRICE_OUT_OF_BAND", RejectReason::PriceOutOfBand},
      {"DUPLICATE_ID", RejectReason::DuplicateId},
      {"POOL_FULL", RejectReason::PoolFull},
      {"UNKNOWN_ID", RejectReason::UnknownId},
      {"INVALID_TYPE", RejectReason::InvalidType},
      {"INVALID_SIDE", RejectReason::InvalidSide}};
  return kNames.at(name);
}

CancelReason parse_cancel(const std::string& name) {
  static const std::map<std::string, CancelReason> kNames = {
      {"USER", CancelReason::User},
      {"IOC_REMAINDER", CancelReason::IocRemainder},
      {"FOK_UNFILLABLE", CancelReason::FokUnfillable},
      {"NO_LIQUIDITY", CancelReason::NoLiquidity}};
  return kNames.at(name);
}

InMsg parse_input(const std::string& text) {
  std::istringstream in(text);
  std::string verb;
  in >> verb;
  InMsg m;
  if (verb == "new") {
    std::string side;
    std::string tif;
    in >> m.order_id >> side >> tif;
    m.type = to_wire(MsgType::New);
    m.side = to_wire(parse_side(side));
    in >> m.qty;
    if (tif == "market") {
      m.ord_type = to_wire(OrdType::Market);
      m.tif = to_wire(Tif::Ioc);
    } else {
      char at = 0;
      in >> at >> m.price;
      m.ord_type = to_wire(OrdType::Limit);
      m.tif = to_wire(tif == "gtc" ? Tif::Gtc : (tif == "ioc" ? Tif::Ioc : Tif::Fok));
    }
  } else if (verb == "cancel") {
    m.type = to_wire(MsgType::Cancel);
    in >> m.order_id;
  } else if (verb == "modify") {
    m.type = to_wire(MsgType::Modify);
    in >> m.order_id;
    auto kv = keyed(in);
    m.qty = static_cast<Qty>(std::stoull(kv.at("qty")));
    m.price = std::stoll(kv.at("px"));
  } else if (verb == "raw") {
    auto kv = keyed(in);
    m.type = static_cast<std::uint8_t>(std::stoul(kv.at("type")));
    m.side = static_cast<std::uint8_t>(std::stoul(kv.at("side")));
    m.ord_type = static_cast<std::uint8_t>(std::stoul(kv.at("ord")));
    m.tif = static_cast<std::uint8_t>(std::stoul(kv.at("tif")));
    m.qty = static_cast<Qty>(std::stoull(kv.at("qty")));
    m.order_id = std::stoull(kv.at("id"));
    m.price = std::stoll(kv.at("px"));
  } else {
    throw std::runtime_error("bad input verb: " + verb);
  }
  return m;
}

OutMsg parse_output(const std::string& text, std::uint64_t seq) {
  std::istringstream in(text);
  std::string verb;
  in >> verb;
  OutMsg r;
  r.seq = seq;
  const auto u32 = [](const std::string& s) { return static_cast<Qty>(std::stoull(s)); };

  if (verb == "level") {
    std::string side;
    in >> side;
    auto kv = keyed(in);
    r.type = to_wire(OutType::LevelUpdate);
    r.side = to_wire(parse_side(side));
    r.price = std::stoll(kv.at("px"));
    r.qty = u32(kv.at("qty"));
    r.flags = kv.contains("removed") ? kFlagLevelRemoved : std::uint8_t{0};
    return r;
  }

  in >> r.order_id;
  if (verb == "cancel_reject" || verb == "modify_reject") {
    auto kv = keyed(in);
    r.type = to_wire(verb == "cancel_reject" ? OutType::CancelReject : OutType::ModifyReject);
    r.reason = to_wire(parse_reject(kv.at("reason")));
    return r;
  }
  if (verb == "reject") {
    auto kv = keyed(in);
    r.type = to_wire(OutType::Reject);
    r.side = static_cast<std::uint8_t>(std::stoul(kv.at("side")));
    r.reason = to_wire(parse_reject(kv.at("reason")));
    return r;
  }

  std::string side;
  in >> side;
  r.side = to_wire(parse_side(side));
  auto kv = keyed(in);
  r.qty = u32(kv.at("qty"));
  r.price = std::stoll(kv.at("px"));
  if (verb == "ack") {
    r.type = to_wire(OutType::Ack);
    r.leaves = u32(kv.at("leaves"));
  } else if (verb == "fill" || verb == "trade") {
    r.type = to_wire(verb == "fill" ? OutType::Fill : OutType::Trade);
    r.contra_id = std::stoull(kv.at("contra"));
    r.leaves = u32(kv.at("leaves"));
    r.flags = kv.contains("aggr") ? kFlagAggressor : std::uint8_t{0};
  } else if (verb == "canceled") {
    r.type = to_wire(OutType::Canceled);
    r.reason = to_wire(parse_cancel(kv.at("reason")));
  } else if (verb == "modified") {
    r.type = to_wire(OutType::Modified);
    r.leaves = u32(kv.at("leaves"));
    r.flags = kv.contains("lost") ? kFlagPriorityLost : std::uint8_t{0};
  } else {
    throw std::runtime_error("bad output verb: " + verb);
  }
  return r;
}

}  // namespace

Scenario parse_scenario(const std::filesystem::path& file) {
  std::ifstream in(file);
  if (!in) {
    throw std::runtime_error("cannot open " + file.string());
  }
  Scenario scenario;
  scenario.name = file.stem().string();
  scenario.config.price_min = 0;
  scenario.config.price_max = 1000;
  scenario.config.max_orders = 16;

  std::uint64_t seq = 0;
  std::string line;
  while (std::getline(in, line)) {
    if (line.empty() || line[0] == '#') {
      continue;
    }
    const std::string body = line.size() > 2 ? line.substr(2) : "";
    try {
      if (line[0] == '>') {
        scenario.steps.push_back({.input = parse_input(body), .expected = {}});
      } else if (line[0] == '<') {
        scenario.steps.at(scenario.steps.size() - 1).expected.push_back(parse_output(body, ++seq));
      } else if (line.starts_with("@config")) {
        std::istringstream words(line.substr(7));
        for (const auto& [key, value] : keyed(words)) {
          if (key == "price_min") {
            scenario.config.price_min = std::stoll(value);
          } else if (key == "price_max") {
            scenario.config.price_max = std::stoll(value);
          } else if (key == "max_orders") {
            scenario.config.max_orders = static_cast<std::uint32_t>(std::stoul(value));
          } else {
            fail(file.string(), line, "unknown config key");
          }
        }
      } else if (line[0] == '=') {
        std::istringstream words(body);
        std::string verb;
        words >> verb;
        if (verb == "live") {
          std::uint32_t live = 0;
          words >> live;
          scenario.live = live;
        } else if (verb == "hash") {
          std::string text;
          words >> text;
          scenario.hash = std::stoull(text, nullptr, 16);
        } else if (verb == "depth") {
          std::string side;
          words >> side;
          auto kv = keyed(words);
          scenario.depth_checks.push_back({parse_side(side), std::stoll(kv.at("px")),
                                           std::stoull(kv.at("qty")),
                                           std::stoull(kv.at("orders"))});
        } else {
          fail(file.string(), line, "unknown check");
        }
      } else {
        fail(file.string(), line, "unknown line");
      }
    } catch (const std::logic_error& e) {
      fail(file.string(), line, e.what());
    }
  }
  return scenario;
}

}  // namespace llme::golden
