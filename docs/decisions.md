# Decisions

Project 9, `low-latency-matching-engine`. `D:\trading-strategy-brainstorm\DECISIONS.md` overrides the spec, and the spec overrides the README. The first section copies this project's entries from the owner's file; the rest records choices made during the build.

## Owner decisions (copied at M1 from DECISIONS.md, recorded 2026-10-31)

- **Merging:** agents push feature branches and open PRs. PRs merge automatically once CI is green. Branch protection and auto-merge are turned on once CI exists (M1).
- **GitHub:** `gh` active account is `avmhdt`, with admin on the repo. Actions is enabled.
- **Licence:** MIT (D-10, default).
- **Nasdaq and downloads:** sample-file terms acknowledged: raw and row-level data stay out of git, aggregates only. Downloads approved for Nasdaq ITCH samples. LOBSTER stays manual, because the owner must accept its terms personally.
- **Money:** cloud bare metal for this project is approved up to USD 50 (D-03). Everything else is USD 0.
- **Storage:** stay on `D:`, with free space checked at each milestone.
- **Benchmarks:** published runs happen when no multi-week capture is running on this PC.
- **D-05 ITCH day:** `01302019.NASDAQ_ITCH50.gz`, the primary day of itch-feed-handler. Changed from "most recent full-day sample".
- **D-02 and D-03 native Linux:** rented cloud bare metal, up to USD 50, for M6 only. Changed from "dual boot or spare box; no cloud spend". Nothing paid is created before M6, and the provider is chosen with the owner first.
- **Accepted defaults:** D-04, D-06 to D-09 stand as written in the spec.
- **D-02 and D-03 provider (owner decision, 2026-11-03):** Cherry Servers, hourly Ryzen bare metal, chosen over AWS metal. Target a Ryzen 9 7950X (Zen 4, $0.399/h) if one is in stock, otherwise a 9950X (Zen 5, $0.70/h), each with an IP-KVM console and Ubuntu 24.04 or 22.04. On 2026-11-03 stock was scarce: no 7950X anywhere and one 9950X in Lithuania. The cap stays USD 50 in total. The owner creates the account, pays and rents; agents never handle credentials or payment. `docs/benchmarking.md` has the session plan, the cost estimate and the rule to stop at 80% of the cap.
- **Zen 5 labelling:** the spec's reference examples are Raptor Lake and Zen 4. Every manifest carries a `uarch` field, and results from a 9950X are labelled Zen 5 wherever they appear.

## Environment (P-01, P-02, recorded 2026-11-02)

| Item | Value |
|---|---|
| OS | WSL2, Ubuntu 22.04.5 LTS (jammy), on Windows 11 Pro. `wsl -l -v`: `Ubuntu` and `docker-desktop`, both Running, both version 2 |
| CPU | Intel Core i9-14900K as seen by WSL2: 32 logical CPUs, 62 GiB RAM |
| GCC | 13.4.0 (`g++-13`) |
| Clang | 18.1.8, with clang-format-18 and clang-tidy-18 |
| CMake, Ninja | 4.4.4, 1.13.2 (from `~/.local/bin`) |
| Python | 3.12 through uv |
| Data | `~/data/low-latency-matching-engine`, with the repo's `data` a symlink to it |
| Build trees | `~/build/low-latency-matching-engine/<preset>` |

Local builds, fuzzing and campaigns on this PC run at no more than 8 threads under `nice`, so the live recorders keep their CPU.

## Pinned dependencies

| Library | Tag |
|---|---|
| CPM.cmake | 0.40.8 (vendored) |
| GoogleTest | v1.15.2 |
| Google Benchmark | v1.9.1 |
| HdrHistogram_c | 0.11.8 |
| xxHash | v0.8.3 |
| CLI11 | v2.4.2 |
| toml++ | v3.4.0 |
| nlohmann/json | v3.11.3 |
| tl::expected | v1.1.0 |

## Semantics choices (M2)

Rule 9 of §16: where the spec is ambiguous, take the simplest consistent option and record it. `docs/design.md` is the full statement; these are the choices it had to make.

- **D-07 market protection:** unbounded sweep, as the default says.
- **`POOL_FULL` applies only to GTC limit orders, and is checked before matching.** IOC, FOK and market orders never take a slot. A GTC that would have filled completely is still rejected on a full pool, because FR-08 makes this a validation with no state change.
- **Validation order:** type, side, `ord_type` and `tif`, qty, price, duplicate, pool. Bad `ord_type` or `tif` bytes use `INVALID_TYPE`, the closest reason code in §7(b).
- **`tif` on a market order** is range-checked but has no other effect ("MARKET implies IOC behaviour").
- **Modify order of checks:** unknown ID, then qty 0 (a cancel, so the price is not looked at), then the price band. An out-of-band modify gets `MODIFY_REJECT` with `PRICE_OUT_OF_BAND`.
- **A no-op modify** (same price and qty) answers `MODIFIED` with no `LEVEL_UPDATE`, because FR-10 ties that record to a change in the aggregate.
- **`LEVEL_UPDATE.qty` saturates at 2³²−1.** The field is u32 and aggregates are u64 (NFR-05). The `removed` flag says when a level is gone.
- **Unlisted output fields.** ACK carries the order qty and limit price, REJECT echoes the raw `side` byte, CANCELED carries the canceled qty. The table in `docs/design.md` is complete.
- **ID 0 is a valid order ID.** The spec does not reject it, so no structure may use it as a sentinel.
- **FR-31 self-trade prevention is not built yet.** It is a Could item scheduled for M6 (D-06). Until then `stp = "off"` is the only accepted value: the fast engine and the profile loader refuse anything else.

## Generator choices (M2)

- **A `ReferenceEngine` shadow book drives the generator.** Without it, cancels would often hit orders that had already filled, about a quarter of them at the balanced mix, and "offset from touch" would be a guess. It also keeps the book stationary. A defect in a fast engine cannot change the input, because the stream depends only on the reference.
- **`target_live` regulator.** The `[mix]` shares alone make the book grow without limit, because adds outnumber cancels and fills, and a 100M run would sit at `POOL_FULL`. Once the shadow book reaches `target_live`, a NEW becomes a cancel, so realised shares shift slightly in steady state.
- **Added profile keys, all optional:** `generator.target_live`, `generator.half_spread`, `generator.traders`, `price.alpha`, `price.max_offset`, `price.floor`, `price.ceil`, `size.min`, `size.max`, `mix.invalid`, the `[modify]` table and an `[engine]` override.
- **Poisson arrivals are expressed as a `[rates]` table** normalised to shares. Independent Poisson streams merge into a multinomial choice of the next type, and an `.mlog` stores order but no timestamps, so the two forms are equivalent.
- **Power-law exponents are rounded to 1/16** and evaluated with `sqrt` and products, because `pow` is not guaranteed identical across C libraries. `-ffp-contract=off` keeps FMA from changing a result under `-march=native`.
- **`--check-invariants` on the reference** does nothing; the checker belongs to the fast engines.

## Fast core choices (M3)

- **`--check-invariants` is incremental.** A complete FR-19 check is O(book + band), about 40 ms on `deep_book`, so doing it after each of 10M messages would take days. After every message the harness instead checks that the book is not crossed and the ID map size equals live orders, and walks every level named by that message's `LEVEL_UPDATE` records, which FR-10 makes the full set of changed levels. The complete sweep runs every 4,096 messages and at the end. A defect that skipped a `LEVEL_UPDATE` would be caught by the record comparison against the reference. Property tests and `fuzz_engine_diff` still run the complete check after every message, on small books.
- **`LevelStore` concept extended.** The spec's three members are kept, and `next_after`, `find` and `check_consistency` are added, because the FOK scan, `depth_at`, snapshots and the invariant checker need them.
- **The alloc tests do not run under sanitizers.** The hook replaces `malloc`, which ASan and TSan also claim. They run in `gcc-release`, `clang-release` and `coverage`.
- **Order size is 48 bytes**, including the `arrival` stamp that exists only for the invariant checker. Footprint at the default config is checked against NFR-12 by a test.

## Pipeline and determinism choices (M4)

- **Ring capacities are compile-time** (`kInRingCapacity`, `kOutRingCapacity`), matching `SpscRing<T, N>` in §10 and the `[ring]` values in `configs/engine.toml`.
- **The end-of-log sentinel is `InMsg::reserved`.** Every value of `type` is meaningful input, since it must produce `INVALID_TYPE`, so the sentinel cannot live there. The gateway zeroes `reserved` on each forwarded message, so a log cannot forge it.
- **`llme-run` writes `run.json` on request** (`--run-json <path>`). A fixed default path would dirty the working tree on every run.
- **Fixtures.** `tests/fixtures/synth_small_balanced.mlog` (10k messages, 320 kB) is committed. The other profiles are generated by the test from fixed seeds and pinned by input hash in `tests/fixtures/expected_hashes.toml`, because six 10k logs would be 1.92 MB against DQ-10's 1 MB.
- **The determinism matrix runs D2 and the reference for now.** D1 and D3 arrive with FR-26 in M6, and AC-W-01 and AC-W-05 name them "once FR-26 exists". The ITCH leg of AC-W-05 waits for M5.
- **`--pipeline` runs D2 only**, because FR-14 and AC-W-06 describe the D2 core loop.
- **Experiment rows ignore `results/` when judging `git_dirty`.** The first AC-W-08 row reads `true` because the run's own report was the only uncommitted file, in a clean clone at `e161f81`. It stays as written, per rule 5 of §16.

## M4 evidence (2026-11-02)

All of it comes from one commit, `ec3220f`, with binaries frozen before the runs started, at `nice` 15 and five threads at most on this WSL2 host.

| Criterion | Result | File |
|---|---|---|
| AC-W-01 | 180M operations over five profiles, including one continuous 100M run: 0 mismatches, final books equal, for D2 | `results/diff/campaign_2026-11-02.json` |
| AC-W-04 | 14,400 s on each of `fuzz_mlog_reader` and `fuzz_engine_diff` under ASan and UBSan: 0 crashes, leaks, timeouts or OOMs | `results/fuzz/` |
| AC-W-05 | 360 runs, Debug and Release, GCC and Clang, pipeline and in-process, D2 and reference: one hash per fixture | `results/determinism.csv` |
| AC-W-08 | 10M operations per profile with invariants on: 0 failures | `results/diff/invariants_2026-11-02.json` |
| AC-W-09 | 1B items in Release; 10M under TSan in CI | `results/diff/ring_stress_1b_2026-11-02.txt` |

- **What is not covered yet.** AC-W-01 and AC-W-05 name D1 and D3, which M6 builds. AC-W-05 also names one ITCH log, which needs M5, and AC-W-04 has two more targets due in M5. M6's exit criterion repeats the D1, D2 and D3 hash check, so M4 is read as "for everything that exists at M4". No threshold changed.
- **`fuzz_engine_diff` averaged 1,786 execs/s over 25.7M execs,** far below the smoke run's 18,939. The complete invariant check runs after every operation, and inputs grow as the corpus does. `fuzz_mlog_reader` averaged 347,722 execs/s over 5.0B.
- **Commits after `ec3220f` on this PR change no engine code.** They default `llme-diff --engines` to `d2,ref`, fix `git_dirty` in `fuzz.sh`, and add docs and evidence.
- **The two fuzz rows read `git_dirty=true`.** The clean clone's only uncommitted files were the reports those runs wrote. `fuzz.sh` now ignores `results/`, and the rows stay as written, per rule 5 of §16.
- **Review.** A second reader, working from the spec, compared the semantics with the reference and golden scenarios and found no contradiction. The closest calls are already recorded above: `POOL_FULL` scope, IOC with no fill, and the no-op modify.

## M5 evidence (2026-11-02 and 03)

Commit `a66ea44`, binaries frozen before the runs started, `nice` 15, and no more than 8 threads in total on this WSL2 host.

| Criterion | Result | File |
|---|---|---|
| AC-W-10 | 133 logs and 104.3M messages, from ITCH, `book_l3` and LOBSTER: 0 mismatches between D2 and the reference, with sidecars for every log | `results/diff/real_flow_2026-11-02.json` |
| D-04 | `book_l3` and the decoder give byte-identical records on all 50 shared symbols | same file, `itch_routes` |
| AC-W-04 | 14,400 s each on `fuzz_lobster_parser` (97.8M execs) and `fuzz_itch_decoder` (668.6M execs): 0 crashes, leaks, timeouts or OOMs. All four targets are now complete | `results/fuzz/` |
| AC-W-05 | 420 runs, now including the 1.69M-message AAPL ITCH log: one hash per fixture | `results/determinism.csv` |

- **Fuzz segments.** The two new targets ran four 1 h segments on one persistent corpus, so an interruption would have cost at most one segment. Each `results/fuzz/` report is the sum of its segments, and the experiment log has a row for each.
- **The first real-flow run failed.** It stopped with exit 1 in the Python converter, which assumed the wire encoding for `book_l3.side`. The fix is `a66ea44`, and the evidence above comes from a full rerun of the fixed commit from a clean clone.
- **`fuzz_itch_decoder` was changed before its long run.** Per-input allocation held it to 797 execs/s, and reusing the buffers raised that to about 65,000.
- **AC-W-05 still names D1 and D3,** which M6 builds.

## Real-flow choices (M5)

- **D-04 resolves to (a), `book_l3`, under its default,** and the fallback decoder exists too, because AC-W-04 needs `fuzz_itch_decoder`. The two routes share one mapping, and `scripts/replay_evidence.sh` compares their records byte for byte.
- **D-05 is served by the decoder.** The `book_l3` output holds itch-feed-handler's 50-symbol watchlist, and only 19 of those are in the true top 50 on 2019-01-30. `llme-itch-decode --top 50` ranks symbols from the raw file by order messages (A, F, E, C, X, D, U) inside the session. The most active is QQQ.
- **The ITCH file is reused** from `~/data/itch-feed-handler/raw/itch/`, with no new download.
- **DQ-05 is applied literally.** Events outside 09:30 to 16:00 ET are skipped, so orders resting before the open are unknown to the converter and their later events are dropped under DQ-04. This costs 0.6% of events on the top 50 (346,393 unknown-ID drops against 57.5M messages).
- **Non-printable executions reduce their order by `MODIFY` and are counted as skipped.** DQ-05 says crosses are not modelled, but leaving those shares on the book would make it wrong for the rest of the day.
- **The expected contra travels in the log.** A synthetic IOC's `order_id` is the resting order's ID with the top bit set, so `exec_contra_match_rate` needs no per-execution data in the sidecar. The spec has the sidecar record it; a sidecar of that size would be row-level data for no benefit.
- **`book_l3.side` is 1 buy, -1 sell,** unlike the wire format's 0 and 1, and `book_l3_to_mlog.py` maps between them.
- **Sizing.** Stub quotes up to $199,999.99 make the DQ-07 band wide: up to 22M levels for one symbol and 102M across the top 50, about 5 GB in total. That is above NFR-12's default engine, which is sized for 2^20 levels, and is what DQ-07 asks for.
- **Extra files outside §6:** `scripts/replay_evidence.sh`, `tools/py/llme_tools/replay_report.py`, `src/workload/replay.{hpp,cpp}`, `src/tools/sidecar.{hpp,cpp}`, `tests/fixtures/make_itch_synthetic.py`.
- **LOBSTER columns against the sample's ReadMe.** They match the spec, except that the ReadMe lists no type 6 and the samples contain none. The adapter still skips type 6, as the spec says.

## LOBSTER top-of-book agreement (owner ruling, 2026-11-03)

§11 sets a target of at least 99% agreement between the engine's top of book and LOBSTER's orderbook file after each message, and R-04 treats less as a warning sign. Continuous replay does not reach it, and no correct engine can on these samples.

**Ruling: the owner accepted the one-step check as the §11 validation on 2026-11-03.** On these samples it agrees on 100% of 1.07M messages, so the target is met. The continuous-replay figures stay in `results/` and in the sidecars, labelled as a limit of 10-level samples.

| Sample | Messages compared | Continuous replay, price and size | Continuous replay, price only | One-step check |
|---|---|---|---|---|
| AAPL 2012-06-21, 10 levels | 392,010 | 35.6% | 41.3% | 100% of 400,390 |
| MSFT 2012-06-21, 10 levels | 666,904 | 52.7% | 94.8% | 100% of 668,764 |

- **Cause.** The sample's ReadMe says the message file covers events "in the requested price range". An order that drifts out of the top 10 levels gets no further events, so its cancel never appears, and orders resting before 09:30 or arriving from outside the window are never seen. Applying each event directly to the named order, with no matching at all, drifts the same way: live orders grow to about 10,000 and the known-order book shows a better bid than the real one on 96% of AAPL messages. The contra match rates of 59% and 74% have the same cause.
- **One-step check.** To keep the check that R-04 wants, `llme-convert-lobster` also rebuilds the visible book from each orderbook row, applies the next event through D2, and compares the top of book with the row after it. This tests add, cancel, delete and execution semantics on every message, with no inherited error, and needs no order IDs. Every event fell inside the visible levels, which supports the cause above. `OneStepTest.AWrongOutcomeIsNoticed` shows the check can fail.
- **Ruling applied.** The README memo (M7) cites the one-step figure for this check and reports the continuous figures next to it, with this cause.

ITCH replay does not have this limitation, because the file is complete. Its contra match rate on the top 50 is 85.8% to 99.9%. The misses come mainly from orders that rested before the open, which are not in the replayed book.

## Level stores D1 and D3 (M6, 2026-11-03)

- **D1 is a `std::pmr::map` per side on a fixed block pool** (`FixedBlockResource`), not on `unsynchronized_pool_resource`. A standard pool resource refills from its upstream when it runs low, so it can still reach `malloc`. The fixed pool is one buffer of 64-byte blocks sized for the most levels a side can hold, min(band, `max_orders`) + 1, and a request it cannot serve throws, so a sizing mistake shows up instead of hiding in a heap call. `AllocTest.OtherDesignsAreAllocationFree` holds D1 and D3 to the D2 standard.
- **D3 reserves and touches its capacity in the constructor**, the same min(band, `max_orders`) + 1, so `insert` never reallocates and never faults a page in.
- **Engine change for stores that erase levels.** D2 keeps an empty level in its array, so the engine could read a level's quantity after `on_level_empty`. A map or vector store erases it at that point. `fill_head` now reads the remaining quantity first and reports an emptied level, so the match loop stops using the reference. D2's output is unchanged: the pinned hashes still match.
- **`EngineCounters::levels_emptied` and `IdMap::probe_stats()`** were added for the cancel-burst experiment (FR-28). The first is one increment when a level empties. The second walks the table, so it runs only between phases, never per message.

## Bare-metal preparation (M6, 2026-11-03)

- **Extra files outside §6:** `scripts/bare_metal_setup.sh` (toolchain set-up, which previews unless given `--apply`), `scripts/bare_metal_run.sh` (the run list with the budget guard) and `scripts/pmu_check.sh` (the counter gate). `scripts/bench_env.sh` is the spec's own file.
- **`bench_env.sh` changes nothing.** It checks the host, and `--plan` prints the D-08 commands with their undo for the owner to run (P-05, rule 5 of §16). It refuses to plan bench cores that share a physical core, and suggests cores that do not.
- **The PMU gate comes first.** `bare_metal_run.sh` runs `pmu_check.sh` before anything else and stops if cycles, instructions, cache-misses and branch-misses are not all non-zero. On this WSL2 host it correctly stops.
- **The budget guard counts from renting.** The cost so far is the hourly rate times the hours since the order completed, so set-up time and idle time count. A step that would take spending past 80% of the cap does not start.

## Build choices

- **`tl::expected`, not `std::expected`.** The code base compiles as C++20 and libstdc++ gates `std::expected` on C++23. The spec names this fallback.
- **`.gitattributes` forces LF.** This PC's Windows git has `core.autocrlf=true`, which would otherwise rewrite shell scripts and fixtures with CRLF on checkout.
- **`/data` added to `.gitignore`.** The existing `data/` rule matches directories only, and `data` here is a symlink.
- **Files arrive with their milestone.** M1 creates every directory of §6. Scripts, configs and docs that belong to later milestones are added by those milestones, so the tree never holds a stub that pretends to work.
- **Extra files outside §6:** `scripts/check_data_hygiene.sh` (shared by pre-commit and CI), `scripts/coverage.sh` (the NFR-08 gate), `.gitattributes`.
- **`.mlog` rules exempt `tests/fixtures/` and `fuzz/corpus/`.** NFR-10 bans `*.mlog` from git, while §11 commits fixtures and regressions in that format. `tests/fixtures/` is synthetic-only and capped at 1 MB by DQ-10, so the exemption keeps both rules.
- **clang-tidy scope.** The `clang-tidy` preset covers `include/`, `src/` and the tools. Tests, benches and fuzz targets are excluded because GoogleTest and Google Benchmark macros cannot satisfy `cppcoreguidelines-*`. Disabled checks and their reasons are in `.clang-tidy`.
