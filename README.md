# Low-latency matching engine

> A price–time priority order book in C++ or Rust, tested against a reference and measured in nanoseconds.

**Status:** in build, see the milestones in `docs/decisions.md`

| | |
|---|---|
| Track | Quant Developer |
| Estimated effort | 4–6 weeks |
| Stack | C++20 or Rust, CMake, Google Benchmark |
| Data cost | Free |
| Strongest for | HFT quant dev, exchange technology |

## Requirements

- [x] Price–time priority with limit, market, IOC, FOK, cancel and modify (priority lost on price change or size increase). FR-02 to FR-10, AC-W-02.
- [x] Hot path built on price-level arrays or intrusive lists, an order-ID hash map and preallocated pools, with zero heap allocation. FR-11, FR-12, AC-W-06.
- [x] A single-threaded matching core fed by lock-free SPSC ring buffers, with deterministic replay from an input log. FR-13 to FR-15, AC-W-09.
- [x] A simple, obviously correct reference implementation for differential testing. FR-16.
- [ ] Benchmarks with Google Benchmark and HDR histograms; profiling with perf or VTune.
- [x] A clean build: CMake, CI, ASan, UBSan, TSan and clang-tidy. FR-33, AC-W-03, AC-W-07.

## Data

Raw and licensed data live in `data/`, which is git-ignored. Only download scripts and small synthetic fixtures are committed.

| Dataset | Cost | What it gives you |
|---|---|---|
| Synthetic order-flow generator | Free | Your own code: tunable arrival rates, price distributions and cancel ratios. |
| [Nasdaq ITCH 5.0 sample files](https://emi.nasdaq.com/ITCH/Nasdaq%20ITCH/) | Free | Replay real order flow for a realistic mix of adds, cancels and modifies. |
| [LOBSTER samples](https://lobsterdata.com) | Free samples | Clean message files for a quick, realistic workload. |

## Success criteria

### Works (correct and reproducible)

- [x] Differential testing: 100M+ random operations produce fills identical to the reference engine. AC-W-01 for D2, `results/diff/campaign_2026-11-02.json`.
- [x] All sanitizers are clean, and hours of fuzzing find nothing. AC-W-03, AC-W-04: 4 h on each of four targets, `results/fuzz/`.
- [ ] Deterministic output: the same input log always gives the same output hash. AC-W-05 met for D2 and the reference, including one ITCH log, `results/determinism.csv`; D1 and D3 come with M6.

### Impresses hirers

- [ ] Latency distributions (p50, p99, p99.9) for add, cancel and match, published with hardware specs. A p50 in the tens of nanoseconds is a strong number.
- [ ] Throughput of several million messages per second per core on real ITCH flow.
- [ ] A writeup comparing two or three data-structure designs with cache-miss counts from perf stat, explaining why the winner wins.
- [ ] Proof of zero hot-path allocations, through an allocator hook or heaptrack.

## Questions this project must answer

- Why not std::map for price levels?
- What happens to your p99.9 under a burst of cancels?
- How would you spread many symbols across cores?
