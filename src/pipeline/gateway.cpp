#include <algorithm>

#include "stages.hpp"

namespace llme::stages {

void run_gateway(std::span<const InMsg> msgs, InRing& ring, GatewayStats& stats) noexcept {
  const auto push = [&](const InMsg& m) noexcept {
    while (!ring.try_push(m)) {
      ++stats.full_spins;
      spin_pause();
    }
  };

  std::uint64_t pushed = 0;
  for (InMsg m : msgs) {
    m.reserved = 0;
    push(m);
    if ((++pushed & kDepthSampleMask) == 0) {
      stats.max_depth = std::max(stats.max_depth, ring.approx_size());
    }
  }

  InMsg sentinel;
  sentinel.reserved = kEndOfLog;
  push(sentinel);
}

}  // namespace llme::stages
