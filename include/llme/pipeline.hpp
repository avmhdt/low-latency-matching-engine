// Three-thread pipeline (FR-14): gateway -> in-ring -> matching core -> out-ring -> publisher.
//
// The gateway pushes the log in order, then an end-of-log sentinel. The core is the only thread
// touching the engine, and the publisher hashes every output record. Waits are busy spins, so the
// run never makes a system call between the ready barrier and the last message. Output depends
// only on the input bytes: the core sees one totally ordered stream, so ring timing cannot
// change it, and the digest is identical to an in-process run.
//
// Thread-safety: run_pipeline owns its threads; call it from one thread at a time.
#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <span>

#include "llme/messages.hpp"
#include "llme/spsc_ring.hpp"
#include "llme/types.hpp"

namespace llme {

inline constexpr std::size_t kInRingCapacity = 65536;
inline constexpr std::size_t kOutRingCapacity = 262144;

using InRing = SpscRing<InMsg, kInRingCapacity>;
using OutRing = SpscRing<OutMsg, kOutRingCapacity>;

struct PipelineCores {
  std::optional<int> gateway;
  std::optional<int> core;
  std::optional<int> publisher;
};

// Reads LLME_PRODUCER_CORE, LLME_BENCH_CORE and LLME_CONSUMER_CORE; empty or unset means unpinned.
[[nodiscard]] PipelineCores cores_from_env();

// Pins the calling thread; returns false if the kernel refuses.
[[nodiscard]] bool pin_current_thread(int core) noexcept;

struct PipelineOptions {
  PipelineCores cores;
  std::optional<std::filesystem::path> olog;  // write output records here, off the hot path
  // Called once with all three threads spinning at the start line, before any message moves, and
  // once after they have finished. The alloc test arms its hook here.
  std::function<void()> on_threads_ready;
  std::function<void()> on_threads_done;
};

struct PipelineResult {
  std::uint64_t output_hash = 0;
  std::uint64_t out_records = 0;
  EngineCounters counters;
  std::uint64_t in_ring_full_spins = 0;   // gateway spins on a full in-ring
  std::uint64_t out_ring_full_spins = 0;  // core spins on a full out-ring
  std::size_t max_in_depth = 0;
  std::size_t max_out_depth = 0;
  double seconds = 0.0;
};

// Runs msgs through a D2 engine; throws std::runtime_error if a thread cannot be pinned or the
// olog cannot be written.
[[nodiscard]] PipelineResult run_pipeline(std::span<const InMsg> msgs, const EngineConfig& cfg,
                                          const PipelineOptions& options);

// The same engine and hash on the calling thread, with no rings: the reference for FR-15.
[[nodiscard]] PipelineResult run_in_process(std::span<const InMsg> msgs, const EngineConfig& cfg);

}  // namespace llme
