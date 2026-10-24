#!/system/bin/sh
# Persistent broker/domain; no reset, reboot or flashing.
set -eu
DIR=$1; DOMAIN=$2; SOCKET=$3
CPUS=${4:-0}; MATRIX=${5:-power}
ITERATIONS=${6:-10}; WARMUP=${7:-5}; REPETITIONS=${8:-2}
PLAN_ONLY=${9:-0}; MIN_EXP=${10:-4}; MAX_EXP=${11:-18}
case "$MATRIX" in power|quick|full) ;; *) echo "invalid MATRIX" >&2; exit 1;; esac
for value in "$ITERATIONS" "$WARMUP" "$REPETITIONS"; do
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
    "$DIR/allocator_latency_scudo.aarch64" --build-info | grep -q route_protocol=scudo_routes_v1
  fi
  total=$(((MAX_EXP-MIN_EXP+1)*3*3*REPETITIONS)); index=0
  exponent=$MIN_EXP
  while [ "$exponent" -le "$MAX_EXP" ]; do
    kib=$((1 << exponent)); bytes=$((kib*1024))
    for processes in 1 2 4; do
      for variant in cache mmap delegated; do
        path=traditional; cache=enabled; export SCUDO_DELEGATION_BACKEND=0
        case "$variant" in mmap) cache=disabled;; delegated) path=delegated; export SCUDO_DELEGATION_BACKEND=1;; esac
        repetition=1
        while [ "$repetition" -le "$REPETITIONS" ]; do
          index=$((index+1))
          printf '\n[%03d/%03d] Scudo | 2^%s KiB = %s KiB | %s processes on CPU %s | %s | repeat %s/%s\n' \
            "$index" "$total" "$exponent" "$kib" "$processes" "$CPUS" "$variant" "$repetition" "$REPETITIONS"
          echo "POWER_CASE_BEGIN index=$index total=$total exponent=$exponent kib=$kib bytes=$bytes processes=$processes cpu=$CPUS variant=$variant repetition=$repetition"
          if [ "$PLAN_ONLY" != 1 ]; then
            if "$DIR/allocator_latency_scudo.aarch64" --size "$bytes" --mode process --cpus "$CPUS" \
              --workers-per-core "$processes" --path "$path" --cache "$cache" --allow-fallback 1 \
              --touch-pages 1 --iterations "$ITERATIONS" --warmup "$WARMUP"; then
              echo "POWER_CASE_PASS index=$index"
            else
              error=$?
              echo "POWER_CASE_FAIL index=$index exit=$error" >&2
              exit "$error"
            fi
          fi
          repetition=$((repetition+1))
        done
      done
    done
    exponent=$((exponent+1))
  done
else
  sizes=131072; modes=thread; touches=1
  if [ "$MATRIX" = full ]; then sizes="131072 262144 1048576"; modes="thread process"; touches="0 1"; fi
  for allocator in scudo jemalloc; do
    if [ "$PLAN_ONLY" != 1 ]; then "$DIR/allocator_latency_$allocator.aarch64" --build-info | grep -q bench_protocol=shared_rings_v3; fi
    for size in $sizes; do for mode in $modes; do for touch in $touches; do for path in traditional delegated; do
      if [ "$path" = delegated ]; then export SCUDO_DELEGATION_BACKEND=1; else export SCUDO_DELEGATION_BACKEND=0; fi
      repetition=1
      while [ "$repetition" -le "$REPETITIONS" ]; do
        echo "V2_PHONE_CASE allocator=$allocator size=$size mode=$mode touch=$touch path=$path repetition=$repetition"
        if [ "$PLAN_ONLY" != 1 ]; then
          "$DIR/allocator_latency_$allocator.aarch64" --size "$size" --mode "$mode" --cpus "$CPUS" \
            --workers-per-core 1 --path "$path" --touch-pages "$touch" --iterations "$ITERATIONS" --warmup "$WARMUP"
        fi
        repetition=$((repetition+1))
      done
    done; done; done; done
  done
fi
if [ "$PLAN_ONLY" = 1 ]; then echo V2_PLAN_ONLY; else echo V2_PHONE_ALL_PASS; fi
# Leave broker alive and backing intact.
