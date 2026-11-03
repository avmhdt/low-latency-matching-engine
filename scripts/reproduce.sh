#!/usr/bin/env bash
# Regenerates README numbers from committed configs. M7 extends this to every result.
#   scripts/reproduce.sh --determinism   build Debug and Release with GCC and Clang, run every
#                                        fixture in-process and through the pipeline, and write
#                                        results/determinism.csv (AC-W-05)
set -euo pipefail

mode=${1:---determinism}
if [ "$mode" != "--determinism" ]; then
  echo "usage: scripts/reproduce.sh --determinism" >&2
  exit 2
fi

results_dir=${LLME_RESULTS_DIR:-results}
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
csv="$results_dir/determinism.csv"
echo "fixture,seed,ops,compiler,build,engine,mode,run,output_hash" >"$csv"

fixtures=$(sed -n 's/^\[\(.*\)\]$/\1/p' tests/fixtures/expected_hashes.toml)

for preset in gcc-release gcc-debug clang-release clang-debug; do
  cmake --preset "$preset" >/dev/null
  cmake --build --preset "$preset" --target llme-gen llme-run >/dev/null
  tools="$HOME/build/low-latency-matching-engine/$preset/src/tools"
  compiler=${preset%-*}
  build=${preset#*-}
  for fixture in $fixtures; do
    profile="configs/workloads/$fixture.toml"
    seed=$(sed -n "/^\[$fixture\]/,/^\[/s/^seed = //p" tests/fixtures/expected_hashes.toml)
    ops=$(sed -n "/^\[$fixture\]/,/^\[/s/^ops = //p" tests/fixtures/expected_hashes.toml)
    log="$work/$fixture.mlog"
    "$tools/llme-gen" --profile "$profile" --ops "$ops" --seed "$seed" --out "$log" >/dev/null
    for engine in d2 d1 d3 ref; do
      for run in 1 2 3 4 5; do
        hash=$("$tools/llme-run" --in "$log" --engine "$engine" --config "$profile")
        echo "$fixture,$seed,$ops,$compiler,$build,$engine,inproc,$run,$hash" >>"$csv"
      done
    done
    for run in 1 2 3 4 5; do
      hash=$("$tools/llme-run" --in "$log" --engine d2 --pipeline --config "$profile")
      echo "$fixture,$seed,$ops,$compiler,$build,d2,pipeline,$run,$hash" >>"$csv"
    done
  done

  # One real log (AC-W-05), when this machine has it. Row-level data stays out of git; only the
  # hash below reaches results/.
  log="${LLME_DATA_DIR:-data}/processed/mlog/itch_AAPL_2019-01-30.mlog"
  if [ -f "$log" ]; then
    ops=$(($(wc -c <"$log") / 32))
    for run in 1 2 3 4 5; do
      for engine in d2 d1 d3 ref; do
        hash=$("$tools/llme-run" --in "$log" --engine "$engine")
        echo "itch_AAPL,0,$ops,$compiler,$build,$engine,inproc,$run,$hash" >>"$csv"
      done
      hash=$("$tools/llme-run" --in "$log" --engine d2 --pipeline)
      echo "itch_AAPL,0,$ops,$compiler,$build,d2,pipeline,$run,$hash" >>"$csv"
    done
  fi
done

# Every row of a fixture must carry the same hash.
if ! awk -F, 'NR > 1 { if ($1 in h && h[$1] != $9) { print "hash differs for " $1; bad = 1 } h[$1] = $9 }
              END { exit bad }' "$csv"; then
  exit 1
fi
echo "$(($(wc -l <"$csv") - 1)) runs, one hash per fixture"
