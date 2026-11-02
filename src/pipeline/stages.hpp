// Internals shared by the three pipeline threads.
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <span>

#include "llme/mlog.hpp"
#include "llme/output_hash.hpp"
#include "llme/pipeline.hpp"

namespace llme::stages {

// Marks the end of the log in InMsg::reserved. The gateway clears that field on every message it
// forwards, so no input can carry it. On the out-ring, type 0 plays the same part: the engine
// never emits it.
inline constexpr std::uint32_t kEndOfLog = 0xE0F0E0F0U;

// Depth is sampled, so that measuring it does not become a stream of shared cache-line reads.
inline constexpr std::uint64_t kDepthSampleMask = 1023;

inline void spin_pause() noexcept {
#if defined(__x86_64__)
  __builtin_ia32_pause();
#endif
}

struct GatewayStats {
  std::uint64_t full_spins = 0;
  std::size_t max_depth = 0;
};

struct PublisherStats {
  std::uint64_t out_records = 0;
  bool write_failed = false;
};

// Pushes every message and the sentinel, spinning while the ring is full.
void run_gateway(std::span<const InMsg> msgs, InRing& ring, GatewayStats& stats) noexcept;

// Drains the ring into the hasher, and the olog if there is one, until the sentinel.
void run_publisher(OutRing& ring, OutputHasher& hasher, OlogWriter* olog,
                   PublisherStats& stats) noexcept;

}  // namespace llme::stages
