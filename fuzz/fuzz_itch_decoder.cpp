// Arbitrary bytes as an ITCH stream (FR-20): framing, message parsing and the replay mapping.
// The two lowest stock locates are selected, so that mutated messages reach the builders.
#include <cstddef>
#include <cstdint>
#include <span>

#include "itch_decoder.hpp"

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  std::uint64_t messages = 0;
  llme::workload::ReplayBuilder first([&](const llme::InMsg&) { ++messages; });
  llme::workload::ReplayBuilder second([&](const llme::InMsg&) { ++messages; });
  llme::workload::ItchConverter converter;
  converter.select(0, &first);
  converter.select(1, &second);

  llme::workload::MemorySource source({data, size});
  llme::workload::ItchFrameReader reader(source);
  std::span<const std::uint8_t> message;
  llme::workload::ItchEvent event;
  while (reader.next(message)) {
    bool malformed = false;
    if (llme::workload::parse_itch(message, event, malformed)) {
      (void)llme::workload::stock_name(event.stock);
      converter.on_event(event);
    }
  }
  (void)llme::workload::replay_config(first.stats());
  return 0;
}
