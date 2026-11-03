#!/usr/bin/env bash
# Hardware counters per message for one design (FR-27, AC-H-03).
#   scripts/perf_stat.sh --engine d1|d2|d3 --in <mlog> [--out results/perf/] [--runs 1] [--events list]
# perf counts only the timed loop: llme-bench-throughput turns counting on and off through
# perf stat --control, so loading and engine construction stay out. Generic events are used where
# the PMU has them; LLC-load-misses falls back to the AMD event pmu_check.sh found, and the CSV
# names the event actually used. One row is appended to <out>/counters.csv.
set -euo pipefail

engine=d2
in=""
out=${LLME_RESULTS_DIR:-results}/perf
runs=1
events=${LLME_PERF_EVENTS:-}  # bare_metal_run.sh --rehearse sets software events here
while [ "$#" -gt 0 ]; do
  case "$1" in
    --engine) engine=$2; shift 2 ;;
    --in) in=$2; shift 2 ;;
    --out) out=$2; shift 2 ;;
    --runs) runs=$2; shift 2 ;;
    --events) events=$2; shift 2 ;;
    *) echo "unknown option: $1" >&2; exit 2 ;;
  esac
done
[ -n "$in" ] || { echo "usage: scripts/perf_stat.sh --engine d2 --in <mlog>" >&2; exit 2; }

tools=${LLME_TOOLS_DIR:-$HOME/build/low-latency-matching-engine/bench/bench}
throughput=$tools/llme-bench-throughput
[ -x "$throughput" ] || { echo "no $throughput: build the bench preset" >&2; exit 1; }

if grep -qi microsoft /proc/version 2>/dev/null && [ -z "${LLME_RESULTS_DIR:-}" ]; then
  echo "WSL2 runs are smoke checks: set LLME_RESULTS_DIR outside results/" >&2
  exit 1
fi

# perf may print an event with a modifier (cycles:u when perf_event_paranoid is 2), so the match
# ignores everything after a colon.
supported() {
  perf stat -x, -e "$1" -- true 2>&1 >/dev/null |
    awk -F, -v e="$1" '{ n = $3; sub(/:.*/, "", n) } n == e && $1 ~ /^[0-9]+$/ { ok = 1 } END { exit !ok }'
}

llc_event=LLC-load-misses
if [ -n "$events" ]; then
  llc_event="custom: $(echo "$events" | tr ',' ' ')"
else
  if ! supported "$llc_event"; then
    llc_event=none
    for candidate in ls_dmnd_fills_from_sys.mem_io_local ls_any_fills_from_sys.dram_io_all \
      l2_cache_req_stat.ic_dc_miss_in_l2; do
      if supported "$candidate"; then
        llc_event=$candidate
        break
      fi
    done
  fi
  events="cycles,instructions,L1-dcache-load-misses,branch-misses"
  [ "$llc_event" != none ] && events="$events,$llc_event"
fi

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
mkfifo "$work/ctl" "$work/ack"

perf stat -x, -o "$work/stat.csv" --control "fifo:$work/ctl,$work/ack" -D -1 -e "$events" -- \
  "$throughput" --in "$in" --engine "$engine" --mode inproc --runs "$runs" \
  --perf-control "$work/ctl,$work/ack" --out "$work/tp" >"$work/tp.json"

msgs=$(python3 -c "import json,sys; print(json.load(open(sys.argv[1]))['msgs'] * $runs)" "$work/tp.json")
count() {
  awk -F, -v e="$1" '{ n = $3; sub(/:.*/, "", n) } n == e && $1 ~ /^[0-9]+$/ { print $1; found = 1 } END { if (!found) print "" }' "$work/stat.csv"
}
cycles=$(count cycles)
instructions=$(count instructions)
l1d=$(count L1-dcache-load-misses)
branch=$(count branch-misses)
llc=$([ "$llc_event" = none ] && echo "" || count "$llc_event")

per() { [ -n "$1" ] && awk -v v="$1" -v n="$msgs" 'BEGIN { printf "%.4f", v / n }' || echo ""; }
ipc=$([ -n "$cycles" ] && [ -n "$instructions" ] && awk -v i="$instructions" -v c="$cycles" 'BEGIN { printf "%.3f", i / c }' || echo "")
workload=$(basename "$in" .mlog)
run_id="$(date -u +%Y%m%dT%H%M%SZ)_${engine}_${workload}"
environment=$(grep -qi microsoft /proc/version 2>/dev/null && echo "WSL2 smoke" || echo native)

mkdir -p "$out"
csv="$out/counters.csv"
if [ ! -f "$csv" ]; then
  echo "run_id,design,workload,msgs,cycles,instructions,ipc,l1d_load_misses,llc_load_misses,branch_misses,per_msg_cycles,per_msg_instructions,per_msg_l1d_load_misses,per_msg_llc_load_misses,per_msg_branch_misses,manifest_id,llc_event,environment" >"$csv"
fi
echo "$run_id,$engine,$workload,$msgs,$cycles,$instructions,$ipc,$l1d,$llc,$branch,$(per "$cycles"),$(per "$instructions"),$(per "$l1d"),$(per "$llc"),$(per "$branch"),$(hostname),$llc_event,$environment" >>"$csv"
cp "$work/stat.csv" "$out/${run_id}.perf.csv"
tail -1 "$csv"
