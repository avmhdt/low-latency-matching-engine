#!/usr/bin/env bash
# Run one libFuzzer target and record its stats.
#   scripts/fuzz.sh <target> [--seconds N] [--jobs J]
# The corpus persists under $LLME_DATA_DIR/fuzz/<target>, so a repeat run resumes from it, and
# crash artifacts land next to it. Writes results/fuzz/<target>_<date>.json and appends a row to
# results/experiment_log.csv. Exits non-zero if the fuzzer found anything.
set -euo pipefail

if [ -f .env ]; then
  set -a
  # shellcheck disable=SC1091
  . ./.env
  set +a
fi

target=${1:?usage: scripts/fuzz.sh <target> [--seconds N] [--jobs J]}
shift
seconds=${LLME_FUZZ_SECONDS:-60}
jobs=1
while [ "$#" -gt 0 ]; do
  case "$1" in
    --seconds) seconds=$2; shift 2 ;;
    --jobs) jobs=$2; shift 2 ;;
    *) echo "unknown option: $1" >&2; exit 2 ;;
  esac
done

build_dir=${LLME_FUZZ_BUILD_DIR:-$HOME/build/low-latency-matching-engine/fuzz}
binary="$build_dir/fuzz/$target"
data_dir=${LLME_DATA_DIR:-data}
results_dir=${LLME_RESULTS_DIR:-results}
corpus="$data_dir/fuzz/$target"
artifacts="$data_dir/fuzz/artifacts/$target"
log="$data_dir/fuzz/$target.$(date -u +%Y%m%dT%H%M%SZ).log"
mkdir -p "$corpus" "$artifacts" "$results_dir/fuzz"

args=(-max_total_time="$seconds" -timeout=25 -rss_limit_mb=4096 -print_final_stats=1
      -artifact_prefix="$artifacts/")
dict="fuzz/corpus/$target.dict"
if [ -f "$dict" ]; then
  args+=(-dict="$dict")
fi
if [ "$jobs" -gt 1 ]; then
  args+=(-fork="$jobs" -ignore_crashes=0 -ignore_timeouts=0 -ignore_ooms=0)
fi

fuzz_status=0
"$binary" "${args[@]}" "$corpus" "fuzz/corpus/$target" >"$log" 2>&1 || fuzz_status=$?

execs=$(sed -n 's/^stat::number_of_executed_units: *//p' "$log" | tail -1)
rate=$(sed -n 's/^stat::average_exec_per_sec: *//p' "$log" | tail -1)
features=$(grep -oE ' ft: [0-9]+' "$log" | tail -1 | grep -oE '[0-9]+' || echo 0)
coverage=$(grep -oE ' cov: [0-9]+' "$log" | tail -1 | grep -oE '[0-9]+' || echo 0)
corpus_size=$(find "$corpus" -type f | wc -l)
crashes=$(find "$artifacts" -type f -name 'crash-*' | wc -l)
timeouts=$(find "$artifacts" -type f -name 'timeout-*' | wc -l)
ooms=$(find "$artifacts" -type f -name 'oom-*' | wc -l)
leaks=$(find "$artifacts" -type f -name 'leak-*' | wc -l)

date_tag=$(date -u +%Y-%m-%d)
out="$results_dir/fuzz/${target}_${date_tag}.json"
cat >"$out" <<EOF
{
  "target": "$target",
  "seconds": $seconds,
  "jobs": $jobs,
  "execs": ${execs:-0},
  "execs_per_sec": ${rate:-0},
  "corpus_size": $corpus_size,
  "coverage": { "cov": ${coverage:-0}, "features": ${features:-0} },
  "crashes": $crashes,
  "leaks": $leaks,
  "timeouts": $timeouts,
  "ooms": $ooms,
  "exit_status": $fuzz_status,
  "git_sha": "$(git rev-parse HEAD 2>/dev/null || echo unknown)"
}
EOF

csv="$results_dir/experiment_log.csv"
if [ -f "$csv" ]; then
  # Columns of spec 7(b): identity, then the headline metric of this run.
  printf '%s,%s,%s,%s,fuzz.sh,%s,,%s,,,%s,clang-18,,%s,,,,,,%s,crashes,%s,%s\n' \
    "fuzz_${target}_$(date -u +%Y%m%dT%H%M%SZ)" "$(date -u +%Y-%m-%dT%H:%M:%SZ)" \
    "$(git rev-parse HEAD 2>/dev/null || echo unknown)" \
    "$([ -n "$(git status --porcelain 2>/dev/null | head -1)" ] && echo true || echo false)" \
    "seconds=$seconds jobs=$jobs" "$target" "${execs:-0}" "$(hostname)" \
    "$(grep -qi microsoft /proc/version 2>/dev/null && echo true || echo false)" \
    "$((crashes + leaks + timeouts + ooms))" "$out" >>"$csv"
fi

cat "$out"
if [ "$fuzz_status" -ne 0 ] || [ $((crashes + leaks + timeouts + ooms)) -ne 0 ]; then
  echo "fuzzer found something; see $log and $artifacts" >&2
  exit 1
fi
