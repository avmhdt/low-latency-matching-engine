#!/bin/sh
# End-to-end CLI checks: llme-gen writes a log that llme-run and llme-diff read back, the same
# seed gives the same bytes, and every tool fails cleanly on bad input.
set -eu
gen=$1
run=$2
diff=$3
dir=$4
export LLME_RESULTS_DIR="$dir"

profile=configs/workloads/balanced.toml
log="$dir/cli_round_trip.mlog"
"$gen" --profile "$profile" --ops 10000 --seed 42 --out "$log" --json | grep -q '"ops":10000'
[ "$(wc -c <"$log")" -eq $((64 + 10000 * 32)) ]
"$run" --in "$log" --json | grep -q '"msgs":10000'

first=$("$run" --in "$log")
second=$("$run" --in "$log")
[ "$first" = "$second" ]

"$gen" --profile "$profile" --ops 10000 --seed 42 --out "$log.again" >/dev/null
cmp "$log" "$log.again"

"$run" --in "$log" --out "$log.olog" >/dev/null
[ "$(($(wc -c <"$log.olog") % 48))" -eq $((64 % 48)) ]

"$diff" --in "$log" --engines ref,ref | grep -q '"mismatches":0'
"$diff" --profile configs/workloads/adversarial.toml --ops 2000 --seed 1 --engines ref,ref |
  grep -q '"mismatches":0'
"$diff" --campaign tests/fixtures/mini_campaign.toml --out "$dir/mini_campaign.json" \
  --regression-dir "$dir" 2>/dev/null
grep -q '"mismatches": 0' "$dir/mini_campaign.json"
"$diff" --campaign tests/fixtures/mini_campaign.toml --out "$dir/mini_campaign.json" --resume \
  --regression-dir "$dir" 2>/dev/null

"$gen" --help >/dev/null
"$run" --help >/dev/null
"$diff" --help >/dev/null
if "$run" --in "$dir/does_not_exist.mlog" 2>/dev/null; then
  echo "llme-run must fail on a missing file" >&2
  exit 1
fi
if "$gen" --profile does_not_exist.toml --out "$log" 2>/dev/null; then
  echo "llme-gen must fail on a missing profile" >&2
  exit 1
fi
if "$diff" --in "$log" --engines nosuch,ref 2>/dev/null; then
  echo "llme-diff must fail on an unknown engine" >&2
  exit 1
fi
rm -f "$log" "$log.again" "$log.olog" "$dir/mini_campaign.json"
