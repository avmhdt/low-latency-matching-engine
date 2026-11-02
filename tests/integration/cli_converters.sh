#!/bin/sh
# The converters on hand-built files in the real formats: logs, sidecars, replay figures, and
# llme-run and llme-diff picking up the band and pool from the sidecar.
set -eu
lobster=$1
itch=$2
run=$3
diff=$4
dir=$5
export LLME_RESULTS_DIR="$dir"

out="$dir/converters"
rm -rf "$out"
mkdir -p "$out"

fail() {
  echo "$1" >&2
  exit 1
}

# LOBSTER: 11 lines, of which one is malformed, one has an unknown ID, one an inexact price.
"$lobster" --messages tests/fixtures/lobster_synthetic_message.csv \
  --orderbook tests/fixtures/lobster_synthetic_orderbook.csv \
  --symbol ZZZT --out "$out/lobster.mlog" --json >"$out/lobster.json"
grep -q '"malformed":1' "$out/lobster.json" || fail "malformed line not counted"
grep -q '"unknown_id":1' "$out/lobster.json" || fail "unknown ID not counted"
grep -q '"price":1' "$out/lobster.json" || fail "inexact price not counted"
grep -q '"hidden":1' "$out/lobster.json" || fail "hidden execution not counted"
grep -q '"halt":1' "$out/lobster.json" || fail "halt not counted"
grep -q '"exec_contra_match_rate":1.0' "$out/lobster.json" || fail "contra rate wrong"
grep -q '"full_agreement_rate":1.0' "$out/lobster.json" || fail "top of book disagrees"
[ -f "$out/lobster.meta.json" ] || fail "no sidecar"
"$lobster" --messages tests/fixtures/lobster_synthetic_message.csv --symbol ZZZT \
  --out "$out/plain.mlog" >/dev/null
"$diff" --in "$out/lobster.mlog" --engines d2,ref | grep -q '"mismatches":0' || fail "lobster diff"

# ITCH: symbols chosen by activity, and by name.
"$itch" --in tests/fixtures/itch_synthetic.bin --top 1 --out-dir "$out" --date 2019-01-30 --json \
  >"$out/itch.json"
grep -q '"symbol":"ZZZT"' "$out/itch.json" || fail "top symbol wrong"
[ -f "$out/itch_ZZZT_2019-01-30.mlog" ] || fail "no ITCH log"
[ ! -f "$out/itch_ZYYT_2019-01-30.mlog" ] || fail "second symbol should not be selected"
"$itch" --in tests/fixtures/itch_synthetic.bin --symbols ZYYT --out-dir "$out" --date 2019-01-30 \
  >/dev/null
[ -f "$out/itch_ZYYT_2019-01-30.mlog" ] || fail "named symbol missing"

meta="$out/itch_ZZZT_2019-01-30.meta.json"
grep -q '"session": 1' "$meta" || fail "pre-open add not skipped"
grep -q '"price": 1' "$meta" || fail "inexact price not dropped"
grep -q '"unknown_id": 2' "$meta" || fail "unknown IDs not dropped"
grep -q '"crossed_add": 1' "$meta" || fail "crossed add not counted"
grep -q '"exec_contra_match_rate": 1.0' "$meta" || fail "ITCH contra rate wrong"

log="$out/itch_ZZZT_2019-01-30.mlog"
"$diff" --in "$log" --engines d2,ref | grep -q '"mismatches":0' || fail "ITCH diff"
first=$("$run" --in "$log")
[ "$first" = "$("$run" --in "$log" --pipeline)" ] || fail "pipeline hash differs"
[ "$first" = "$("$run" --in "$log" --engine ref)" ] || fail "reference hash differs"
"$run" --in "$log" --replay-stats | grep -q '"crossed_add":1' || fail "replay stats"

# Failure paths.
if "$itch" --in tests/fixtures/itch_synthetic.bin --out-dir "$out" 2>/dev/null; then
  fail "llme-itch-decode must refuse an empty selection"
fi
if "$itch" --in tests/fixtures/itch_synthetic.bin --symbols NOPE --out-dir "$out" \
  --date 2019-01-30 2>/dev/null; then
  fail "llme-itch-decode must refuse an unknown symbol"
fi
if "$itch" --in tests/fixtures/ab --top 1 --out-dir "$out" 2>/dev/null; then
  fail "llme-itch-decode must refuse a missing file"
fi
if "$lobster" --messages "$out/missing.csv" --symbol X --out "$out/x.mlog" 2>/dev/null; then
  fail "llme-convert-lobster must refuse a missing file"
fi
if "$lobster" --messages tests/fixtures/lobster_synthetic_message.csv --symbol X \
  --orderbook "$out/missing.csv" --out "$out/x.mlog" 2>/dev/null; then
  fail "llme-convert-lobster must refuse a missing orderbook"
fi
rm -rf "$out"
