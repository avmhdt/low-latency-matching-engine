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
- **Still open:** bare-metal provider, researched first, needed by M6.

## Environment (P-01, P-02, recorded 2026-11-02)

| Item | Value |
|---|---|
| OS | WSL2, Ubuntu 22.04.5 LTS, on Windows 11 Pro |
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

- **Ring capacities are compile-time** (`kInRingCapacity`, `kOutRingCapacity`), matching `SpscRing<T, N>` in �10 and the `[ring]` values in `configs/engine.toml`.
- **The end-of-log sentinel is `InMsg::reserved`.** Every value of `type` is meaningful input, since it must produce `INVALID_TYPE`, so the sentinel cannot live there. The gateway zeroes `reserved` on each forwarded message, so a log cannot forge it.
- **`llme-run` writes `run.json` on request** (`--run-json <path>`). A fixed default path would dirty the working tree on every run.
- **Fixtures.** `tests/fixtures/synth_small_balanced.mlog` (10k messages, 320 kB) is committed. The other profiles are generated by the test from fixed seeds and pinned by input hash in `tests/fixtures/expected_hashes.toml`, because six 10k logs would be 1.92 MB against DQ-10's 1 MB.
- **The determinism matrix runs D2 and the reference for now.** D1 and D3 arrive with FR-26 in M6, and AC-W-01 and AC-W-05 name them "once FR-26 exists". The ITCH leg of AC-W-05 waits for M5.
- **`--pipeline` runs D2 only**, because FR-14 and AC-W-06 describe the D2 core loop.
- **Experiment rows ignore `results/` when judging `git_dirty`.** The first AC-W-08 row reads `true` because the run's own report was the only uncommitted file, in a clean clone at `e161f81`. It stays as written, per rule 5 of �16.

## Build choices

- **`tl::expected`, not `std::expected`.** The code base compiles as C++20 and libstdc++ gates `std::expected` on C++23. The spec names this fallback.
- **`.gitattributes` forces LF.** This PC's Windows git has `core.autocrlf=true`, which would otherwise rewrite shell scripts and fixtures with CRLF on checkout.
- **`/data` added to `.gitignore`.** The existing `data/` rule matches directories only, and `data` here is a symlink.
- **Files arrive with their milestone.** M1 creates every directory of §6. Scripts, configs and docs that belong to later milestones are added by those milestones, so the tree never holds a stub that pretends to work.
- **Extra files outside §6:** `scripts/check_data_hygiene.sh` (shared by pre-commit and CI), `scripts/coverage.sh` (the NFR-08 gate), `.gitattributes`.
- **`.mlog` rules exempt `tests/fixtures/` and `fuzz/corpus/`.** NFR-10 bans `*.mlog` from git, while §11 commits fixtures and regressions in that format. `tests/fixtures/` is synthetic-only and capped at 1 MB by DQ-10, so the exemption keeps both rules.
- **clang-tidy scope.** The `clang-tidy` preset covers `include/`, `src/` and the tools. Tests, benches and fuzz targets are excluded because GoogleTest and Google Benchmark macros cannot satisfy `cppcoreguidelines-*`. Disabled checks and their reasons are in `.clang-tidy`.
