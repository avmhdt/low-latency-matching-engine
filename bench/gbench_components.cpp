// Google Benchmark component suite (FR-24). Numbers from here are published only from the
// bench preset on native Linux; CI just compiles and runs it briefly.
#include <benchmark/benchmark.h>

#include "llme/output_hash.hpp"

namespace {

void bm_output_hash_update(benchmark::State& state) {
  llme::OutputHasher hasher;
  llme::OutMsg m;
  for (auto _ : state) {
    ++m.seq;
    hasher.update(m);
  }
  benchmark::DoNotOptimize(hasher.digest());
}
BENCHMARK(bm_output_hash_update);

}  // namespace

BENCHMARK_MAIN();
