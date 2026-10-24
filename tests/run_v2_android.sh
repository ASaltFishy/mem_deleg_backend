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
iterations=${ITERATIONS:-10}; warmup=${WARMUP:-5}; repetitions=${REPETITIONS:-2}
min_exp=${MIN_EXP:-4}; max_exp=${MAX_EXP:-18}
[[ "$matrix" == power || "$matrix" == quick || "$matrix" == full ]]
for value in "$iterations" "$warmup" "$repetitions"; do [[ "$value" =~ ^[1-9][0-9]*$ ]]; done
[[ "$min_exp" =~ ^[0-9]+$ && "$max_exp" =~ ^[0-9]+$ ]]
((min_exp >= 4 && min_exp <= max_exp && max_exp <= 18))
[[ "$domain" =~ ^[A-Za-z0-9_-]+$ && "$phone_dir" =~ ^/[A-Za-z0-9_/-]+$ && "$cpus" =~ ^[0-9]+(,[0-9]+)*$ ]]
if [[ "$matrix" == power && "$cpus" == *,* ]]; then echo 'power requires a single CPU ID' >&2; exit 2; fi
if [[ "${1:-}" == --dry-run ]]; then
  exec sh "$root/tests/phone/run_v2_latency.sh" /unused "$domain" /unused.sock "$cpus" "$matrix" "$iterations" "$warmup" "$repetitions" 1 "$min_exp" "$max_exp"
fi
[[ $# == 0 ]] || { echo 'Only --dry-run is supported; configure via environment.' >&2; exit 2; }
results_dir=${RESULTS_DIR:-$root/work/scudo-power-$(date +%Y%m%d-%H%M%S)}
log=${LOG:-$results_dir/run.log}
mkdir -p "$results_dir" "$(dirname "$log")"
echo "Results: $results_dir"
echo "Raw log: $log"
# Check plotting dependency before spending time on the device run.
if [[ "$matrix" == power ]]; then python3 -c 'import matplotlib'; fi
names=(allocator_latency_scudo.aarch64 memory_delegation_backend_broker.aarch64)
[[ "$matrix" == power ]] || names+=(allocator_latency_jemalloc.aarch64)
"$adb_bin" "${adb_args[@]}" shell mkdir -p "$phone_dir"
version=$(sha256sum "$root/build/memory_delegation_backend_broker.aarch64" | cut -c1-16)
bin_dir="$phone_dir/$version-$(date +%s)-$$"
"$adb_bin" "${adb_args[@]}" shell mkdir -p "$bin_dir"
for name in "${names[@]}"; do "$adb_bin" "${adb_args[@]}" push "$root/build/$name" "$bin_dir/$name"; done
"$adb_bin" "${adb_args[@]}" push "$root/tests/phone/run_v2_latency.sh" "$bin_dir/run_v2_latency.sh"
"$adb_bin" "${adb_args[@]}" shell chmod 755 "$bin_dir/"'*'
args="$bin_dir/run_v2_latency.sh $bin_dir $domain $phone_dir/backend.sock $cpus $matrix $iterations $warmup $repetitions 0 $min_exp $max_exp"
if [[ "${USE_SU:-0}" == 1 ]]; then
  "$adb_bin" "${adb_args[@]}" shell su -c "sh $args" 2>&1 | tee "$log"
else
  "$adb_bin" "${adb_args[@]}" shell "sh $args" 2>&1 | tee "$log"
fi
grep -q V2_PHONE_ALL_PASS "$log"
if [[ "$matrix" == power ]]; then
  python3 "$root/tests/plot_scudo_power.py" --log "$log" --output-dir "$results_dir"
fi
echo "Completed. Results: $results_dir"
