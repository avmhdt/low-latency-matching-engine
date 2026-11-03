#!/usr/bin/env bash
# Flame graph of one design on one log (FR-27, AC-H-06), plus the three functions with the most
# self samples, written next to the SVG as <name>.top.txt.
#   scripts/perf_flamegraph.sh --engine d2 --in <mlog> --out results/figures/flame_d2.svg
# Samples come only from the timed loop (perf record --control), with frame-pointer call graphs:
# the bench preset keeps frame pointers. FlameGraph is the pinned clone that
# bare_metal_setup.sh makes, or $FLAMEGRAPH_DIR.
set -euo pipefail

engine=d2
in=""
out=""
frequency=999
while [ "$#" -gt 0 ]; do
  case "$1" in
    --engine) engine=$2; shift 2 ;;
    --in) in=$2; shift 2 ;;
    --out) out=$2; shift 2 ;;
    --frequency) frequency=$2; shift 2 ;;
    *) echo "unknown option: $1" >&2; exit 2 ;;
  esac
done
[ -n "$in" ] && [ -n "$out" ] || { echo "usage: scripts/perf_flamegraph.sh --engine d2 --in <mlog> --out <svg>" >&2; exit 2; }

tools=${LLME_TOOLS_DIR:-$HOME/build/low-latency-matching-engine/bench/bench}
flamegraph=${FLAMEGRAPH_DIR:-$HOME/src/FlameGraph}
for f in "$tools/llme-bench-throughput" "$flamegraph/stackcollapse-perf.pl" "$flamegraph/flamegraph.pl"; do
  [ -e "$f" ] || { echo "missing $f" >&2; exit 1; }
done
if grep -qi microsoft /proc/version 2>/dev/null && [ -z "${LLME_RESULTS_DIR:-}" ]; then
  echo "WSL2 runs are smoke checks: set LLME_RESULTS_DIR outside results/ and --out with it" >&2
  exit 1
fi

# Hardware cycles where the PMU has them; otherwise the software clock, and the output says so.
event=cycles
if ! perf stat -x, -e cycles -- true 2>&1 >/dev/null | awk -F, '$3 == "cycles" && $1 ~ /^[0-9]+$/ { ok = 1 } END { exit !ok }'; then
  event=cpu-clock
fi

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
mkfifo "$work/ctl" "$work/ack"
perf record -q -e "$event" -F "$frequency" --call-graph fp -o "$work/perf.data" \
  --control "fifo:$work/ctl,$work/ack" -D -1 -- \
  "$tools/llme-bench-throughput" --in "$in" --engine "$engine" --mode inproc --runs 3 \
  --perf-control "$work/ctl,$work/ack" --out "$work/tp" >/dev/null
perf script -i "$work/perf.data" 2>/dev/null | "$flamegraph/stackcollapse-perf.pl" >"$work/collapsed"

mkdir -p "$(dirname "$out")"
title="llme $engine on $(basename "$in" .mlog), $event, $(grep -qi microsoft /proc/version 2>/dev/null && echo 'WSL2 smoke' || hostname)"
"$flamegraph/flamegraph.pl" --title "$title" "$work/collapsed" >"$out"

# Self samples by leaf frame, as shares of the total.
awk '{ n = $NF; $NF = ""; sub(/ $/, ""); k = split($0, frames, ";"); self[frames[k]] += n; total += n }
     END { for (f in self) printf "%.1f%%\t%s\n", 100 * self[f] / total, f }' "$work/collapsed" |
  sort -rn | head -3 >"${out%.svg}.top.txt"
echo "event: $event" >>"${out%.svg}.top.txt"
cat "${out%.svg}.top.txt"
