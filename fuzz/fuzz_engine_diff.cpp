// Bytes decoded into an operation sequence, run through D1, D2, D3 and the reference; aborts on any
// output difference, final-book difference or invariant failure (FR-20).
//
// The book is deliberately tiny, 32 prices, 32 IDs and a pool of 16, so that a mutated input keeps
// hitting empty sides, level churn, ID reuse and POOL_FULL. Input format, 6 bytes per operation:
//   byte 0: operation; 0-6 GTC, 7 IOC, 8 FOK, 9 market, 10-11 cancel, 12-13 modify, 14-15 raw
//   byte 1: bit 0 side, bits 1-5 order ID
//   byte 2: price; values at or above 224 land outside the band
//   byte 3: qty modulo 16; 255 means the largest u32
//   bytes 4-5: raw type and flag bytes for raw operations
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <stdexcept>
#include <vector>

#include "engine_adapters.hpp"
#include "llme/messages.hpp"

namespace {

constexpr std::size_t kOpBytes = 6;
constexpr std::uint8_t kOpCases = 16;

llme::InMsg decode(const std::uint8_t* op) {
  llme::InMsg m;
  const std::uint8_t kind = op[0] % kOpCases;
  m.side = op[1] & 1U;
  m.order_id = 1U + ((op[1] >> 1U) & 31U);
  m.price = op[2] >= 224 ? 32 + (op[2] - 224) : op[2] % 32;
  m.qty = op[3] == 255 ? 0xFFFFFFFFU : op[3] % 16U;
  m.trader_id = op[4];
  if (kind <= 9) {
    m.type = llme::to_wire(llme::MsgType::New);
    m.tif = llme::to_wire(kind == 7 ? llme::Tif::Ioc : kind == 8 ? llme::Tif::Fok : llme::Tif::Gtc);
    m.ord_type = llme::to_wire(kind == 9 ? llme::OrdType::Market : llme::OrdType::Limit);
  } else if (kind <= 11) {
    m.type = llme::to_wire(llme::MsgType::Cancel);
  } else if (kind <= 13) {
    m.type = llme::to_wire(llme::MsgType::Modify);
  } else {
    m.type = op[4];
    m.side = op[5] & 7U;
    m.ord_type = (op[5] >> 3U) & 3U;
    m.tif = (op[5] >> 5U) & 7U;
  }
  return m;
}

[[noreturn]] void die(const char* what) {
  std::fputs(what, stderr);
  std::fputc('\n', stderr);
  std::abort();
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  const llme::EngineConfig config{.price_min = 0, .price_max = 31, .max_orders = 16};
  const auto reference = llme::make_adapter("ref", config);
  const std::array<std::unique_ptr<llme::EngineAdapter>, 3> fast = {
      llme::make_adapter("d2", config), llme::make_adapter("d1", config),
      llme::make_adapter("d3", config)};

  std::vector<llme::OutMsg> out_fast;
  std::vector<llme::OutMsg> out_reference;
  for (std::size_t i = 0; i + kOpBytes <= size; i += kOpBytes) {
    const llme::InMsg m = decode(data + i);
    out_reference.clear();
    reference->on_message(m, out_reference);
    for (const auto& engine : fast) {
      out_fast.clear();
      engine->on_message(m, out_fast);
      if (out_fast != out_reference) {
        die("engines disagree");
      }
      try {
        engine->check_full();
      } catch (const std::logic_error& e) {
        die(e.what());
      }
    }
  }
  for (const auto& engine : fast) {
    if (engine->snapshot() != reference->snapshot()) {
      die("final books differ");
    }
  }
  return 0;
}
