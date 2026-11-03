#!/usr/bin/env bash
# Benchmark host state (D-08, P-05).
#   scripts/bench_env.sh                    check, and print OK or WARN per item
#   scripts/bench_env.sh --plan             print the commands that would set D-08; run nothing
#   scripts/bench_env.sh --cores 2,3,4      bench, producer and consumer cores (default: .env)
# This script never changes system state. The owner reviews and runs the --plan commands (rule 5
# of spec section 16). Every change in the plan is listed with its undo.
set -uo pipefail

if [ -f .env ]; then
  set -a
  # shellcheck disable=SC1091
  . ./.env
  set +a
fi

mode=check
cores="${LLME_BENCH_CORE:-2},${LLME_PRODUCER_CORE:-3},${LLME_CONSUMER_CORE:-4}"
while [ "$#" -gt 0 ]; do
  case "$1" in
    --plan) mode=plan; shift ;;
    --cores) cores=$2; shift 2 ;;
    *) echo "unknown option: $1" >&2; exit 2 ;;
  esac
done

IFS=, read -r -a bench_cores <<<"$cores"
cpu_dir=/sys/devices/system/cpu

# Every other logical CPU on the same physical core.
siblings_of() {
  local list
  list=$(cat "$cpu_dir/cpu$1/topology/thread_siblings_list" 2>/dev/null || echo "$1")
  echo "$list" | tr ',' '\n' | while IFS=- read -r lo hi; do
    seq "$lo" "${hi:-$lo}"
  done | grep -vx "$1" || true
}

is_bench_core() {
  local c
  for c in "${bench_cores[@]}"; do
    [ "$c" = "$1" ] && return 0
  done
  return 1
}

# Siblings to take offline. A sibling that is itself a bench core means two bench threads share a
# physical core, which D-08 forbids, so it is reported rather than taken offline.
siblings=()
shared=()
for c in "${bench_cores[@]}"; do
  while read -r s; do
    [ -z "$s" ] && continue
    if is_bench_core "$s"; then
      shared+=("$c+$s")
    else
      siblings+=("$s")
    fi
  done < <(siblings_of "$c")
done

# First CPUs from 2 upward, one per physical core: a valid --cores value for this host.
suggest_cores() {
  local seen="" picked=() c core
  for c in $(seq 2 $(($(nproc --all) - 1))); do
    core="$(cat "$cpu_dir/cpu$c/topology/physical_package_id" 2>/dev/null):$(cat "$cpu_dir/cpu$c/topology/core_id" 2>/dev/null)"
    [[ " $seen " == *" $core "* ]] && continue
    seen="$seen $core"
    picked+=("$c")
    [ "${#picked[@]}" -eq 3 ] && break
  done
  (IFS=,; echo "${picked[*]}")
}
isolated=$(IFS=,; echo "${bench_cores[*]}")
online=$(cat "$cpu_dir/online")
total=$(nproc --all)
housekeeping=$(seq 0 $((total - 1)) | grep -vxF -f <(printf '%s\n' "${bench_cores[@]}" "${siblings[@]}") |
  paste -sd, -)

boost_files=()
for f in "$cpu_dir/cpufreq/boost" "$cpu_dir"/cpufreq/policy*/boost; do
  [ -e "$f" ] && boost_files+=("$f")
done

if [ "$mode" = plan ]; then
  if [ "${#shared[@]}" -gt 0 ]; then
    echo "# STOP: bench cores ${shared[*]} are SMT siblings on one physical core." >&2
    echo "# Pick cores on distinct physical cores, for example: scripts/bench_env.sh --plan --cores $(suggest_cores)" >&2
    exit 1
  fi
  cat <<EOF
# D-08 plan for $(hostname): bench core ${bench_cores[0]}, producer ${bench_cores[1]:-}, consumer ${bench_cores[2]:-}
# SMT siblings to take offline: ${siblings[*]:-none}
# Housekeeping CPUs, where IRQs go: $housekeeping
# Review each command before running it. Steps 1-2 need a reboot; steps 3-7 do not, and are lost
# on reboot.

# 1. Isolate the cores from the scheduler, timer ticks and RCU callbacks (kernel command line).
grep -q 'isolcpus=$isolated' /etc/default/grub || sudo sed -i 's/^GRUB_CMDLINE_LINUX_DEFAULT="\\(.*\\)"/GRUB_CMDLINE_LINUX_DEFAULT="\\1 isolcpus=$isolated nohz_full=$isolated rcu_nocbs=$isolated"/' /etc/default/grub
sudo update-grub
#    undo: remove those three words from GRUB_CMDLINE_LINUX_DEFAULT, then sudo update-grub

# 2. Reboot so that step 1 takes effect (watch it on the IP-KVM console), then check:
sudo reboot
#    afterwards: cat /proc/cmdline should list isolcpus=$isolated

# 3. Performance governor on every CPU.
sudo cpupower frequency-set -g performance
#    undo: sudo cpupower frequency-set -g powersave   (or schedutil, whatever cpupower -c 0 frequency-info showed)

# 4. Turbo or boost off.
EOF
  if [ "${#boost_files[@]}" -gt 0 ]; then
    for f in "${boost_files[@]}"; do
      echo "echo 0 | sudo tee $f"
    done
    echo "#    undo: echo 1 to the same files"
  else
    echo "# No boost control in sysfs on this kernel and driver. Check:"
    echo "cat $cpu_dir/cpufreq/policy0/scaling_driver $cpu_dir/amd_pstate/status 2>/dev/null"
    echo "# If boost cannot be turned off here, record it: hw_manifest.sh reports turbo=unknown."
  fi
  cat <<EOF

# 5. Counters for unprivileged perf, kernel symbols for flame graphs, no NMI watchdog on a counter.
sudo sysctl -w kernel.perf_event_paranoid=1 kernel.kptr_restrict=0 kernel.nmi_watchdog=0
#    undo: sudo sysctl -w kernel.perf_event_paranoid=$(cat /proc/sys/kernel/perf_event_paranoid 2>/dev/null) kernel.kptr_restrict=$(cat /proc/sys/kernel/kptr_restrict 2>/dev/null) kernel.nmi_watchdog=$(cat /proc/sys/kernel/nmi_watchdog 2>/dev/null)

# 6. SMT siblings of the bench cores offline, so nothing shares their physical cores.
EOF
  for s in "${siblings[@]}"; do
    echo "echo 0 | sudo tee $cpu_dir/cpu$s/online"
  done
  cat <<EOF
#    undo: echo 1 to the same files

# 7. Interrupts away from the bench cores.
sudo systemctl stop irqbalance
for irq in /proc/irq/[0-9]*; do echo $housekeeping | sudo tee \$irq/smp_affinity_list >/dev/null 2>&1; done
#    undo: sudo systemctl start irqbalance

# Then: scripts/bench_env.sh   (every line should read OK)
EOF
  exit 0
fi

ok=0
warn=0
report() {
  if [ "$1" = OK ]; then ok=$((ok + 1)); else warn=$((warn + 1)); fi
  printf '%-4s %-26s %s\n' "$1" "$2" "$3"
}

if grep -qi microsoft /proc/version 2>/dev/null; then
  report WARN "platform" "WSL2: no PMU, no isolation; numbers here are labelled WSL2 only"
else
  report OK "platform" "native Linux $(uname -r)"
fi

cmdline=$(cat /proc/cmdline)
for word in "isolcpus=$isolated" "nohz_full=$isolated" "rcu_nocbs=$isolated"; do
  if [[ " $cmdline " == *" $word "* ]]; then
    report OK "cmdline" "$word"
  else
    report WARN "cmdline" "missing $word"
  fi
done

for c in "${bench_cores[@]}"; do
  gov=$(cat "$cpu_dir/cpu$c/cpufreq/scaling_governor" 2>/dev/null || echo unknown)
  if [ "$gov" = performance ]; then
    report OK "governor cpu$c" "$gov"
  else
    report WARN "governor cpu$c" "$gov"
  fi
done

if [ "${#boost_files[@]}" -eq 0 ]; then
  report WARN "boost" "no sysfs control found; state unknown"
else
  for f in "${boost_files[@]}"; do
    if [ "$(cat "$f")" = 0 ]; then report OK "boost" "$f = 0"; else report WARN "boost" "$f = $(cat "$f")"; fi
  done
fi

for pair in "${shared[@]}"; do
  report WARN "bench cores" "$pair share a physical core; try --cores $(suggest_cores)"
done
for s in "${siblings[@]}"; do
  state=$(cat "$cpu_dir/cpu$s/online" 2>/dev/null || echo 1)
  if [ "$state" = 0 ]; then report OK "SMT sibling cpu$s" "offline"; else report WARN "SMT sibling cpu$s" "online"; fi
done
[ "${#siblings[@]}" -eq 0 ] && report OK "SMT siblings" "none (online: $online)"

paranoid=$(cat /proc/sys/kernel/perf_event_paranoid 2>/dev/null || echo unknown)
if [ "$paranoid" != unknown ] && [ "$paranoid" -le 1 ]; then
  report OK "perf_event_paranoid" "$paranoid"
else
  report WARN "perf_event_paranoid" "$paranoid (needs <= 1, or run perf with sudo)"
fi

watchdog=$(cat /proc/sys/kernel/nmi_watchdog 2>/dev/null || echo unknown)
if [ "$watchdog" = 0 ]; then report OK "nmi_watchdog" "0"; else report WARN "nmi_watchdog" "$watchdog"; fi

if systemctl is-active --quiet irqbalance 2>/dev/null; then
  report WARN "irqbalance" "running"
else
  report OK "irqbalance" "not running"
fi

echo "$ok OK, $warn WARN"
