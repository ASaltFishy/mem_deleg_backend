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
case ",$THREAD_COUNTS," in *,,*|*[!1248,]*) echo 'Invalid THREAD_COUNTS' >&2; exit 2;; esac
thread_counts=$(printf '%s' "$THREAD_COUNTS" | tr ',' ' ')
for threads in $thread_counts; do
  case "$threads" in 1|2|4|8) ;; *) echo 'Invalid thread count' >&2; exit 2;; esac
done
case "$SIZE_SWEEP" in default|power) ;; *) echo 'Invalid SIZE_SWEEP' >&2; exit 2;; esac
case "$ALLOCATORS" in scudo|jemalloc|scudo,jemalloc) ;; *) echo 'Invalid ALLOCATORS' >&2; exit 2;; esac
allocator_list=$(printf '%s' "$ALLOCATORS" | tr ',' ' ')
case "$ORDER_OFFSET" in 0|1|2|3|4|5) ;; *) echo 'ORDER_OFFSET must be 0..5' >&2; exit 2;; esac
case "$ALLOW_FALLBACK" in 0|1) ;; *) echo 'ALLOW_FALLBACK must be 0 or 1' >&2; exit 2;; esac
case "$MATRIX" in power|quick|full|threads) ;; *) echo "invalid MATRIX" >&2; exit 1;; esac
for value in "$ITERATIONS" "$WARMUP" "$REPETITIONS" "$PRIMARY_ITERATIONS" "$PRIMARY_WARMUP"; do
  case "$value" in ''|*[!0-9]*|0) echo "counts must be positive integers" >&2; exit 1;; esac
done
case "$MIN_EXP:$MAX_EXP" in *[!0-9:]*|:|:*|*:) exit 2;; esac
[ "$MIN_EXP" -ge 4 ] && [ "$MAX_EXP" -le 18 ] && [ "$MIN_EXP" -le "$MAX_EXP" ] || exit 2
if [ "$MATRIX" = power ]; then
  case "$CPUS" in ''|*[!0-9]*) echo "power matrix requires one CPU ID (same-core processes)" >&2; exit 2;; esac
fi
export MD_KERNEL_BRIDGE=required MD_BROKER_SOCKET="$SOCKET" MEMORY_DELEGATION_DOMAIN="$DOMAIN"
export SCUDO_SHARED_ARENA_DISABLE=1
echo "BACKEND_POLICY local_free=immediate remote_free=receipt auto_clear=off explicit_zero=on"
echo "ROUTE_POLICY allow_fallback=$ALLOW_FALLBACK"
echo "MEASUREMENT_POLICY order=balanced_six offset=$ORDER_OFFSET primary_iterations_min=$PRIMARY_ITERATIONS primary_warmup_min=$PRIMARY_WARMUP"
printf '\nCONFIG allocator=scudo matrix=%s cpu=%s warmup=%s iterations=%s repetitions=%s\n' "$MATRIX" "$CPUS" "$WARMUP" "$ITERATIONS" "$REPETITIONS"
if [ "$PLAN_ONLY" != 1 ]; then
  # Read-only ABI probe, before creating backing or starting any benchmark.
  "$DIR/memory_delegation_backend_broker.aarch64" --check-kernel || exit 1
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
  if [ "$SIZE_SWEEP" = power ]; then
    sizes=; exponent=$MIN_EXP
    while [ "$exponent" -le "$MAX_EXP" ]; do
      sizes="$sizes $(((1 << exponent)*1024))"
      exponent=$((exponent+1))
    done
    echo "SIZE_SWEEP power min_exp=$MIN_EXP max_exp=$MAX_EXP unit=KiB"
  fi
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
          echo "V2_PHONE_CASE allocator=$allocator size=$size mode=$mode touch=$touch workers_per_core=$workers path=$path repetition=$repetition"
          if [ "$PLAN_ONLY" != 1 ]; then
            "$DIR/allocator_latency_$allocator.aarch64" --size "$size" --mode "$mode" --cpus "$CPUS" \
              --workers-per-core "$workers" --path "$path" --allow-fallback "$ALLOW_FALLBACK" --touch-pages "$touch" --iterations "$case_iterations" --warmup "$case_warmup"
          fi
        done
        repetition=$((repetition+1))
      done
    done; done; done; done
  done
fi
if [ "$PLAN_ONLY" = 1 ]; then echo V2_PLAN_ONLY; else echo V2_PHONE_ALL_PASS; fi
# Leave broker alive and backing intact.
