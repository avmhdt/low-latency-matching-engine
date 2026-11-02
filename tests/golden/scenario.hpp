// Golden scenario DSL (tests/golden/*.scn), hand-written from docs/design.md.
//
//   # comment
//   @config price_min=0 price_max=1000 max_orders=16
//   > new <id> buy|sell gtc|ioc|fok <qty> @<price>
//   > new <id> buy|sell market <qty>
//   > cancel <id>
//   > modify <id> qty=<q> px=<p>
//   > raw type=<t> side=<s> ord=<o> tif=<t> qty=<q> id=<i> px=<p>
//   < ack <id> buy|sell qty=<q> px=<p> leaves=<l>
//   < reject <id> side=<raw> reason=<NAME>
//   < fill <id> buy|sell qty=<q> px=<p> contra=<id> leaves=<l> [aggr]
//   < trade <id> buy|sell qty=<q> px=<p> contra=<id> leaves=<l>
//   < canceled <id> buy|sell qty=<q> px=<p> reason=<NAME>
//   < modified <id> buy|sell qty=<q> px=<p> leaves=<l> [lost]
//   < cancel_reject <id> reason=<NAME>
//   < modify_reject <id> reason=<NAME>
//   < level buy|sell px=<p> qty=<q> [removed]
//   = depth buy|sell px=<p> qty=<q> orders=<n>
//   = live <n>
//   = hash 0x<hex>
//
// `seq` is implicit: expected records number from 1, gap-free. Each `<` line expects the record
// produced by the most recent `>` line, in order, and a `>` line checks that the previous input
// produced nothing more than what was listed.
#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "llme/messages.hpp"
#include "llme/types.hpp"

namespace llme::golden {

struct Step {
  InMsg input;
  std::vector<OutMsg> expected;
};

struct DepthCheck {
  Side side = Side::Buy;
  Price price = 0;
  std::uint64_t qty = 0;
  std::size_t orders = 0;
};

struct Scenario {
  std::string name;
  EngineConfig config;
  std::vector<Step> steps;
  std::vector<DepthCheck> depth_checks;
  std::optional<std::uint32_t> live;
  std::optional<std::uint64_t> hash;
};

Scenario parse_scenario(const std::filesystem::path& file);

}  // namespace llme::golden
