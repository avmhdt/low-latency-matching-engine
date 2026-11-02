// Google Benchmark component suite (FR-24). Numbers from here are published only from the
// bench preset on native Linux; CI just compiles and runs it briefly.
#include <benchmark/benchmark.h>

#include <cstdint>
#include <vector>

#include "llme/engine.hpp"
#include "llme/id_map.hpp"
#include "llme/output_hash.hpp"
#include "llme/price_ladder.hpp"

namespace {

using llme::Side;

constexpr std::uint32_t kMapEntries = 1U << 20U;

llme::InMsg limit(llme::OrderId id, Side side, llme::Qty qty, llme::Price price,
                  llme::Tif tif = llme::Tif::Gtc) {
  llme::InMsg m;
  m.type = llme::to_wire(llme::MsgType::New);
  m.side = llme::to_wire(side);
  m.tif = llme::to_wire(tif);
  m.qty = qty;
  m.order_id = id;
  m.price = price;
  return m;
}

llme::EngineConfig bench_config() {
  return {.price_min = 0, .price_max = (1 << 20) - 1, .max_orders = 1U << 20U};
}

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

// Arg: load factor in percent. The map is filled once and each iteration touches one key.
void bm_id_map_find(benchmark::State& state) {
  const double load = static_cast<double>(state.range(0)) / 100.0;
  llme::IdMap map(kMapEntries, load);
  const auto count = static_cast<std::uint32_t>(static_cast<double>(map.capacity()) * load);
  for (std::uint32_t i = 0; i < count; ++i) {
    map.insert(i * 7ULL + 1, i);
  }
  std::uint64_t key = 0;
  for (auto _ : state) {
    key = (key + 1) % count;
    benchmark::DoNotOptimize(map.find(key * 7 + 1));
  }
}
BENCHMARK(bm_id_map_find)->Arg(50)->Arg(90);

void bm_id_map_insert_erase(benchmark::State& state) {
  const double load = static_cast<double>(state.range(0)) / 100.0;
  llme::IdMap map(kMapEntries, load);
  const auto count = static_cast<std::uint32_t>(static_cast<double>(map.capacity()) * load);
  for (std::uint32_t i = 0; i < count; ++i) {
    map.insert(i * 7ULL + 1, i);
  }
  std::uint64_t key = 0xFFFFFFFFULL;
  for (auto _ : state) {
    ++key;
    map.insert(key, 1);
    map.erase(key);
  }
}
BENCHMARK(bm_id_map_insert_erase)->Arg(50)->Arg(90);

void bm_level_insert_unlink(benchmark::State& state) {
  llme::OrderPool pool(1024);
  llme::Level level;
  for (std::uint32_t i = 0; i < 16; ++i) {
    const std::uint32_t index = pool.allocate();
    pool[index].leaves = 1;
    llme::level_push_back(pool, level, index);
  }
  for (auto _ : state) {
    const std::uint32_t index = pool.allocate();
    pool[index].leaves = 1;
    llme::level_push_back(pool, level, index);
    benchmark::DoNotOptimize(level.qty);
    llme::level_unlink(pool, level, index);
    pool.release(index);
    benchmark::ClobberMemory();
  }
}
BENCHMARK(bm_level_insert_unlink);

// Empties the best level so the next best is found through the bitmap. Arg: gap between levels.
void bm_best_price_search(benchmark::State& state) {
  const llme::EngineConfig config = bench_config();
  llme::LadderLevelStore ladder(config);
  const llme::Price gap = state.range(0);
  for (llme::Price price = 0; price < 4096 && price * gap <= config.price_max; ++price) {
    ladder.level(Side::Sell, price * gap).count = 1;
  }
  for (auto _ : state) {
    ladder.level(Side::Sell, 0).count = 0;
    ladder.on_level_empty(Side::Sell, 0);
    benchmark::DoNotOptimize(ladder.best(Side::Sell));
    ladder.level(Side::Sell, 0).count = 1;
  }
}
BENCHMARK(bm_best_price_search)->Arg(1)->Arg(64)->Arg(4096);

// An aggressor that sweeps N one-lot levels, which are rested again outside the timer.
void bm_sweep(benchmark::State& state) {
  const auto levels = static_cast<llme::Qty>(state.range(0));
  llme::MatchingEngine<llme::LadderLevelStore, llme::NullSink> engine(bench_config(),
                                                                      llme::NullSink{});
  llme::OrderId next_id = 1;
  for (auto _ : state) {
    state.PauseTiming();
    for (llme::Qty i = 0; i < levels; ++i) {
      engine.on_message(limit(next_id++, Side::Sell, 1, 500000 + i));
    }
    const llme::InMsg aggressor =
        limit(next_id++, Side::Buy, levels, 500000 + levels, llme::Tif::Ioc);
    state.ResumeTiming();
    engine.on_message(aggressor);
  }
}
BENCHMARK(bm_sweep)->Arg(1)->Arg(5)->Arg(20);

}  // namespace

BENCHMARK_MAIN();
