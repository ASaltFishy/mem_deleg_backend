#!/usr/bin/env bash
set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
python_bin="${PYTHON_BIN:-python3}"
scudo_binary="${SCUDO_BENCH_BINARY:-${script_dir}/../build/allocator_latency_scudo.aarch64}"
jemalloc_binary="${JEMALLOC_BENCH_BINARY:-${script_dir}/../build/allocator_latency_jemalloc.aarch64}"
results_dir="${RESULTS_DIR:-${script_dir}/results/phone-$(date +%Y%m%d-%H%M%S)}"

exec "${python_bin}" "${script_dir}/run_allocator_latency_bench.py" \
  --target android \
  --allocators scudo,jemalloc \
  --scudo-binary "${scudo_binary}" \
  --jemalloc-binary "${jemalloc_binary}" \
  --execution-modes userspace,kernel \
  --touch-page-modes 1 \
  --modes thread,process \
  --core-counts 1 \
  --workers-per-core 1,2,4,8 \
  --sizes "${SIZES:-131072,262144,1048576}" \
  --iterations "${ITERATIONS:-10}" \
  --warmup "${WARMUP:-5}" \
  --repetitions "${REPETITIONS:-2}" \
  --command-retries "${COMMAND_RETRIES:-0}" \
  --cooldown "${COOLDOWN:-0.5}" \
  --cold-runs "${COLD_RUNS:-0}" \
  --paths traditional,delegated \
  --adb "${ADB_BIN:-adb}" \
  --adb-serial "${ADB_SERIAL:-}" \
  --device-shm-dir "${DEVICE_SHM_DIR:-/dev/md_shm}" \
  --use-su \
  --results-dir "${results_dir}" \
  "$@"
