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

## Build choices

- **`tl::expected`, not `std::expected`.** The code base compiles as C++20 and libstdc++ gates `std::expected` on C++23. The spec names this fallback.
- **`.gitattributes` forces LF.** This PC's Windows git has `core.autocrlf=true`, which would otherwise rewrite shell scripts and fixtures with CRLF on checkout.
- **`/data` added to `.gitignore`.** The existing `data/` rule matches directories only, and `data` here is a symlink.
- **Files arrive with their milestone.** M1 creates every directory of §6. Scripts, configs and docs that belong to later milestones are added by those milestones, so the tree never holds a stub that pretends to work.
- **Extra files outside §6:** `scripts/check_data_hygiene.sh` (shared by pre-commit and CI), `scripts/coverage.sh` (the NFR-08 gate), `.gitattributes`.
- **`.mlog` rules exempt `tests/fixtures/` and `fuzz/corpus/`.** NFR-10 bans `*.mlog` from git, while §11 commits fixtures and regressions in that format. `tests/fixtures/` is synthetic-only and capped at 1 MB by DQ-10, so the exemption keeps both rules.
- **clang-tidy scope.** The `clang-tidy` preset covers `include/`, `src/` and the tools. Tests, benches and fuzz targets are excluded because GoogleTest and Google Benchmark macros cannot satisfy `cppcoreguidelines-*`. Disabled checks and their reasons are in `.clang-tidy`.
