#!/system/bin/sh
# Diagnostic run only. Requires root and a benchmark built with MD_SLOW_TRACE=1.
# Use the current per-boot domain/socket; do not reset backing or change clocks.
set -eu
BENCH=${BENCH:?Set BENCH to the full path of the trace-enabled Scudo benchmark}
VARIANT=${VARIANT:-mmap}; SIZE=${SIZE:-2097152}; WORKERS=${WORKERS:-4}
ITERATIONS=${ITERATIONS:-100}; WARMUP=${WARMUP:-10}; REPEATS=${REPEATS:-1}
ALLOW_FALLBACK=${ALLOW_FALLBACK:-1}
for value in "$SIZE" "$WORKERS" "$ITERATIONS" "$WARMUP" "$REPEATS"; do
  case "$value" in ''|*[!0-9]*|0) echo 'Counts and size must be positive integers' >&2; exit 2;; esac
done
case "$ALLOW_FALLBACK" in 0|1) ;; *) exit 2;; esac
path=traditional; cache=enabled; export SCUDO_DELEGATION_BACKEND=0
case "$VARIANT" in
  cache) ;;
  mmap) cache=disabled;;
  delegated) path=delegated; export SCUDO_DELEGATION_BACKEND=1;;
  *) echo 'VARIANT must be cache, mmap or delegated' >&2; exit 2;;
esac
"$BENCH" --build-info | grep -q 'slow_trace=1' || {
  echo 'Rebuild the benchmark with MD_SLOW_TRACE=1 before capture' >&2; exit 2;
}
boot_id=$(cat /proc/sys/kernel/random/boot_id)
export MEMORY_DELEGATION_DOMAIN=${MEMORY_DELEGATION_DOMAIN:-allocator_delegation_v8}
export MD_BROKER_SOCKET=${MD_BROKER_SOCKET:-/data/local/tmp/md_backend_v8_${boot_id}/backend.sock}
export MD_KERNEL_BRIDGE=required SCUDO_SHARED_ARENA_DISABLE=1
if [ "$path" = delegated ]; then test -S "$MD_BROKER_SOCKET"; fi
name=md_alloc_sched_$(date +%Y%m%d_%H%M%S)_$$
OUT=${OUT:-/data/local/tmp/$name}
instance=/sys/kernel/tracing/instances/$name
mkdir -p "$OUT"
mkdir "$instance"
cleanup() {
  set +e
  echo 0 > "$instance/tracing_on"
  cat "$instance/trace" > "$OUT/trace.txt"
  cat "$instance/per_cpu/cpu0/stats" > "$OUT/trace-stats.txt"
  echo 0 > "$instance/events/enable"
  rmdir "$instance"
}
trap cleanup EXIT
trap 'exit 130' INT TERM
echo 0 > "$instance/tracing_on"
echo mono > "$instance/trace_clock"
echo 1 > "$instance/tracing_cpumask"
echo 16384 > "$instance/buffer_size_kb"
: > "$OUT/events.txt"
for event in sched/sched_switch sched/sched_wakeup irq/irq_handler_entry irq/irq_handler_exit irq/softirq_entry irq/softirq_exit; do
  echo 1 > "$instance/events/$event/enable"
  echo "$event" >> "$OUT/events.txt"
done
# Kernel configurations differ; report exactly which syscall events exist.
for event in sys_enter_mmap sys_exit_mmap sys_enter_munmap sys_exit_munmap sys_enter_madvise sys_exit_madvise; do
  if [ -f "$instance/events/syscalls/$event/enable" ]; then
    echo 1 > "$instance/events/syscalls/$event/enable"
    echo "syscalls/$event" >> "$OUT/events.txt"
  fi
done
echo "diagnostic_only=1 clock=mono cpu=0 variant=$VARIANT size=$SIZE workers=$WORKERS iterations=$ITERATIONS warmup=$WARMUP repeats=$REPEATS boot_id=$boot_id" > "$OUT/config.txt"
"$BENCH" --build-info >> "$OUT/config.txt"
echo 1 > "$instance/tracing_on"
i=1
while [ "$i" -le "$REPEATS" ]; do
  echo "SCHED_CASE_BEGIN repetition=$i variant=$VARIANT" >> "$OUT/bench.log"
  "$BENCH" --size "$SIZE" --mode process --cpus 0 --workers-per-core "$WORKERS" \
    --path "$path" --cache "$cache" --allow-fallback "$ALLOW_FALLBACK" \
    --touch-pages 1 --iterations "$ITERATIONS" --warmup "$WARMUP" >> "$OUT/bench.log" 2>&1
  i=$((i+1))
done
echo "SCHED_CAPTURE_PASS output=$OUT"
