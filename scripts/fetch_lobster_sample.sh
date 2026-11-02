#!/usr/bin/env bash
# LOBSTER samples are a manual owner download (P-08): the site's terms must be accepted by hand.
#   scripts/fetch_lobster_sample.sh --verify   check the unzipped files and list the samples
set -euo pipefail

dir="${LLME_LOBSTER_DIR:-data/raw/lobster}"

if [ "${1:-}" != "--verify" ]; then
  sed -n '2,3p' "$0" | sed 's/^# \{0,1\}//'
  exit 2
fi

shopt -s nullglob globstar
found=0
for f in "$dir"/**/*_message_*.csv; do
  columns=$(head -1 "$f" | awk -F, '{print NF}')
  if [ "$columns" -ne 6 ]; then
    echo "expected 6 columns, found $columns: $f" >&2
    exit 1
  fi
  echo "$f"
  found=$((found + 1))
done

if [ "$found" -eq 0 ]; then
  echo "no *_message_*.csv under $dir; see P-08 in the spec" >&2
  exit 1
fi
echo "$found message file(s) ok"
