#!/usr/bin/env bash
# First thing on a benchmark host: are hardware counters really there? Counts cycles,
# instructions, cache-misses and branch-misses over a small workload and stops (exit 1) unless all
# four are non-zero numbers. Also reports which of the counters FR-27 needs work, and the AMD
# events that stand in for LLC-load-misses, which perf's generic event often lacks on Zen.
#   scripts/pmu_check.sh [--out results/perf/pmu_check_<host>.json]
set -uo pipefail

out="${LLME_RESULTS_DIR:-results}/perf/pmu_check_$(hostname).json"
if [ "${1:-}" = "--out" ]; then
  out=$2
fi
mkdir -p "$(dirname "$out")"

paranoid=$(cat /proc/sys/kernel/perf_event_paranoid 2>/dev/null || echo unknown)
if ! command -v perf >/dev/null; then
  echo "STOP: perf is not installed (scripts/bare_metal_setup.sh installs it)" >&2
  exit 1
fi

# A workload that branches and misses cache, so that every counter has something to count.
workload() {
  awk 'BEGIN { srand(1); for (i = 0; i < 3000000; i++) { a[int(rand() * 1000000)]++ } }'
}
export -f workload

count() {
  perf stat -x, -e "$1" -- bash -c workload 2>&1 >/dev/null | awk -F, -v event="$1" '
    $3 == event || index($3, event) == 1 { print $1; found = 1; exit }
    END { if (!found) print "<missing>" }'
}

required=(cycles instructions cache-misses branch-misses)
spec=(L1-dcache-load-misses LLC-load-misses)
amd=(ls_dmnd_fills_from_sys.mem_io_local ls_any_fills_from_sys.dram_io_all l2_cache_req_stat.ic_dc_miss_in_l2)

status=0
json_items=()
echo "perf_event_paranoid = $paranoid"
for event in "${required[@]}"; do
  value=$(count "$event")
  if [[ "$value" =~ ^[0-9]+$ ]] && [ "$value" -gt 0 ]; then
    echo "OK    $event = $value"
  else
    echo "FAIL  $event = $value"
    status=1
  fi
  json_items+=("\"$event\": \"$value\"")
done
for event in "${spec[@]}" "${amd[@]}"; do
  value=$(count "$event")
  if [[ "$value" =~ ^[0-9]+$ ]]; then
    echo "info  $event = $value"
  else
    echo "info  $event unavailable ($value)"
  fi
  json_items+=("\"$event\": \"$value\"")
done

{
  echo "{"
  echo "  \"host\": \"$(hostname)\","
  echo "  \"timestamp_utc\": \"$(date -u +%Y-%m-%dT%H:%M:%SZ)\","
  echo "  \"perf_event_paranoid\": \"$paranoid\","
  echo "  \"pass\": $([ "$status" -eq 0 ] && echo true || echo false),"
  echo "  \"counts\": {"
  for i in "${!json_items[@]}"; do
    sep=,
    [ "$i" -eq $((${#json_items[@]} - 1)) ] && sep=
    echo "    ${json_items[$i]}$sep"
  done
  echo "  }"
  echo "}"
} >"$out"

if [ "$status" -ne 0 ]; then
  echo "STOP: hardware counters are not available here." >&2
  if [ "$paranoid" != unknown ] && [ "$paranoid" -gt 1 ] && [ "$(id -u)" -ne 0 ]; then
    echo "perf_event_paranoid is $paranoid: apply scripts/bench_env.sh --plan step 5, or run with sudo." >&2
  fi
  echo "Do not run benchmarks on this host. Report written to $out" >&2
  exit 1
fi
echo "PASS: hardware counters work. Report written to $out"
