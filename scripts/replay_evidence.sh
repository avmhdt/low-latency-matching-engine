#!/usr/bin/env bash
# AC-W-10 evidence on real flow; takes a few minutes. Converts the LOBSTER samples and the D-05
# ITCH symbols, converts every overlapping book_l3 symbol as a cross-check of D-04, diffs D2
# against the reference on every log, and writes aggregates only to results/diff/.
#   scripts/replay_evidence.sh [--skip-decode]
set -euo pipefail

if [ -f .env ]; then
  set -a
  # shellcheck disable=SC1091
  . ./.env
  set +a
fi

tools=${LLME_TOOLS_DIR:-$HOME/build/low-latency-matching-engine/release/src/tools}
data=${LLME_DATA_DIR:-data}
results=${LLME_RESULTS_DIR:-results}
itch_file=${LLME_ITCH_FILE:-$HOME/data/itch-feed-handler/raw/itch/01302019.NASDAQ_ITCH50.gz}
book_l3=${LLME_BOOK_L3_DIR:-$HOME/data/itch-feed-handler/book_l3}
lobster=${LLME_LOBSTER_DIR:-$data/raw/lobster}
date=2019-01-30
mlog="$data/processed/mlog"
diffs="$data/processed/diff"
stamp=$(date -u +%Y-%m-%d)
mkdir -p "$mlog" "$diffs"
rm -f "$diffs"/*.json

for messages in "$lobster"/*_message_*.csv; do
  name=$(basename "$messages")
  symbol=${name%%_*}
  "$tools/llme-convert-lobster" --messages "$messages" \
    --orderbook "${messages/_message_/_orderbook_}" --symbol "$symbol" \
    --out "$mlog/lobster_$symbol.mlog"
done

# One decode covers the D-05 top 50 and every book_l3 symbol, because each decode reads the
# whole day twice.
if [ "${1:-}" != "--skip-decode" ]; then
  watchlist=$(find "$book_l3"/venue=XNAS -mindepth 2 -maxdepth 2 -type d -name "date=$date" |
    sed 's|.*/symbol=\([^/]*\)/.*|\1|' | sort | paste -sd, -)
  "$tools/llme-itch-decode" --in "$itch_file" --top 50 --symbols "$watchlist" --out-dir "$mlog" \
    --date "$date"
fi

# D-04: both routes must give the same records. The 64-byte headers differ only in `source`.
agree=0
differ=0
missing=0
for partition in "$book_l3"/venue=XNAS/symbol=*/date="$date"; do
  symbol=$(basename "$(dirname "$partition")")
  symbol=${symbol#symbol=}
  log="$mlog/book_l3_${symbol}_$date.mlog"
  uv run --quiet --project tools/py llme-convert-book-l3 --in "$book_l3" --symbol "$symbol" \
    --date "$date" --out "$log" --llme-run "$tools/llme-run" >/dev/null
  decoded="$mlog/itch_${symbol}_$date.mlog"
  if [ ! -f "$decoded" ]; then
    missing=$((missing + 1))
  elif cmp -s -i 64 "$log" "$decoded"; then
    agree=$((agree + 1))
  else
    differ=$((differ + 1))
    echo "records differ: $symbol" >&2
  fi
done

for log in "$mlog"/itch_*.mlog "$mlog"/book_l3_*.mlog "$mlog"/lobster_*.mlog; do
  name=$(basename "$log" .mlog)
  "$tools/llme-diff" --in "$log" --engines d2,ref --shrink --regression-dir "$data/regressions" \
    --out "$diffs/$name.json" >/dev/null
done

uv run --quiet --project tools/py llme-replay-report --mlog-dir "$mlog" --diff-dir "$diffs" \
  --routes-agree "$agree" --routes-differ "$differ" --routes-missing "$missing" \
  --out "$results/diff/real_flow_$stamp.json"
