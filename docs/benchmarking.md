# Benchmarking

M6 completes this document with the method, the timer overhead, coordinated omission and the results. This first part is the plan for the rented bare-metal session (D-02, D-03), written so that machine time is spent measuring rather than setting up.

## Method

### Latency (`llme-bench-latency`, FR-23)

- **Timer.** Each message is timed alone. `lfence; rdtsc` comes before `on_message`, and `rdtscp; lfence` after, so that out-of-order execution cannot move work across either read. Ticks become nanoseconds through a frequency calibrated against `steady_clock` over 200 ms at start-up, which assumes an invariant TSC; `hw_manifest.sh` records `constant_tsc` and `nonstop_tsc`.
- **Timer overhead.** The p50 of a million back-to-back read pairs is measured on every run and reported next to every percentile (`timer_overhead_p50_ns`), because at tens of nanoseconds it is a large share of the number (R-03).
- **Op classes.** add is a NEW that rests with no fill, match is a NEW with at least one fill, then cancel and modify. Rejects, and IOC or FOK orders that end without a fill, are not in any class.
- **Runs.** The first `--warmup` messages are not timed. Each of `--runs` runs builds a fresh engine, so no run inherits a warm book. Each percentile is the median across runs, with the spread of p50 reported. A spread over 5% of the median marks the class `unstable` (NFR-09); it is flagged, never dropped. The `.hgrm` files hold all runs merged.
- **Pipeline mode.** The gateway releases message i at start + i / rate, whatever the core is doing, and latency runs from that intended time to the core finishing the message. A stall therefore counts against every message queued behind it instead of hiding them, which is the coordinated-omission error that closed-loop timing makes (R-06). This measures in-ring wait plus matching; output goes to a null sink, so there is no out-ring or publisher in this figure. The core thread takes `--core` and the gateway `LLME_PRODUCER_CORE`; the tool refuses to put both on one CPU, where two spinning threads would measure only each other. It is one pass with no warm-up, and its summary says so.

### Throughput (`llme-bench-throughput`, FR-25)

- **Per core.** Messages over the median wall time of the timed loop, for one engine per symbol in one matching thread. In pipeline mode the gateway and publisher run on their own cores, and the figure is still per matching core.
- **Top 50 in day order.** `llme-itch-decode` writes `itch_top_<date>.interleave`, the symbol of every message in its original order across symbols, so all 50 books are live together as they were on the day. Logs passed without it run one after another, and the output says `concatenated`.

### Cancel bursts (`llme-bench-burst`, FR-28)

- **Phases.** The same profile and seed run without bursts and then with bursts of each length, aimed at uniform and at near-touch orders. Every message is timed, and the generator marks which ones came from a burst, so tails are reported inside and outside the bursts per op class.
- **Probe lengths.** ID-map probe lengths are a table walk, so they are sampled at each burst boundary and at the end, never per message.

### What every tool records

- **Thread state.** Pinning, and whether memory was locked. `mlockall` runs only when `RLIMIT_MEMLOCK` is unlimited, because locking future mappings under a finite limit makes every later large allocation fail. The engines prefault their memory anyway, so locking only guards against swapping.
- **Run record.** `environment`, `native` or `WSL2 smoke`, plus host, git SHA and an experiment-log row.

## WSL2 smoke runs

All three tools refuse to write into the repository's `results/` when they run under WSL2: `LLME_RESULTS_DIR` has to point elsewhere. WSL2 output is a smoke check that the tools work. It is never a published number, and `Bench.Smoke` in the test suite writes only to the build tree.

## Host

- **Provider:** Cherry Servers, hourly bare metal (owner decision, recorded in `docs/decisions.md`).
- **Machine:** Ryzen 9 7950X (Zen 4) at $0.399 per hour if one is in stock. Otherwise Ryzen 9 9950X (Zen 5) at $0.70 per hour.
- **Console and OS:** IP-KVM console, Ubuntu 24.04 (22.04 also works).
- **Labelling:** the spec's reference examples are Raptor Lake and Zen 4. `scripts/hw_manifest.sh` writes a `uarch` field into every manifest, and every published number names the host's microarchitecture. A 9950X result is labelled Zen 5 wherever it appears, and is never presented as Zen 4.
- **No market data is copied to the server.** The ITCH day is downloaded there from Nasdaq, under the approved D-05 file name, and checked against the SHA-256 that itch-feed-handler recorded. Synthetic logs are generated on the host. LOBSTER is not needed for M6.

## What the owner does on Cherry Servers

Agents never handle the account, credentials or payment.

1. Create the account and add a payment method.
2. Order an hourly server: a 7950X, or a 9950X if no 7950X is in stock, with Ubuntu 24.04 and your SSH public key. Note the hourly rate and the time the order completes (UTC).
3. Before ordering, check Cherry's hourly terms: minimum billing period, IPv4 or bandwidth charges, and whether a powered-off server still bills. The cost estimate below assumes none of these apply.
4. Log in over SSH, then run:
   ```
   git clone https://github.com/avmhdt/low-latency-matching-engine
   cd low-latency-matching-engine
   git checkout <commit given with the M6 PR>
   scripts/bare_metal_setup.sh
   scripts/bare_metal_setup.sh --apply
   ```
   The first command only prints what the second will do.
5. Run `scripts/bench_env.sh --plan`, review it, and run its commands (P-05). Step 2 reboots the machine; watch it on the IP-KVM console. Then run `scripts/bench_env.sh`. Every line should read OK, and any that cannot is recorded as it is.
6. Run `scripts/pmu_check.sh`. **If it does not print PASS, stop and delete the server.** Without counters, AC-H-03 cannot be met on that host.
7. Run the session:
   ```
   LLME_RATE_USD_PER_HOUR=<rate> LLME_RENTED_AT=<UTC time of step 2> scripts/bare_metal_run.sh
   ```
   `--dry-run` first shows the plan and the costs.
8. Copy the bundle it prints (`~/llme-results-<host>-<time>.tar.gz`) back to this PC, for example with `scp`, and put it in the repository folder. The agent checks it and commits the results.
9. Delete the server in the Cherry Servers portal. Billing stops only when the server is deleted.

## Run list

`scripts/bare_metal_run.sh` runs the steps in this order. Each one is resumable, and a step whose tool does not exist yet is skipped and listed.

| # | Step | What it produces | Estimate |
|---|---|---|---|
| — | Set-up (owner): `bare_metal_setup.sh --apply` | toolchain, perf, FlameGraph, release and bench builds, test pass | 23 min |
| — | D-08 (owner): `bench_env.sh --plan` with reboot | isolated cores, governor, boost, SMT, counters | 15 min |
| 1 | `pmu_check` | `results/perf/pmu_check_<host>.json`; **stops the session on failure** | 1 min |
| 2 | `manifest` | `results/manifests/<host>.json` and its bench_env check | 1 min |
| 3 | `determinism` | pinned hashes reproduced on a new CPU and kernel (AC-W-05) | 5 min |
| 4 | `synthetic` | 10M-message balanced, cancel-heavy and burst logs | 3 min |
| 5 | `itch_fetch` | the 2019-01-30 ITCH file from Nasdaq, SHA-256 checked | 10 min |
| 6 | `itch_decode` | top 50 symbols to `.mlog` (D-05) | 6 min |
| 7 | `gbench` | component benchmarks, 5 repetitions (FR-24) | 10 min |
| 8 | `latency` | D1, D2 and D3 on balanced and QQQ, 5 runs each (FR-23, AC-H-01) | 20 min |
| 9 | `latency_pipeline` | ring-to-ring latency at a fixed open-loop rate | 15 min |
| 10 | `throughput` | D1, D2 and D3, in-process and pipeline, top 1 and top 50, 5 runs each (FR-25, AC-H-02) | 30 min |
| 11 | `perf_stat` | counters per message for each design (FR-27, AC-H-03) | 20 min |
| 12 | `flamegraph` | D2 on QQQ (AC-H-06) | 10 min |
| 13 | `heaptrack` | allocations around the timed loop (AC-H-04) | 20 min |
| 14 | `burst` | 1k, 10k and 100k cancel bursts (FR-28, AC-H-05) | 20 min |
| — | Bundle and copy back | one tarball | 5 min |

The total is about 3.6 h. With 50% contingency for reruns and surprises, call it 5.4 h.

## Cost against the cap

| Machine | Rate | 3.6 h | 5.4 h with contingency | A second session of the same length | 80% stop |
|---|---|---|---|---|---|
| 7950X (Zen 4) | $0.399/h | $1.44 | $2.15 | $4.31 in total | $40, after about 100 h |
| 9950X (Zen 5) | $0.70/h | $2.52 | $3.78 | $7.56 in total | $40, after about 57 h |

The cap is USD 50 in total (D-03). Before each step, `bare_metal_run.sh` adds that step's estimate to the cost so far, counted from the moment of renting. If the sum would pass $40, 80% of the cap, it bundles the results and stops so that the server can be deleted. Idle time bills too, which is why the run list is scripted end to end.

## Before renting

Every step's tool exists and runs: `llme-bench-latency`, `llme-bench-throughput`, `llme-bench-burst`, the D1 and D3 level stores, `perf_stat.sh`, `perf_flamegraph.sh` and `heaptrack.sh`. Before renting, the whole run list is rehearsed on this PC:

```
LLME_RESULTS_DIR=$HOME/build/low-latency-matching-engine/rehearsal/results LLME_RATE_USD_PER_HOUR=0 \
  LLME_RENTED_AT=$(date -u +%Y-%m-%dT%H:%M:%SZ) scripts/bare_metal_run.sh --rehearse
```

`--rehearse` runs every step for real at small scale: one run instead of five, the first 300,000 messages of each log, 1M-message synthetic logs, and software events (`task-clock`, `context-switches`) in `perf_stat.sh`, since WSL2 has no hardware counters. It writes outside `results/`, goes on past the PMU check, and reuses `LLME_ITCH_FILE` instead of downloading the day again. Its numbers mean nothing; it only proves that every command works, so that rented time goes on measuring.

Two things the rehearsal cannot check. The hardware counter events are known to work only once `pmu_check.sh` passes on the host. And WSL2's `perf` cannot resolve symbols in this build, so its flame graph shows raw addresses; on native Ubuntu, `perf` reads the symbols from the bench binaries, which keep frame pointers.

## Hardware counters on Zen

Zen's PMU has no exact equivalent of perf's generic `LLC-load-misses`, which often reads as unsupported there. `pmu_check.sh` records, for each counter of FR-27 and for AMD alternatives (`ls_dmnd_fills_from_sys.mem_io_local`, `ls_any_fills_from_sys.dram_io_all`, `l2_cache_req_stat.ic_dc_miss_in_l2`), whether it works. `perf_stat.sh` uses the generic event where it is supported. Otherwise it uses the closest AMD event and names it in `results/perf/counters.csv`, so no column is ever filled with a stand-in under the generic name.
