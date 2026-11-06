#!/system/bin/sh
# Persistent broker/domain; no reset, reboot or flashing.
set -eu
DIR=$1; DOMAIN=$2; SOCKET=$3
CPUS=${4:-0}; MATRIX=${5:-power}
ITERATIONS=${6:-30}; WARMUP=${7:-10}; REPETITIONS=${8:-5}
PLAN_ONLY=${9:-0}; MIN_EXP=${10:-4}; MAX_EXP=${11:-18}
ALLOW_FALLBACK=${12:-1}
PRIMARY_ITERATIONS=${13:-10000}; PRIMARY_WARMUP=${14:-1000}; ORDER_OFFSET=${15:-0}
ALLOCATORS=${16:-scudo,jemalloc}
SIZE_SWEEP=${17:-default}
THREAD_COUNTS=${18:-1,2,4,8}
PERF_MINIMAL=${19:-0}
PROCESS_COUNTS=${20:-1,2,4,8}
case "$PERF_MINIMAL" in 0|1) ;; *) echo 'Invalid PERF_MINIMAL' >&2; exit 2;; esac
stats_node=/sys/kernel/debug/memory_delegation/switch_cycle_stats_enabled
saved_kernel_stats=
restore_kernel_stats() {
  if [ -n "$saved_kernel_stats" ]; then echo "$saved_kernel_stats" > "$stats_node"; fi
}
trap restore_kernel_stats EXIT
trap 'exit 130' INT TERM
case ",$THREAD_COUNTS," in *,,*|*[!1248,]*) echo 'Invalid THREAD_COUNTS' >&2; exit 2;; esac
thread_counts=$(printf '%s' "$THREAD_COUNTS" | tr ',' ' ')
for threads in $thread_counts; do
  case "$threads" in 1|2|4|8) ;; *) echo 'Invalid thread count' >&2; exit 2;; esac
done
case ",$PROCESS_COUNTS," in *,,*|*[!1248,]*) echo 'Invalid PROCESS_COUNTS' >&2; exit 2;; esac
process_counts=$(printf '%s' "$PROCESS_COUNTS" | tr ',' ' ')
for processes in $process_counts; do
  case "$processes" in 1|2|4|8) ;; *) echo 'Invalid process count' >&2; exit 2;; esac
done
case "$SIZE_SWEEP" in default|power) ;; *) echo 'Invalid SIZE_SWEEP' >&2; exit 2;; esac
case "$ALLOCATORS" in scudo|jemalloc|scudo,jemalloc) ;; *) echo 'Invalid ALLOCATORS' >&2; exit 2;; esac
allocator_list=$(printf '%s' "$ALLOCATORS" | tr ',' ' ')
case "$ORDER_OFFSET" in 0|1|2|3|4|5) ;; *) echo 'ORDER_OFFSET must be 0..5' >&2; exit 2;; esac
case "$ALLOW_FALLBACK" in 0|1) ;; *) echo 'ALLOW_FALLBACK must be 0 or 1' >&2; exit 2;; esac
case "$MATRIX" in power|quick|full|threads|processes) ;; *) echo "invalid MATRIX" >&2; exit 1;; esac
for value in "$ITERATIONS" "$WARMUP" "$REPETITIONS" "$PRIMARY_ITERATIONS" "$PRIMARY_WARMUP"; do
  case "$value" in ''|*[!0-9]*|0) echo "counts must be positive integers" >&2; exit 1;; esac
done
case "$MIN_EXP:$MAX_EXP" in
  *[!0-9:]*|:|:*|*:) echo 'MIN_EXP and MAX_EXP must be integers (KiB exponents)' >&2; exit 2;;
esac
[ "$MIN_EXP" -ge 4 ] && [ "$MAX_EXP" -le 19 ] && [ "$MIN_EXP" -le "$MAX_EXP" ] || {
  echo 'Size range must satisfy 4 <= MIN_EXP <= MAX_EXP <= 19 (16 KiB .. 512 MiB)' >&2; exit 2;
}
if [ "$MATRIX" = power ]; then
  case "$CPUS" in ''|*[!0-9]*) echo "power matrix requires one CPU ID (same-core processes)" >&2; exit 2;; esac
fi
export MD_KERNEL_BRIDGE=required MD_BROKER_SOCKET="$SOCKET" MEMORY_DELEGATION_DOMAIN="$DOMAIN"
export SCUDO_SHARED_ARENA_DISABLE=1
echo "BACKEND_POLICY local_free=immediate remote_free=receipt auto_clear=off explicit_zero=on"
echo "ROUTE_POLICY allow_fallback=$ALLOW_FALLBACK"
echo "MEASUREMENT_POLICY order=balanced_six offset=$ORDER_OFFSET primary_iterations_min=$PRIMARY_ITERATIONS primary_warmup_min=$PRIMARY_WARMUP"
printf '\nCONFIG allocators=%s matrix=%s cpu=%s warmup=%s iterations=%s repetitions=%s perf_minimal=%s arena_bytes=268435456\n' "$ALLOCATORS" "$MATRIX" "$CPUS" "$WARMUP" "$ITERATIONS" "$REPETITIONS" "$PERF_MINIMAL"
if [ "$PLAN_ONLY" != 1 ]; then
  # Read-only ABI probe, before creating backing or starting any benchmark.
  "$DIR/memory_delegation_backend_broker.aarch64" --check-kernel || exit 1
fi
if [ "$PLAN_ONLY" != 1 ]; then
  selected_allocators=$allocator_list
  [ "$MATRIX" != power ] || selected_allocators=scudo
  for selected in $selected_allocators; do
    info=$("$DIR/allocator_latency_$selected.aarch64" --build-info)
    echo "$info" | grep -q "perf_minimal=$PERF_MINIMAL arena_capacity=268435456" || {
      echo 'Rebuild benchmark: PERF_MINIMAL/256 MiB Arena build does not match requested run' >&2; exit 2;
    }
  done
  if [ "$PERF_MINIMAL" = 1 ]; then
    if [ -f "$stats_node" ]; then
      saved_kernel_stats=$(cat "$stats_node")
      echo 0 > "$stats_node"
      echo 'KERNEL_STATS_POLICY enabled=off restore_on_exit=1'
    else
      echo 'KERNEL_STATS_POLICY node=unavailable unchanged=1'
    fi
  fi
fi
if [ "$PLAN_ONLY" != 1 ] && [ ! -S "$SOCKET" ]; then
  nohup "$DIR/memory_delegation_backend_broker.aarch64" "$SOCKET" "$DOMAIN" > "$DIR/broker.log" 2>&1 &
  echo $! > "$DIR/broker.pid"
  n=0
  until grep -q BACKEND_BROKER_READY "$DIR/broker.log"; do
    n=$((n+1))
    [ "$n" -lt 50 ] || { cat "$DIR/broker.log"; exit 1; }
    if ! kill -0 "$(cat "$DIR/broker.pid")" 2>/dev/null; then cat "$DIR/broker.log"; exit 1; fi
    sleep 0.1
  done
fi
if [ "$MATRIX" = power ]; then
  if [ "$PLAN_ONLY" != 1 ]; then
    build_info=$("$DIR/allocator_latency_scudo.aarch64" --build-info)
    echo "$build_info" | grep -q 'route_protocol=scudo_routes_v1'
    echo "$build_info" | grep -q 'prepare_policy=primary_checkpoint_skip_v1' || {
      echo 'Rebuild Android benchmark: missing Primary preparation fix' >&2; exit 2;
    }
    echo "$build_info" | grep -q 'slow_trace=0 touch_trace=0' || {
      echo 'Use an uninstrumented benchmark for performance curves; trace builds are diagnostic only' >&2; exit 2;
    }
  fi
  total=$(((MAX_EXP-MIN_EXP+1)*3*3*REPETITIONS)); index=0
  exponent=$MIN_EXP
  while [ "$exponent" -le "$MAX_EXP" ]; do
    kib=$((1 << exponent)); bytes=$((kib*1024))
    case_iterations=$ITERATIONS; case_warmup=$WARMUP
    if [ "$exponent" -le 6 ]; then
      [ "$case_iterations" -ge "$PRIMARY_ITERATIONS" ] || case_iterations=$PRIMARY_ITERATIONS
      [ "$case_warmup" -ge "$PRIMARY_WARMUP" ] || case_warmup=$PRIMARY_WARMUP
    fi
    group=0
    for processes in 1 2 4; do
      repetition=1
      while [ "$repetition" -le "$REPETITIONS" ]; do
        # Interleave a complete triplet per repetition. All six permutations
        # balance first/last positions over six repeats; offset is reproducible.
        slot=$(((repetition-1+exponent-MIN_EXP+group+ORDER_OFFSET)%6))
        case "$slot" in
          0) variants="cache mmap delegated";;
          1) variants="mmap delegated cache";;
          2) variants="delegated cache mmap";;
          3) variants="delegated mmap cache";;
          4) variants="mmap cache delegated";;
          5) variants="cache delegated mmap";;
        esac
        for variant in $variants; do
          path=traditional; cache=enabled; export SCUDO_DELEGATION_BACKEND=0
          case "$variant" in mmap) cache=disabled;; delegated) path=delegated; export SCUDO_DELEGATION_BACKEND=1;; esac
          index=$((index+1))
          printf '\n[%03d/%03d] Scudo | 2^%s KiB = %s KiB | %s processes on CPU %s | %s | repeat %s/%s\n' \
            "$index" "$total" "$exponent" "$kib" "$processes" "$CPUS" "$variant" "$repetition" "$REPETITIONS"
          echo "POWER_CASE_BEGIN index=$index total=$total exponent=$exponent kib=$kib bytes=$bytes processes=$processes cpu=$CPUS variant=$variant repetition=$repetition iterations=$case_iterations warmup=$case_warmup order_slot=$slot"
          if [ "$PLAN_ONLY" != 1 ]; then
            if "$DIR/allocator_latency_scudo.aarch64" --size "$bytes" --mode process --cpus "$CPUS" \
              --workers-per-core "$processes" --path "$path" --cache "$cache" --allow-fallback "$ALLOW_FALLBACK" \
              --touch-pages 1 --iterations "$case_iterations" --warmup "$case_warmup"; then
              echo "POWER_CASE_PASS index=$index"
            else
              error=$?
              echo "POWER_CASE_FAIL index=$index exit=$error" >&2
              exit "$error"
            fi
          fi
        done
        repetition=$((repetition+1))
      done
      group=$((group+1))
    done
    exponent=$((exponent+1))
  done
else
  sizes=131072; modes=thread; touches=1; worker_counts=1
  if [ "$MATRIX" = full ]; then sizes="131072 262144 1048576"; modes="thread process"; touches="0 1"; fi
  if [ "$MATRIX" = threads ]; then
    modes=thread; touches=1; worker_counts=$thread_counts
    echo "THREAD_MATRIX counts=$THREAD_COUNTS touch_pages=1 cpus=$CPUS"
  fi
  if [ "$MATRIX" = processes ]; then
    modes=process; touches=1; worker_counts=$process_counts
    echo "PROCESS_MATRIX counts=$PROCESS_COUNTS touch_pages=1 cpus=$CPUS"
  fi
  if [ "$SIZE_SWEEP" = power ]; then
    sizes=; exponent=$MIN_EXP
    while [ "$exponent" -le "$MAX_EXP" ]; do
      sizes="$sizes $(((1 << exponent)*1024))"
      exponent=$((exponent+1))
    done
    echo "SIZE_SWEEP power min_exp=$MIN_EXP max_exp=$MAX_EXP unit=KiB"
  fi
  total=0; index=0
  for a in $allocator_list; do for size in $sizes; do for mode in $modes; do for touch in $touches; do for workers in $worker_counts; do
    total=$((total+2*REPETITIONS))
  done; done; done; done; done
  echo "MATRIX_TOTAL groups=$total"
  for allocator in $allocator_list; do
    if [ "$PLAN_ONLY" != 1 ]; then "$DIR/allocator_latency_$allocator.aarch64" --build-info | grep -q bench_protocol=shared_rings_v3; fi
    for size in $sizes; do for mode in $modes; do for touch in $touches; do for workers in $worker_counts; do
      case_iterations=$ITERATIONS; case_warmup=$WARMUP
      if [ "$SIZE_SWEEP" = power ] && [ "$size" -le 65536 ]; then
        [ "$case_iterations" -ge "$PRIMARY_ITERATIONS" ] || case_iterations=$PRIMARY_ITERATIONS
        [ "$case_warmup" -ge "$PRIMARY_WARMUP" ] || case_warmup=$PRIMARY_WARMUP
      fi
      repetition=1
      while [ "$repetition" -le "$REPETITIONS" ]; do
        # Alternate which configuration runs first in each repeated pair.
        paths="traditional delegated"
        if [ $(((repetition+workers)%2)) -eq 1 ]; then paths="delegated traditional"; fi
        for path in $paths; do
          if [ "$path" = delegated ]; then export SCUDO_DELEGATION_BACKEND=1; else export SCUDO_DELEGATION_BACKEND=0; fi
          index=$((index+1))
          printf '\n[%03d/%03d] %s | %s KiB | %s | %s workers/CPU | touch=%s | %s | repeat %s/%s\n' \
            "$index" "$total" "$allocator" "$((size/1024))" "$mode" "$workers" "$touch" "$path" "$repetition" "$REPETITIONS"
          echo "LATENCY_CASE index=$index total=$total allocator=$allocator size=$size mode=$mode touch=$touch workers_per_core=$workers path=$path repetition=$repetition"
          if [ "$PLAN_ONLY" != 1 ]; then
            if "$DIR/allocator_latency_$allocator.aarch64" --size "$size" --mode "$mode" --cpus "$CPUS" \
              --workers-per-core "$workers" --path "$path" --allow-fallback "$ALLOW_FALLBACK" --touch-pages "$touch" --iterations "$case_iterations" --warmup "$case_warmup"; then
              echo "LATENCY_CASE_PASS index=$index"
            else
              error=$?
              echo "LATENCY_CASE_FAIL index=$index exit=$error" >&2
              exit "$error"
            fi
          fi
        done
        repetition=$((repetition+1))
      done
    done; done; done; done
  done
fi
if [ "$PLAN_ONLY" = 1 ]; then echo LATENCY_PLAN_ONLY; else echo LATENCY_ALL_PASS; fi
# Leave broker alive and backing intact.
