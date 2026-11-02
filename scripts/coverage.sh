#!/usr/bin/env bash
# Source-based coverage over the test suite; fails under the NFR-08 thresholds:
# >= 90% lines on src/core + include/llme, >= 80% on all C++ outside bench/ and fuzz/.
set -euo pipefail

build_dir=${1:-$HOME/build/low-latency-matching-engine/coverage}
profdata_dir="$build_dir/profiles"
rm -rf "$profdata_dir"
mkdir -p "$profdata_dir"

LLVM_PROFILE_FILE="$profdata_dir/%p-%m.profraw" \
  ctest --test-dir "$build_dir" --output-on-failure -LE "local-data" -j "${LLME_TEST_JOBS:-4}"

llvm-profdata-18 merge -sparse "$profdata_dir"/*.profraw -o "$build_dir/merged.profdata"

objects=()
while IFS= read -r binary; do
  objects+=(-object "$binary")
done < <(find "$build_dir" -type f -executable \
  \( -path '*/src/tools/llme-*' -o -path '*/tests/llme_*' \) ! -name '*.cmake')

report() {
  llvm-cov-18 report "${objects[@]}" -instr-profile="$build_dir/merged.profdata" "$@" |
    tee /dev/stderr | awk '/^TOTAL/ {gsub("%", "", $10); print $10}'
}

ignore='(_deps|\.cache|/tests/|/bench/|/fuzz/)'
overall=$(report -ignore-filename-regex="$ignore")
core=$(report -ignore-filename-regex="$ignore|/src/(io|reference|pipeline|workload|testing|tools)/")

if [ "${LLME_COVERAGE_REPORT:-}" != "" ]; then
  llvm-cov-18 show "${objects[@]}" -instr-profile="$build_dir/merged.profdata" \
    -ignore-filename-regex="$ignore" -format=html -output-dir="$LLME_COVERAGE_REPORT"
fi

echo "core lines: ${core}% (need 90), overall lines: ${overall}% (need 80)"
awk -v core="$core" -v overall="$overall" 'BEGIN { exit !(core >= 90 && overall >= 80) }'
