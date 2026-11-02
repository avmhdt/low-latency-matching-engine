#!/bin/sh
# Data access smoke test: llme-gen writes a 10k-message log that llme-run reads back.
set -eu
gen=$1
run=$2
dir=$3

log="$dir/cli_round_trip.mlog"
"$gen" --ops 10000 --seed 42 --out "$log"
[ "$(wc -c <"$log")" -eq $((64 + 10000 * 32)) ]
"$run" --in "$log" --json | grep -q '"msgs":10000'
"$gen" --help >/dev/null
"$run" --help >/dev/null
if "$run" --in "$dir/does_not_exist.mlog" 2>/dev/null; then
  echo "llme-run must fail on a missing file" >&2
  exit 1
fi
rm -f "$log"
