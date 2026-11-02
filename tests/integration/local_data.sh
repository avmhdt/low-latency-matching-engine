#!/usr/bin/env bash
# AC-W-10 on real data; carries the local-data label, so it never runs in CI. Converts the LOBSTER
# samples and diffs them, then diffs every ITCH log already under data/processed/mlog.
set -euo pipefail
lobster=$1
run=$2
diff=$3
dir=$4

lobster_dir=${LLME_LOBSTER_DIR:-data/raw/lobster}
mlog_dir=${LLME_DATA_DIR:-data}/processed/mlog
export LLME_RESULTS_DIR="$dir"

shopt -s nullglob
checked=0
for messages in "$lobster_dir"/*_message_*.csv; do
  name=$(basename "$messages")
  symbol=${name%%_*}
  log="$mlog_dir/lobster_$symbol.mlog"
  "$lobster" --messages "$messages" --orderbook "${messages/_message_/_orderbook_}" \
    --symbol "$symbol" --out "$log" >/dev/null
  "$diff" --in "$log" --engines d2,ref | grep -q '"mismatches":0'
  [ "$("$run" --in "$log")" = "$("$run" --in "$log" --pipeline)" ]
  checked=$((checked + 1))
done
[ "$checked" -gt 0 ] || { echo "no LOBSTER samples under $lobster_dir" >&2; exit 1; }

for log in "$mlog_dir"/itch_*.mlog "$mlog_dir"/book_l3_*.mlog; do
  "$diff" --in "$log" --engines d2,ref | grep -q '"mismatches":0'
  checked=$((checked + 1))
done
echo "$checked logs agree"
