#!/usr/bin/env bash
# Hardware and build manifest attached to every published result (FR-27). JSON on stdout:
#   scripts/hw_manifest.sh > results/manifests/<host>.json
set -euo pipefail

json_escape() { sed -e 's/\\/\\\\/g' -e 's/"/\\"/g' -e 's/\t/ /g' | tr -d '\n'; }
field() { printf '  "%s": "%s",\n' "$1" "$(printf '%s' "$2" | json_escape)"; }
read_or() { if [ -r "$1" ]; then cat "$1"; else echo "$2"; fi; }
lscpu_field() { lscpu | sed -n "s/^$1:[[:space:]]*//p" | head -1; }

wsl2=false
if grep -qi microsoft /proc/version 2>/dev/null; then
  wsl2=true
fi

turbo=unknown
if [ -r /sys/devices/system/cpu/intel_pstate/no_turbo ]; then
  if [ "$(cat /sys/devices/system/cpu/intel_pstate/no_turbo)" = 0 ]; then turbo=on; else turbo=off; fi
elif [ -r /sys/devices/system/cpu/cpufreq/boost ]; then
  if [ "$(cat /sys/devices/system/cpu/cpufreq/boost)" = 1 ]; then turbo=on; else turbo=off; fi
fi

# Microarchitecture label, so that a Zen 5 result is never mistaken for the spec's Zen 4 or Raptor
# Lake reference examples.
family=$(grep -m1 '^cpu family' /proc/cpuinfo 2>/dev/null | cut -d: -f2 | tr -d ' ' || echo 0)
model=$(grep -m1 '^model[[:space:]]*:' /proc/cpuinfo 2>/dev/null | cut -d: -f2 | tr -d ' ' || echo 0)
vendor=$(grep -m1 '^vendor_id' /proc/cpuinfo 2>/dev/null | cut -d: -f2 | tr -d ' ' || echo unknown)
uarch=unknown
if [ "$vendor" = AuthenticAMD ]; then
  if [ "$family" = 26 ]; then
    uarch="Zen 5"
  elif [ "$family" = 25 ] && [ "$model" -ge 96 ] && [ "$model" -le 127 ]; then
    uarch="Zen 4"
  elif [ "$family" = 25 ]; then
    uarch="Zen 3 or Zen 4 (family 25, model $model)"
  fi
elif [ "$vendor" = GenuineIntel ] && [ "$family" = 6 ]; then
  case "$model" in
    183 | 191) uarch="Raptor Lake" ;;
    151 | 154) uarch="Alder Lake" ;;
    *) uarch="Intel family 6 model $model" ;;
  esac
fi

git_sha=$(git rev-parse HEAD 2>/dev/null || echo unknown)
git_dirty=false
if [ -n "$(git status --porcelain 2>/dev/null | head -1)" ]; then
  git_dirty=true
fi

echo "{"
field host "$(hostname)"
field timestamp_utc "$(date -u +%Y-%m-%dT%H:%M:%SZ)"
field cpu_model "$(lscpu_field 'Model name')"
field uarch "$uarch"
field microcode "$(grep -m1 microcode /proc/cpuinfo 2>/dev/null | cut -d: -f2 | tr -d ' ' || true)"
field cpus "$(nproc --all)"
field threads_per_core "$(lscpu_field 'Thread(s) per core')"
field l1d "$(lscpu_field 'L1d cache')"
field l2 "$(lscpu_field 'L2 cache')"
field l3 "$(lscpu_field 'L3 cache')"
field tsc_flags "$(grep -m1 '^flags' /proc/cpuinfo 2>/dev/null | tr ' ' '\n' |
  grep -E '^(constant_tsc|nonstop_tsc|rdtscp)$' | tr '\n' ' ' || true)"
field kernel "$(uname -r)"
field cmdline "$(read_or /proc/cmdline unknown)"
field governor "$(read_or /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor unknown)"
field turbo "$turbo"
field smt "$(read_or /sys/devices/system/cpu/smt/active unknown)"
field thp "$(read_or /sys/kernel/mm/transparent_hugepage/enabled unknown)"
field gcc "$(g++-13 --version 2>/dev/null | head -1 || echo none)"
field clang "$(clang++-18 --version 2>/dev/null | head -1 || echo none)"
field bench_flags "-O3 -DNDEBUG -march=native -fno-omit-frame-pointer"
field git_sha "$git_sha"
printf '  "git_dirty": %s,\n' "$git_dirty"
printf '  "wsl2": %s\n' "$wsl2"
echo "}"
