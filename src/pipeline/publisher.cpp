#include <array>
#include <cstddef>

#include "stages.hpp"

namespace llme::stages {

void run_publisher(OutRing& ring, OutputHasher& hasher, OlogWriter* olog,
                   PublisherStats& stats) noexcept {
  constexpr std::size_t kBatch = 1024;
  std::array<OutMsg, kBatch> batch;
  while (true) {
    const std::size_t count = ring.pop_batch(batch.data(), batch.size());
    if (count == 0) {
      spin_pause();
    }
    for (std::size_t i = 0; i < count; ++i) {
      const OutMsg& r = batch[i];
      if (r.type == 0) {
        return;
      }
      hasher.update(r);
      ++stats.out_records;
      if (olog != nullptr && !olog->write(r)) {
        stats.write_failed = true;
      }
    }
  }
}

}  // namespace llme::stages
