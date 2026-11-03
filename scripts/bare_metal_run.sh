#!/usr/bin/env bash
# The M6 run list on the rented host, in order, with a budget guard (D-03).
#   LLME_RATE_USD_PER_HOUR=0.399 LLME_RENTED_AT=2026-11-04T09:00:00Z scripts/bare_metal_run.sh [--dry-run]
#
# Before each step, cost so far is rate x hours since LLME_RENTED_AT. If that plus the step's
# estimate would pass 80% of LLME_BUDGET_USD (default 50), it stops, so the server can be deleted
# with margin to spare. Billing runs from the moment of renting, so set-up time counts.
# Steps leave a marker under .bare_metal/, so a rerun resumes where it stopped. A step whose tool
# is not built yet is skipped and listed. The PMU check comes first and stops everything if it
# fails. Everything ends up in one tarball for copying back.
set -uo pipefail

: "${LLME_RATE_USD_PER_HOUR:?set the hourly rate from the Cherry Servers order}"
: "${LLME_RENTED_AT:?set the UTC time the server was rented, e.g. 2026-11-04T09:00:00Z}"
budget=${LLME_BUDGET_USD:-50}
stop_fraction=0.8
dry_run=false
[ "${1:-}" = "--dry-run" ] && dry_run=true

repo=$(cd "$(dirname "$0")/.." && pwd)
cd "$repo" || exit 1
export PATH=$HOME/.local/bin:$PATH
export LLME_RESULTS_DIR=results
host=$(hostname)
tools=$HOME/build/low-latency-matching-engine/bench/src/tools
bench=$HOME/build/low-latency-matching-engine/bench/bench
marks=.bare_metal
mkdir -p "$marks" results/manifests results/perf
log=$marks/progress.log
rented=$(date -u -d "$LLME_RENTED_AT" +%s) || { echo "bad LLME_RENTED_AT" >&2; exit 2; }
itch=data/raw/itch/01302019.NASDAQ_ITCH50.gz
itch_sha=8c97b5b13bc451c012c2466fb7e258da134dab29aa47b67fe7b0088c78e870be
mlog=data/processed/mlog
skipped=()

spent() { awk -v r="$LLME_RATE_USD_PER_HOUR" -v s="$(($(date -u +%s) - rented))" 'BEGIN { printf "%.2f", r * s / 3600 }'; }

# step <id> <estimated minutes> <needed file or -> <command...>
step() {
  local id=$1 minutes=$2 needs=$3
  shift 3
  if [ -f "$marks/$id.done" ]; then
    echo "done  $id"
    return 0
  fi
  if [ "$needs" != - ] && [ ! -e "$needs" ]; then
    echo "skip  $id: $needs is not built yet"
    skipped+=("$id")
    return 0
  fi
  local now projected limit
  now=$(spent)
  projected=$(awk -v n="$now" -v r="$LLME_RATE_USD_PER_HOUR" -v m="$minutes" 'BEGIN { printf "%.2f", n + r * m / 60 }')
  limit=$(awk -v b="$budget" -v f="$stop_fraction" 'BEGIN { printf "%.2f", b * f }')
  if awk -v p="$projected" -v l="$limit" 'BEGIN { exit !(p > l) }'; then
    echo "STOP: $id would take spending to \$$projected, past 80% of \$$budget (\$$limit). Bundle and delete the server." | tee -a "$log"
    bundle
    exit 3
  fi
  echo "run   $id (~${minutes} min, spent so far \$$now)" | tee -a "$log"
  $dry_run && return 0
  local start
  start=$(date -u +%s)
  if ! bash -c "$*" >"$marks/$id.out" 2>&1; then
    echo "FAIL  $id; output in $marks/$id.out" | tee -a "$log"
    tail -20 "$marks/$id.out"
    bundle
    exit 1
  fi
  echo "ok    $id in $(($(date -u +%s) - start)) s" | tee -a "$log"
  touch "$marks/$id.done"
}

bundle() {
  $dry_run && return 0
  local tarball=$HOME/llme-results-$host-$(date -u +%Y%m%dT%H%M%SZ).tar.gz
  tar -czf "$tarball" results "$marks" 2>/dev/null
  echo "bundle: $tarball"
}

# 1. Gate: nothing runs without hardware counters, and the host state is on record.
step pmu_check 1 - "scripts/pmu_check.sh"
step manifest 1 - "scripts/hw_manifest.sh > results/manifests/$host.json && scripts/bench_env.sh > results/manifests/$host.bench_env.txt"

# 2. Inputs. Synthetic logs are generated here and must match the hashes pinned in the repo, which
#    makes this host one more cell of the determinism matrix (AC-W-05). The ITCH day comes straight
#    from Nasdaq (D-05), so no market data is copied to the server.
step determinism 5 "$tools/llme-gen" "ctest --test-dir \$HOME/build/low-latency-matching-engine/release -L determinism"
step synthetic 3 "$tools/llme-gen" "mkdir -p $mlog && for p in balanced cancel_heavy burst; do $tools/llme-gen --profile configs/workloads/\$p.toml --ops 10000000 --seed 42 --out $mlog/synth_\${p}_s42.mlog; done"
step itch_fetch 10 - "test -f $itch || scripts/fetch_itch_sample.sh 01302019.NASDAQ_ITCH50.gz; echo '$itch_sha  $itch' | sha256sum -c -"
step itch_decode 6 "$tools/llme-itch-decode" "$tools/llme-itch-decode --in $itch --top 50 --out-dir $mlog"

# 3. Measurements (M6). Each tool is skipped until it exists; see docs/benchmarking.md.
step gbench 10 "$bench/llme-gbench" "taskset -c \${LLME_BENCH_CORE:-2} $bench/llme-gbench --benchmark_repetitions=5 --benchmark_out=results/perf/gbench.json"
step latency 20 "$bench/llme-bench-latency" "for d in d2 d1 d3; do for w in synth_balanced_s42 itch_QQQ_2019-01-30; do $bench/llme-bench-latency --in $mlog/\$w.mlog --engine \$d --warmup 1000000 --runs 5 --core \${LLME_BENCH_CORE:-2} --out results/latency/\${d}_\$w/; done; done"
step latency_pipeline 15 "$bench/llme-bench-latency" "$bench/llme-bench-latency --in $mlog/synth_balanced_s42.mlog --engine d2 --pipeline --rate 1000000 --out results/latency/d2_pipeline/"
step throughput 30 "$bench/llme-bench-throughput" "for d in d2 d1 d3; do for m in inproc pipeline; do $bench/llme-bench-throughput --in $mlog/itch_QQQ_2019-01-30.mlog --engine \$d --mode \$m --runs 5 --out results/throughput/\${d}_\${m}_top1/; $bench/llme-bench-throughput --in \$(ls $mlog/itch_*.mlog | head -50 | paste -sd,) --engine \$d --mode \$m --runs 5 --out results/throughput/\${d}_\${m}_top50/; done; done"
step perf_stat 20 scripts/perf_stat.sh "for d in d1 d2 d3; do scripts/perf_stat.sh --engine \$d --in $mlog/itch_QQQ_2019-01-30.mlog --out results/perf/; done"
step flamegraph 10 scripts/perf_flamegraph.sh "scripts/perf_flamegraph.sh --engine d2 --in $mlog/itch_QQQ_2019-01-30.mlog --out results/figures/flame_d2.svg"
step heaptrack 20 scripts/heaptrack.sh "scripts/heaptrack.sh --engine d2 --in $mlog/synth_balanced_s42.mlog --out results/alloc/"
step burst 20 "$bench/llme-bench-burst" "$bench/llme-bench-burst --profile configs/workloads/burst.toml --burst 1000,10000,100000 --out results/burst/"

bundle
echo "spent so far: \$$(spent) of \$$budget"
if [ "${#skipped[@]}" -gt 0 ]; then
  echo "skipped, tool not built yet: ${skipped[*]}"
fi
echo "Copy the bundle back, then delete the server in the Cherry Servers portal to stop billing."
