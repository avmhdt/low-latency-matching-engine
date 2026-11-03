#!/usr/bin/env bash
# Allocation proof (FR-12, AC-H-04), the heaptrack half and the hook half.
#   scripts/heaptrack.sh --engine d2 --in <mlog> [--messages N] [--out results/alloc/]
# heaptrack runs the throughput tool twice, over N and over 2N messages. Everything before the timed
# loop (loading, engine construction, prefault) is the same in both, so if the counts of
# allocation calls match, the extra N messages allocated nothing. The hook report comes from
# AllocTest, which counts allocations while armed around 10M messages.
set -euo pipefail

engine=d2
in=""
messages=1000000
out=${LLME_RESULTS_DIR:-results}/alloc
while [ "$#" -gt 0 ]; do
  case "$1" in
    --engine) engine=$2; shift 2 ;;
    --in) in=$2; shift 2 ;;
    --messages) messages=$2; shift 2 ;;
    --out) out=$2; shift 2 ;;
    *) echo "unknown option: $1" >&2; exit 2 ;;
  esac
done
[ -n "$in" ] || { echo "usage: scripts/heaptrack.sh --engine d2 --in <mlog>" >&2; exit 2; }

build=${LLME_BUILD_ROOT:-$HOME/build/low-latency-matching-engine}
throughput=$build/bench/bench/llme-bench-throughput
alloc_tests=$build/release/tests/llme_alloc_tests
for f in "$throughput" "$alloc_tests"; do
  [ -x "$f" ] || { echo "missing $f: build the bench and release presets" >&2; exit 1; }
done
command -v heaptrack >/dev/null && command -v heaptrack_print >/dev/null ||
  { echo "heaptrack is not installed" >&2; exit 1; }
if grep -qi microsoft /proc/version 2>/dev/null && [ -z "${LLME_RESULTS_DIR:-}" ]; then
  echo "WSL2 runs are smoke checks: set LLME_RESULTS_DIR outside results/" >&2
  exit 1
fi

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
mkdir -p "$out"

calls() {
  heaptrack_print "$1" 2>/dev/null | sed -n 's/^calls to allocation functions: \([0-9]*\).*/\1/p'
}
run_with() {
  heaptrack -o "$work/ht_$1" "$throughput" --in "$in" --engine "$engine" --mode inproc --runs 1 \
    --limit "$1" --out "$work/tp_$1" >"$work/ht_$1.log" 2>&1
  local f
  for f in "$work/ht_$1.zst" "$work/ht_$1.gz"; do
    if [ -f "$f" ]; then
      echo "$f"
      return 0
    fi
  done
  echo "heaptrack wrote no data for $1 messages:" >&2
  tail -5 "$work/ht_$1.log" >&2
  return 1
}
short=$(run_with "$messages")
long=$(run_with $((messages * 2)))
short_calls=$(calls "$short")
long_calls=$(calls "$long")

environment=$(grep -qi microsoft /proc/version 2>/dev/null && echo "WSL2 smoke" || echo native)
{
  echo "heaptrack, design $engine, log $(basename "$in"), $environment, $(hostname), $(git rev-parse --short HEAD 2>/dev/null)"
  echo "allocation calls over $messages messages:        $short_calls"
  echo "allocation calls over $((messages * 2)) messages: $long_calls"
  if [ "$short_calls" = "$long_calls" ]; then
    echo "result: equal, so the extra $messages messages made no allocation"
  else
    echo "result: DIFFERENT, so the timed loop allocates"
  fi
  echo
  heaptrack_print "$long" 2>/dev/null | sed -n '/^total runtime/,$p' | head -20
} >"$out/heaptrack_summary.txt"

cat >"$out/timed_loop.md" <<EOF
The timed loop is the inner loop of llme-bench-throughput between its start and stop marks.
Everything before it, from reading the log to constructing and prefaulting the engine, is identical
in the $messages and $((messages * 2)) message runs. So any allocation made per message would
make the second count larger than the first.
EOF

"$alloc_tests" --gtest_filter='AllocTest.*' --gtest_output="json:$work/alloc.json" >/dev/null
python3 - "$work/alloc.json" "$out/hook_report.json" "$environment" <<'PY'
import json, sys
report = json.load(open(sys.argv[1]))
tests = [t for suite in report["testsuites"] for t in suite["testsuite"]]
json.dump({
    "environment": sys.argv[3],
    "armed_messages": 10_000_000,
    "tests": [{"name": t["name"], "status": t["result"], "failures": len(t.get("failures", []))} for t in tests],
    "all_passed": all(not t.get("failures") for t in tests),
}, open(sys.argv[2], "w"), indent=2)
PY
cat "$out/heaptrack_summary.txt" | head -4
[ "$short_calls" = "$long_calls" ]
