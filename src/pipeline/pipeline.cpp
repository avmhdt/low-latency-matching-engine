#include "llme/pipeline.hpp"

#include <pthread.h>
#include <sched.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>

#include "llme/engine.hpp"
#include "stages.hpp"

namespace llme {

namespace {

using stages::kDepthSampleMask;
using stages::kEndOfLog;
using stages::spin_pause;

std::optional<int> core_from_env(const char* name) {
  // NOLINTNEXTLINE(concurrency-mt-unsafe): read once at start-up, before any thread exists
  const char* value = std::getenv(name);
  if (value == nullptr || *value == '\0') {
    return std::nullopt;
  }
  return std::stoi(value);
}

// All three threads spin here, so the first message moves with every thread already running and
// the caller can arm its allocation hook after the last thread start-up allocation.
class StartLine {
 public:
  void arrive() noexcept { ready_.fetch_add(1, std::memory_order_acq_rel); }
  [[nodiscard]] bool all_arrived() const noexcept {
    return ready_.load(std::memory_order_acquire) == kThreads;
  }
  void open() noexcept { go_.store(true, std::memory_order_release); }
  void wait_for_open() const noexcept {
    while (!go_.load(std::memory_order_acquire)) {
      spin_pause();
    }
  }

 private:
  static constexpr int kThreads = 3;
  std::atomic<int> ready_{0};
  std::atomic<bool> go_{false};
};

struct RingSink {
  OutRing* ring;
  std::uint64_t* full_spins;

  void operator()(const OutMsg& m) const noexcept {
    while (!ring->try_push(m)) {
      ++*full_spins;
      spin_pause();
    }
  }
};

struct CoreStats {
  std::uint64_t full_spins = 0;
  std::size_t max_depth = 0;
};

// The hot loop: pop, match, repeat. The engine pushes its output through RingSink.
void run_core(MatchingEngine<LadderLevelStore, RingSink>& engine, InRing& in_ring,
              OutRing& out_ring, CoreStats& stats) noexcept {
  InMsg m;
  std::uint64_t handled = 0;
  while (true) {
    if (!in_ring.try_pop(m)) {
      spin_pause();
      continue;
    }
    if (m.reserved == kEndOfLog) {
      break;
    }
    engine.on_message(m);
    if ((++handled & kDepthSampleMask) == 0) {
      stats.max_depth = std::max(stats.max_depth, out_ring.approx_size());
    }
  }
  const OutMsg sentinel;
  RingSink{&out_ring, &stats.full_spins}(sentinel);
}

}  // namespace

PipelineCores cores_from_env() {
  return {.gateway = core_from_env("LLME_PRODUCER_CORE"),
          .core = core_from_env("LLME_BENCH_CORE"),
          .publisher = core_from_env("LLME_CONSUMER_CORE")};
}

bool pin_current_thread(int core) noexcept {
  if (core < 0 || core >= CPU_SETSIZE) {
    return false;
  }
  cpu_set_t set;
  CPU_ZERO(&set);
  CPU_SET(static_cast<std::size_t>(core), &set);
  return pthread_setaffinity_np(pthread_self(), sizeof(set), &set) == 0;
}

PipelineResult run_pipeline(std::span<const InMsg> msgs, const EngineConfig& cfg,
                            const PipelineOptions& options) {
  const auto in_ring = std::make_unique<InRing>();
  const auto out_ring = std::make_unique<OutRing>();

  std::optional<OlogWriter> olog;
  if (options.olog) {
    auto writer = OlogWriter::create(*options.olog);
    if (!writer) {
      throw std::runtime_error(std::string(to_string(writer.error())) + ": " +
                               options.olog->string());
    }
    olog.emplace(std::move(*writer));
  }

  CoreStats core_stats;
  MatchingEngine<LadderLevelStore, RingSink> engine(
      cfg, RingSink{out_ring.get(), &core_stats.full_spins});
  stages::GatewayStats gateway_stats;
  stages::PublisherStats publisher_stats;
  OutputHasher hasher;
  StartLine start;
  std::atomic<bool> pin_failed{false};

  const auto enter = [&](const std::optional<int>& pin) {
    if (pin && !pin_current_thread(*pin)) {
      pin_failed.store(true, std::memory_order_relaxed);
    }
    start.arrive();
    start.wait_for_open();
  };

  std::thread gateway([&] {
    enter(options.cores.gateway);
    stages::run_gateway(msgs, *in_ring, gateway_stats);
  });
  std::thread core([&] {
    enter(options.cores.core);
    run_core(engine, *in_ring, *out_ring, core_stats);
  });
  std::thread publisher([&] {
    enter(options.cores.publisher);
    stages::run_publisher(*out_ring, hasher, olog ? &*olog : nullptr, publisher_stats);
  });

  while (!start.all_arrived()) {
    spin_pause();
  }
  if (options.on_threads_ready) {
    options.on_threads_ready();
  }
  const auto began = std::chrono::steady_clock::now();
  start.open();
  gateway.join();
  core.join();
  publisher.join();
  const std::chrono::duration<double> elapsed = std::chrono::steady_clock::now() - began;
  if (options.on_threads_done) {
    options.on_threads_done();
  }

  if (pin_failed.load(std::memory_order_relaxed)) {
    throw std::runtime_error("cannot pin a pipeline thread to its core");
  }
  if (olog) {
    if (auto closed = olog->close(); !closed || publisher_stats.write_failed) {
      throw std::runtime_error("cannot write " + options.olog.value_or("output log").string());
    }
  }

  PipelineResult result;
  result.output_hash = hasher.digest();
  result.out_records = publisher_stats.out_records;
  result.counters = engine.counters();
  result.in_ring_full_spins = gateway_stats.full_spins;
  result.out_ring_full_spins = core_stats.full_spins;
  result.max_in_depth = gateway_stats.max_depth;
  result.max_out_depth = core_stats.max_depth;
  result.seconds = elapsed.count();
  return result;
}

PipelineResult run_in_process(std::span<const InMsg> msgs, const EngineConfig& cfg) {
  struct HashSink {
    OutputHasher* hasher;
    void operator()(const OutMsg& m) const noexcept { hasher->update(m); }
  };
  OutputHasher hasher;
  MatchingEngine<LadderLevelStore, HashSink> engine(cfg, HashSink{&hasher});
  const auto began = std::chrono::steady_clock::now();
  for (const InMsg& m : msgs) {
    engine.on_message(m);
  }
  const std::chrono::duration<double> elapsed = std::chrono::steady_clock::now() - began;

  PipelineResult result;
  result.output_hash = hasher.digest();
  result.counters = engine.counters();
  result.out_records = result.counters.out_records;
  result.seconds = elapsed.count();
  return result;
}

}  // namespace llme
