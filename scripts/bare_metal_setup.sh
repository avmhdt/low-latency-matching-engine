#!/usr/bin/env bash
# One-time setup of a rented bare-metal benchmark host, Ubuntu 24.04 or 22.04 (D-02).
#   scripts/bare_metal_setup.sh            print every command; change nothing
#   scripts/bare_metal_setup.sh --apply    run them; safe to repeat
# Installs the toolchain of P-02 plus perf and heaptrack, pins FlameGraph, puts the data root on
# local disk, and builds the release and bench presets. It does not touch kernel or CPU settings:
# those are scripts/bench_env.sh --plan, for the owner to apply (P-05).
set -euo pipefail

apply=false
[ "${1:-}" = "--apply" ] && apply=true

flamegraph_commit=41fee1f99f9276008b7cd112fca19dc3ea84ac32
data_root=${DATA_ROOT:-$HOME/data/low-latency-matching-engine}
repo_dir=$(cd "$(dirname "$0")/.." && pwd)

run() {
  echo "+ $*"
  if $apply; then
    bash -c "$*"
  fi
}

. /etc/os-release
case "${VERSION_ID:-}" in
  24.04 | 22.04) ;;
  *) echo "Ubuntu 24.04 or 22.04 expected, found ${PRETTY_NAME:-unknown}" >&2; exit 1 ;;
esac
[ "$(uname -m)" = x86_64 ] || { echo "x86-64 expected" >&2; exit 1; }
echo "# ${PRETTY_NAME}, kernel $(uname -r), $(nproc) CPUs, repo at $repo_dir"
$apply || echo "# Preview only. Run with --apply to execute."

packages="build-essential git curl ca-certificates zlib1g-dev ninja-build python3 heaptrack \
linux-tools-common linux-tools-generic linux-tools-$(uname -r)"

if [ "$VERSION_ID" = 24.04 ]; then
  # 24.04 ships GCC 13, Clang 18 and CMake 3.28 in its own archive.
  run "sudo apt-get update -qq"
  run "sudo apt-get install -y -qq $packages cmake g++-13 clang-18 lld-18 llvm-18 libclang-rt-18-dev"
else
  # 22.04 needs the toolchain PPA for GCC 13 and apt.llvm.org for Clang 18, as in P-02.
  run "sudo apt-get update -qq"
  run "sudo apt-get install -y -qq $packages software-properties-common gnupg lsb-release wget"
  run "sudo add-apt-repository -y ppa:ubuntu-toolchain-r/test"
  run "sudo apt-get install -y -qq g++-13"
  run "command -v clang++-18 >/dev/null || (wget -qO /tmp/llvm.sh https://apt.llvm.org/llvm.sh && sudo bash /tmp/llvm.sh 18)"
  run "sudo apt-get install -y -qq libclang-rt-18-dev"
fi

# uv supplies Python 3.12 for tools/py, and CMake on 22.04.
run "command -v uv >/dev/null || curl -LsSf https://astral.sh/uv/install.sh | sh"
if [ "$VERSION_ID" = 22.04 ]; then
  run "\$HOME/.local/bin/uv tool install cmake"
fi

run "test -d \$HOME/src/FlameGraph || git clone -q https://github.com/brendangregg/FlameGraph \$HOME/src/FlameGraph"
run "git -C \$HOME/src/FlameGraph checkout -q $flamegraph_commit"

run "mkdir -p $data_root/raw/itch $data_root/processed/mlog $data_root/fuzz"
run "test -e $repo_dir/data || ln -s $data_root $repo_dir/data"

run "cd $repo_dir && PATH=\$HOME/.local/bin:\$PATH cmake --preset release >/dev/null && cmake --build --preset release"
run "cd $repo_dir && PATH=\$HOME/.local/bin:\$PATH cmake --preset bench >/dev/null && cmake --build --preset bench"
run "cd $repo_dir && ctest --preset release -j \$(nproc) | tail -3"

cat <<EOF
# Done. Next, in order:
#   scripts/bench_env.sh --plan     review, then run its commands (owner, P-05)
#   scripts/pmu_check.sh            must print PASS, or stop here
#   scripts/bare_metal_run.sh       the run list, with the budget guard
EOF
