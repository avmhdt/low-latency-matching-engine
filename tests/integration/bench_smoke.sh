#!/bin/sh
# The benchmark tools run end to end on small inputs and write what they promise. Numbers here
# mean nothing; results go to the build tree, never to results/.
set -eu
latency=$1
throughput=$2
burst=$3
gen=$4
dir=$5/bench_smoke
rm -rf "$dir"
mkdir -p "$dir"
export LLME_RESULTS_DIR="$dir/results"

fail() {
  echo "$1" >&2
  exit 1
}

log=tests/fixtures/synth_small_balanced.mlog
for design in d1 d2 d3; do
  "$latency" --in "$log" --engine "$design" --warmup 1000 --runs 2 --out "$dir/lat_$design" \
    >"$dir/lat_$design.json"
  for op in add cancel match modify; do
    [ -s "$dir/lat_$design/$op.hgrm" ] || fail "no $op.hgrm for $design"
  done
  grep -q '"timer_overhead_p50_ns"' "$dir/lat_$design.json" || fail "no timer overhead"
done
[ "$(wc -l <"$LLME_RESULTS_DIR/latency/summary.csv")" -eq 13 ] || fail "latency CSV rows"

"$latency" --in "$log" --pipeline --rate 500000 --limit 5000 --out "$dir/lat_pipe" |
  grep -q '"mode":"pipeline"' || fail "pipeline latency"

"$gen" --profile configs/workloads/adversarial.toml --ops 5000 --seed 3 --out "$dir/adv.mlog" >/dev/null
for mode in inproc pipeline; do
  "$throughput" --in "$log,$dir/adv.mlog" --engine d2 --mode "$mode" --runs 2 --out "$dir/tp_$mode" |
    grep -q '"order":"concatenated"' || fail "throughput $mode"
done
"$throughput" --in "$log" --engine d3 --limit 2000 --runs 1 | grep -q '"msgs":2000' ||
  fail "throughput limit"

# A two-symbol interleave, written by hand in the decoder's format.
printf '\000\000\001\000\000\000' >"$dir/two.interleave"
"$gen" --profile configs/workloads/adversarial.toml --ops 2 --seed 5 --out "$dir/one.mlog" >/dev/null
"$gen" --profile configs/workloads/adversarial.toml --ops 1 --seed 4 --out "$dir/two.mlog" >/dev/null
cat >"$dir/two.interleave.json" <<'EOF'
{ "count": 3, "logs": ["two.mlog", "one.mlog"], "symbols": ["B", "A"] }
EOF
"$throughput" --interleave "$dir/two.interleave" --limit 3 --runs 1 | grep -q '"order":"interleaved"' ||
  fail "interleaved throughput"

"$burst" --profile configs/workloads/burst.toml --burst 100 --ops 20000 --every 5000 --out "$dir/burst" |
  grep -q '"phases"' || fail "burst"
grep -q ',inside,' "$LLME_RESULTS_DIR/burst/summary.csv" || fail "burst CSV"

if "$latency" --in "$log" --engine d9 --runs 1 2>/dev/null; then fail "unknown design accepted"; fi
if "$throughput" --runs 1 2>/dev/null; then fail "throughput without input accepted"; fi
if "$throughput" --in "$log" --mode sideways 2>/dev/null; then fail "bad mode accepted"; fi
rm -rf "$dir"
