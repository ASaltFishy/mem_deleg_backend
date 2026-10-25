#!/usr/bin/env bash
# Build first; this entry deploys and runs without flashing or rebooting.
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
adb_bin=${ADB_BIN:-adb}
adb_args=()
[[ -z "${ADB_SERIAL:-}" ]] || adb_args=(-s "$ADB_SERIAL")
export ADB_SERVER_SOCKET=${ADB_SERVER_SOCKET:-tcp:127.0.0.1:15037}
domain=${DOMAIN:-allocator_delegation_v8}
phone_dir=${PHONE_DIR:-/data/local/tmp/md_backend_v8}
cpus=${CPUS:-0}; matrix=${MATRIX:-power}
allocators=${ALLOCATORS:-scudo,jemalloc}
case "$allocators" in scudo|jemalloc|scudo,jemalloc) ;; *) echo 'ALLOCATORS must be scudo, jemalloc or scudo,jemalloc' >&2; exit 2;; esac
if [[ "$matrix" == power && "$allocators" == jemalloc ]]; then
  echo 'power currently supports Scudo only; use MATRIX=quick or full for jemalloc' >&2; exit 2
fi
iterations=${ITERATIONS:-30}; warmup=${WARMUP:-10}; repetitions=${REPETITIONS:-5}
min_exp=${MIN_EXP:-4}; max_exp=${MAX_EXP:-18}
allow_fallback=${ALLOW_FALLBACK:-1}
primary_iterations=${PRIMARY_ITERATIONS:-10000}
primary_warmup=${PRIMARY_WARMUP:-1000}
order_offset=${ORDER_OFFSET:-0}
size_sweep=${SIZE_SWEEP:-default}
thread_counts=${THREAD_COUNTS:-1,2,4,8}
[[ "$thread_counts" =~ ^(1|2|4|8)(,(1|2|4|8))*$ ]] || { echo 'THREAD_COUNTS must list 1,2,4,8' >&2; exit 2; }
case "$size_sweep" in default|power) ;; *) echo 'SIZE_SWEEP must be default or power' >&2; exit 2;; esac
[[ "$primary_iterations" =~ ^[1-9][0-9]*$ && "$primary_warmup" =~ ^[1-9][0-9]*$ ]]
[[ "$order_offset" =~ ^[0-5]$ ]] || { echo 'ORDER_OFFSET must be 0..5' >&2; exit 2; }
[[ "$allow_fallback" == 0 || "$allow_fallback" == 1 ]] || { echo 'ALLOW_FALLBACK must be 0 or 1' >&2; exit 2; }
[[ "$matrix" == power || "$matrix" == quick || "$matrix" == full || "$matrix" == threads ]]
for value in "$iterations" "$warmup" "$repetitions"; do [[ "$value" =~ ^[1-9][0-9]*$ ]]; done
[[ "$min_exp" =~ ^[0-9]+$ && "$max_exp" =~ ^[0-9]+$ ]]
((min_exp >= 4 && min_exp <= max_exp && max_exp <= 18))
[[ "$domain" =~ ^[A-Za-z0-9_-]+$ && "$phone_dir" =~ ^/[A-Za-z0-9_/-]+$ && "$cpus" =~ ^[0-9]+(,[0-9]+)*$ ]]
if [[ "$matrix" == power && "$cpus" == *,* ]]; then echo 'power requires a single CPU ID' >&2; exit 2; fi
if [[ "${1:-}" == --dry-run ]]; then
  exec sh "$root/tests/phone/run_v2_latency.sh" /unused "$domain" /unused.sock "$cpus" "$matrix" "$iterations" "$warmup" "$repetitions" 1 "$min_exp" "$max_exp" "$allow_fallback" "$primary_iterations" "$primary_warmup" "$order_offset" "$allocators" "$size_sweep" "$thread_counts"
fi
[[ $# == 0 ]] || { echo 'Only --dry-run is supported; configure via environment.' >&2; exit 2; }
results_dir=${RESULTS_DIR:-$root/work/scudo-power-$(date +%Y%m%d-%H%M%S)}
log=${LOG:-$results_dir/run.log}
mkdir -p "$results_dir" "$(dirname "$log")"
echo "Results: $results_dir"
echo "Raw log: $log"
# Check plotting dependency before spending time on the device run.
python3 -c 'import matplotlib'
names=(memory_delegation_backend_broker.aarch64)
if [[ "$matrix" == power ]]; then
  names+=(allocator_latency_scudo.aarch64)
else
  for allocator in ${allocators//,/ }; do names+=("allocator_latency_$allocator.aarch64"); done
fi
version=$(sha256sum "$root/build/memory_delegation_backend_broker.aarch64" | cut -c1-16)
bin_dir="$phone_dir/$version-$(date +%s)-$$"
# adb push always runs as shell. Stage in a shell-owned directory, then install
# through su when the persistent per-boot directory belongs to root.
upload_dir=
cleanup_upload() {
  if [[ -n "$upload_dir" ]]; then
    "$adb_bin" "${adb_args[@]}" shell rm -rf "$upload_dir" || true
  fi
}
trap cleanup_upload EXIT
if [[ "${USE_SU:-0}" == 1 ]]; then
  upload_dir=$("$adb_bin" "${adb_args[@]}" shell mktemp -d /data/local/tmp/md-upload.XXXXXXXX | tr -d '\r\n')
  [[ "$upload_dir" =~ ^/data/local/tmp/md-upload\.[A-Za-z0-9]+$ ]] || {
    upload_dir=; echo 'Cannot create adb upload staging directory' >&2; exit 1;
  }
  deploy_dir=$upload_dir
else
  "$adb_bin" "${adb_args[@]}" shell mkdir -p "$bin_dir"
  deploy_dir=$bin_dir
fi
for name in "${names[@]}"; do "$adb_bin" "${adb_args[@]}" push "$root/build/$name" "$deploy_dir/$name"; done
"$adb_bin" "${adb_args[@]}" push "$root/tests/phone/run_v2_latency.sh" "$deploy_dir/run_v2_latency.sh"
if [[ "${USE_SU:-0}" == 1 ]]; then
  "$adb_bin" "${adb_args[@]}" shell su -c "mkdir -p '$bin_dir'"
  "$adb_bin" "${adb_args[@]}" shell su -c "cp -R '$upload_dir/.' '$bin_dir/'"
  "$adb_bin" "${adb_args[@]}" shell su -c "chmod 755 '$bin_dir/'*"
  cleanup_upload
  upload_dir=
else
  "$adb_bin" "${adb_args[@]}" shell chmod 755 "$bin_dir/"'*'
fi
args="$bin_dir/run_v2_latency.sh $bin_dir $domain $phone_dir/backend.sock $cpus $matrix $iterations $warmup $repetitions 0 $min_exp $max_exp $allow_fallback $primary_iterations $primary_warmup $order_offset $allocators $size_sweep $thread_counts"
if [[ "${USE_SU:-0}" == 1 ]]; then
  "$adb_bin" "${adb_args[@]}" shell su -c "sh $args" 2>&1 | tee "$log"
else
  "$adb_bin" "${adb_args[@]}" shell "sh $args" 2>&1 | tee "$log"
fi
grep -q V2_PHONE_ALL_PASS "$log"
if [[ "$matrix" == power ]]; then
  python3 "$root/tests/plot_scudo_power.py" --log "$log" --output-dir "$results_dir"
else
  python3 "$root/tests/summarize_v2_latency.py" --log "$log" --output-dir "$results_dir"
fi
echo "Completed. Results: $results_dir"
