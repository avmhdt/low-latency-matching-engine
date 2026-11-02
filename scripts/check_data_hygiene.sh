#!/usr/bin/env bash
# NFR-10: fail on data files or anything over 500 kB. With arguments it checks those paths
# (pre-commit); without, every tracked file (CI). tests/fixtures and fuzz/corpus hold small
# synthetic files only, so .mlog files are allowed there; DQ-10 caps fixtures at 1 MB in total.
set -euo pipefail

max_bytes=$((500 * 1024))
fixture_limit=$((1024 * 1024))
corpus_limit=$((200 * 1024))
status=0

if [ "$#" -gt 0 ]; then
  files=("$@")
else
  mapfile -t files < <(git ls-files)
fi

for f in "${files[@]}"; do
  case "$f" in
    data | data/* | */data/*)
      echo "data file in git: $f" >&2
      status=1
      ;;
    tests/fixtures/* | fuzz/corpus/*) ;;
    *.mlog | *.olog | *.itch | *.gz | *.parquet | .env)
      echo "forbidden file type: $f" >&2
      status=1
      ;;
  esac
  if [ -f "$f" ] && [ "$(wc -c <"$f")" -gt "$max_bytes" ]; then
    echo "file over 500 kB: $f" >&2
    status=1
  fi
done

if [ "$#" -eq 0 ]; then
  if [ -d tests/fixtures ] && [ "$(du -sb tests/fixtures | cut -f1)" -gt "$fixture_limit" ]; then
    echo "tests/fixtures is over 1 MB" >&2
    status=1
  fi
  if [ -d fuzz/corpus ] && [ "$(du -sb fuzz/corpus | cut -f1)" -gt "$corpus_limit" ]; then
    echo "fuzz/corpus is over 200 kB" >&2
    status=1
  fi
fi

exit "$status"
