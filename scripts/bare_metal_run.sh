#!/usr/bin/env bash
# The M6 run list on the rented host, in order, with a budget guard (D-03).
#   LLME_RATE_USD_PER_HOUR=0.399 LLME_RENTED_AT=2026-11-04T09:00:00Z scripts/bare_metal_run.sh [--dry-run]
#   LLME_RESULTS_DIR=/tmp/r LLME_RATE_USD_PER_HOUR=0 LLME_RENTED_AT=... scripts/bare_metal_run.sh --rehearse
#
# Before each step, cost so far is rate x hours since LLME_RENTED_AT. If that plus the step's
# estimate would pass 80% of LLME_BUDGET_USD (default 50), it stops, so the server can be deleted
# with margin to spare. Billing runs from the moment of renting, so set-up time counts.
# Steps leave a marker, so a rerun resumes where it stopped. A step whose tool is not built is
# skipped and listed. The PMU check comes first and stops everything if it fails. Everything ends
# up in one tarball for copying back.
#
# --rehearse runs every step for real at small scale (one run, short logs), so that a broken command
# shows up before any machine is rented. It needs LLME_RESULTS_DIR outside results/, continues
# past a failed PMU check, reuses LLME_ITCH_FILE if set, and its numbers mean nothing.
set -uo pipefail

: "${LLME_RATE_USD_PER_HOUR:?set the hourly rate from the Cherry Servers order}"
: "${LLME_RENTED_AT:?set the UTC time the server was rented, e.g. 2026-11-04T09:00:00Z}"
budget=${LLME_BUDGET_USD:-50}
stop_fraction=0.8
mode=run
case "${1:-}" in
  --dry-run) mode=dry ;;
  --rehearse) mode=rehearse ;;
  "") ;;
  *) echo "unknown option: $1" >&2; exit 2 ;;
esac

repo=$(cd "$(dirname "$0")/.." && pwd)
cd "$repo" || exit 1
export PATH=$HOME/.local/bin:$PATH
if [ "$mode" = rehearse ]; then
  : "${LLME_RESULTS_DIR:?--rehearse needs LLME_RESULTS_DIR outside results/}"
  marks=$LLME_RESULTS_DIR/.bare_metal
  runs=1
  limit="--limit 300000"
  synth_ops=1000000
  burst_ops="--ops 400000 --every 100000"
  gbench_args="--benchmark_min_time=0.01s"
  warmup=100000
  itch=${LLME_ITCH_FILE:-data/raw/itch/01302019.NASDAQ_ITCH50.gz}
  mlog=$LLME_RESULTS_DIR/mlog
  export LLME_PERF_EVENTS=task-clock,context-switches
else
  export LLME_RESULTS_DIR=results
  marks=.bare_metal
  runs=5
  limit=""
  synth_ops=10000000
  burst_ops=""
  gbench_args="--benchmark_repetitions=5"
  warmup=1000000
  itch=data/raw/itch/01302019.NASDAQ_ITCH50.gz
  mlog=data/processed/mlog
fi
out=$LLME_RESULTS_DIR
# Cores from bench_env.sh --plan; the pipeline tools read these.
export LLME_BENCH_CORE=${LLME_BENCH_CORE:-2} LLME_PRODUCER_CORE=${LLME_PRODUCER_CORE:-3} LLME_CONSUMER_CORE=${LLME_CONSUMER_CORE:-4}
host=$(hostname)
tools=$HOME/build/low-latency-matching-engine/bench/src/tools
bench=$HOME/build/low-latency-matching-engine/bench/bench
mkdir -p "$marks" "$out/manifests" "$out/perf" "$mlog"
log=$marks/progress.log
rented=$(date -u -d "$LLME_RENTED_AT" +%s) || { echo "bad LLME_RENTED_AT" >&2; exit 2; }
itch_sha=8c97b5b13bc451c012c2466fb7e258da134dab29aa47b67fe7b0088c78e870be
qqq=$mlog/itch_QQQ_2019-01-30.mlog
synth=$mlog/synth_balanced_s42.mlog
top50=$mlog/itch_top_2019-01-30.interleave
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
  local now projected cap
  now=$(spent)
  projected=$(awk -v n="$now" -v r="$LLME_RATE_USD_PER_HOUR" -v m="$minutes" 'BEGIN { printf "%.2f", n + r * m / 60 }')
  cap=$(awk -v b="$budget" -v f="$stop_fraction" 'BEGIN { printf "%.2f", b * f }')
  if awk -v p="$projected" -v l="$cap" 'BEGIN { exit !(p > l) }'; then
    echo "STOP: $id would take spending to \$$projected, past 80% of \$$budget (\$$cap). Bundle and delete the server." | tee -a "$log"
    bundle
    exit 3
  fi
  echo "run   $id (~${minutes} min, spent so far \$$now)" | tee -a "$log"
  [ "$mode" = dry ] && return 0
  local start
  start=$(date -u +%s)
  if ! bash -c "$*" >"$marks/$id.out" 2>&1; then
    if [ "$mode" = rehearse ] && [ "$id" = pmu_check ]; then
      echo "warn  pmu_check failed, as it must without counters; rehearsal goes on" | tee -a "$log"
      return 0
    fi
    echo "FAIL  $id; output in $marks/$id.out" | tee -a "$log"
    tail -20 "$marks/$id.out"
    bundle
    exit 1
  fi
  echo "ok    $id in $(($(date -u +%s) - start)) s" | tee -a "$log"
  touch "$marks/$id.done"
}

bundle() {
  [ "$mode" = run ] || return 0
  local tarball=$HOME/llme-results-$host-$(date -u +%Y%m%dT%H%M%SZ).tar.gz
  tar -czf "$tarball" results "$marks" 2>/dev/null
  echo "bundle: $tarball"
}

# 1. Gate: nothing runs without hardware counters, and the host state is on record.
step pmu_check 1 - "scripts/pmu_check.sh --out $out/perf/pmu_check_$host.json"
step manifest 1 - "scripts/hw_manifest.sh > $out/manifests/$host.json && scripts/bench_env.sh > $out/manifests/$host.bench_env.txt"

# 2. Inputs. Synthetic logs are generated here and must match the hashes pinned in the repo, which
#    makes this host one more cell of the determinism matrix (AC-W-05). The ITCH day comes straight
#    from Nasdaq (D-05), so no market data is copied to the server.
step determinism 5 "$tools/llme-gen" "ctest --test-dir \$HOME/build/low-latency-matching-engine/release -L determinism"
step synthetic 3 "$tools/llme-gen" "for p in balanced cancel_heavy burst; do $tools/llme-gen --profile configs/workloads/\$p.toml --ops $synth_ops --seed 42 --out $mlog/synth_\${p}_s42.mlog; done"
step itch_fetch 10 - "test -f $itch || scripts/fetch_itch_sample.sh 01302019.NASDAQ_ITCH50.gz; echo '$itch_sha  $itch' | sha256sum -c -"
step itch_decode 6 "$tools/llme-itch-decode" "$tools/llme-itch-decode --in $itch --top 50 --out-dir $mlog"

# 3. Measurements (M6); see docs/benchmarking.md.
step gbench 10 "$bench/llme-gbench" "taskset -c \$LLME_BENCH_CORE $bench/llme-gbench $gbench_args --benchmark_out=$out/perf/gbench.json"
step latency 20 "$bench/llme-bench-latency" "for d in d2 d1 d3; do for w in $synth $qqq; do $bench/llme-bench-latency --in \$w --engine \$d --warmup $warmup --runs $runs $limit --core \$LLME_BENCH_CORE --out $out/latency/\${d}_\$(basename \$w .mlog)/; done; done"
step latency_pipeline 15 "$bench/llme-bench-latency" "$bench/llme-bench-latency --in $synth --engine d2 --pipeline --rate 1000000 $limit --core \$LLME_BENCH_CORE --out $out/latency/d2_pipeline/"
step throughput 30 "$bench/llme-bench-throughput" "for d in d2 d1 d3; do for m in inproc pipeline; do $bench/llme-bench-throughput --in $qqq --engine \$d --mode \$m --runs $runs $limit --core \$LLME_BENCH_CORE --out $out/throughput/\${d}_\${m}_top1/; $bench/llme-bench-throughput --interleave $top50 --engine \$d --mode \$m --runs $runs $limit --core \$LLME_BENCH_CORE --out $out/throughput/\${d}_\${m}_top50/; done; done"
step perf_stat 20 scripts/perf_stat.sh "for d in d1 d2 d3; do for w in $qqq $synth; do taskset -c \$LLME_BENCH_CORE scripts/perf_stat.sh --engine \$d --in \$w --out $out/perf/; done; done"
step flamegraph 10 scripts/perf_flamegraph.sh "taskset -c \$LLME_BENCH_CORE scripts/perf_flamegraph.sh --engine d2 --in $qqq --out $out/figures/flame_d2.svg"
step heaptrack 20 scripts/heaptrack.sh "scripts/heaptrack.sh --engine d2 --in $synth --out $out/alloc/"
step burst 20 "$bench/llme-bench-burst" "$bench/llme-bench-burst --profile configs/workloads/burst.toml --burst 1000,10000,100000 $burst_ops --core \$LLME_BENCH_CORE --out $out/burst/"

bundle
echo "spent so far: \$$(spent) of \$$budget"
if [ "${#skipped[@]}" -gt 0 ]; then
  echo "skipped, tool not built yet: ${skipped[*]}"
fi
if [ "$mode" = run ]; then
  echo "Copy the bundle back, then delete the server in the Cherry Servers portal to stop billing."
fi
