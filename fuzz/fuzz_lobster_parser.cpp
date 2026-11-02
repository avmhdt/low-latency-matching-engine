// Arbitrary bytes as LOBSTER CSV (FR-20): the message and orderbook parsers and the replay
// mapping, with the resulting log replayed through D2 on a small book.
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

#include "lobster_adapter.hpp"
#include "replay.hpp"

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  std::vector<llme::InMsg> msgs;
  llme::workload::ReplayBuilder builder([&](const llme::InMsg& m) { msgs.push_back(m); });

  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast): fuzz input read as text
  std::string_view text(reinterpret_cast<const char*>(data), size);
  while (!text.empty()) {
    const auto newline = text.find('\n');
    const std::string_view line = text.substr(0, newline);
    text = newline == std::string_view::npos ? std::string_view{} : text.substr(newline + 1);

    llme::workload::TopOfBook top;
    (void)llme::workload::parse_lobster_top(line, top);
    llme::workload::LobsterEvent event;
    if (llme::workload::parse_lobster_message(line, event)) {
      llme::workload::apply_lobster_event(event, builder);
    }
  }

  // Parsed prices can be anything, so the band stays fixed and out-of-band orders just reject.
  const llme::EngineConfig config{.price_min = 0, .price_max = 4095, .max_orders = 64};
  (void)llme::workload::replay_log(msgs, config);
  return 0;
}
