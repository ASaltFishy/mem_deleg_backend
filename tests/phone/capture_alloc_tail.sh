#!/system/bin/sh
set -eu
dir=/data/local/tmp/md-slow-tail
name=${1:-md_alloc_tail_$(date +%Y%m%d_%H%M%S)}
probes=${2:-0}
policy=/sys/devices/system/cpu/cpu0/cpufreq
old_min=; old_max=; old_governor=
registered=
case "$name" in *[!a-zA-Z0-9_]*) exit 2;; esac
out=$dir/$name
instance=/sys/kernel/tracing/instances/$name
mkdir "$out"
mkdir "$instance"
cleanup() {
  set +e
  echo 0 > "$instance/tracing_on"
  if [ -n "$old_min" ]; then
    cat "$policy/scaling_min_freq" "$policy/scaling_max_freq" > "$out/frequency-end.txt"
    echo "$old_min" > "$policy/scaling_min_freq"
    echo "$old_max" > "$policy/scaling_max_freq"
    echo "$old_governor" > "$policy/scaling_governor"
  fi
  cat "$instance/trace" > "$out/trace.txt"
  cat "$instance/per_cpu/cpu0/stats" > "$out/trace-stats.txt"
  echo 0 > "$instance/events/enable"
  if [ "$probes" = 1 ]; then
    cat /sys/kernel/tracing/kprobe_profile > "$out/kprobe-profile.txt"
    for event in $registered; do
      echo "-:$event" >> /sys/kernel/tracing/kprobe_events
    done
  fi
  rmdir "$instance"
}
trap cleanup EXIT
trap 'exit 130' INT TERM
echo 0 > "$instance/tracing_on"
echo mono > "$instance/trace_clock"
echo 1 > "$instance/tracing_cpumask"
echo 8192 > "$instance/buffer_size_kb"
for event in sched/sched_switch irq/irq_handler_entry irq/irq_handler_exit irq/softirq_entry irq/softirq_exit; do
  echo 1 > "$instance/events/$event/enable"
done
if [ "$probes" = 1 ]; then
  for f in sync syncmm unmap; do
    case "$f" in sync) symbol=md_sync_task_work;; syncmm) symbol=md_memory_delegation_sync_mm;; unmap) symbol=zap_page_range_single;; esac
    if [ "$f" = unmap ]; then
      echo "p:$name/${f}_enter $symbol start=%x1:x64 len=%x2:u64" >> /sys/kernel/tracing/kprobe_events
    else
      echo "p:$name/${f}_enter $symbol" >> /sys/kernel/tracing/kprobe_events
    fi
    registered="$registered $name/${f}_enter"
    echo "r128:$name/${f}_exit $symbol" >> /sys/kernel/tracing/kprobe_events
    registered="$registered $name/${f}_exit"
  done
  echo 1 > "$instance/events/$name/enable"
fi
boot_id=$(cat /proc/sys/kernel/random/boot_id)
export MD_KERNEL_BRIDGE=required SCUDO_DELEGATION_BACKEND=1 SCUDO_SHARED_ARENA_DISABLE=1
export MEMORY_DELEGATION_DOMAIN=allocator_delegation_v8
export MD_BROKER_SOCKET=/data/local/tmp/md_backend_v8_${boot_id}/backend.sock
test -S "$MD_BROKER_SOCKET"
echo "boot_id=$boot_id" > "$out/config.txt"
if [ -n "${FREQ:-}" ]; then
  old_min=$(cat "$policy/scaling_min_freq")
  old_max=$(cat "$policy/scaling_max_freq")
  old_governor=$(cat "$policy/scaling_governor")
  [ "$FREQ" -ge "$old_min" ] && [ "$FREQ" -le "$old_max" ] || { echo 'FREQ outside current policy bounds' >&2; exit 2; }
  echo "$FREQ" > "$policy/scaling_min_freq"
  echo "$FREQ" > "$policy/scaling_max_freq"
  echo performance > "$policy/scaling_governor"
  cat "$policy/scaling_min_freq" "$policy/scaling_max_freq" > "$out/frequency-start.txt"
fi
if [ "${PRIME:-0}" = 1 ]; then
  p=1
  while [ "$p" -le 12 ]; do
    "$dir/allocator_latency_scudo.trace.aarch64" --size 16777216 --mode process --cpus 0 \
      --workers-per-core 4 --path delegated --allow-fallback 0 --touch-pages 1 \
      --iterations 100 --warmup 5 > /dev/null
    p=$((p+1))
  done
fi
echo 1 > "$instance/tracing_on"
i=1
while [ "$i" -le "${REPEATS:-24}" ]; do
  echo "SLOW_CASE_BEGIN repetition=$i" >> "$out/bench.log"
  "$dir/allocator_latency_scudo.trace.aarch64" --size 16777216 --mode process --cpus 0 \
    --workers-per-core 4 --path delegated --allow-fallback 0 --touch-pages 1 \
    --iterations 100 --warmup 5 >> "$out/bench.log" 2>&1
  i=$((i+1))
done
echo "SLOW_CAPTURE_PASS output=$out"
