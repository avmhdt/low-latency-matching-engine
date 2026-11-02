// Arbitrary bytes as a log file (FR-20). Validation runs on an in-memory image, the same code
// MlogReader::open uses after mmap, so the fuzzer does not spend its time on file I/O.
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <vector>

#include "llme/mlog.hpp"

namespace {

template <class Reader>
std::uint64_t touch_records(std::span<const std::byte> image) {
  std::uint64_t sum = 0;
  if (const auto reader = Reader::from_bytes(image)) {
    for (const auto& record : reader->records()) {
      sum += record.order_id;
    }
    sum += reader->header().record_count;
  }
  return sum;
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  // Copied into 8-byte aligned storage because a mapped file is always page aligned.
  std::vector<std::uint64_t> storage((size + 7) / 8);
  if (size != 0) {
    std::memcpy(storage.data(), data, size);
  }
  const auto image = std::as_bytes(std::span(storage)).first(size);
  volatile std::uint64_t sink = touch_records<llme::MlogReader>(image);
  sink = sink + touch_records<llme::OlogReader>(image);
  return 0;
}
