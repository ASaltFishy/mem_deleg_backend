#!/usr/bin/env bash
# Strict secondary-allocation sweep; run on the Ubuntu build host.
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
export ADB_SERVER_SOCKET=${ADB_SERVER_SOCKET:-tcp:127.0.0.1:15037}
export ADB_SERIAL=${ADB_SERIAL:-3B15AL00K5D00000}
boot_id=$(adb -s "$ADB_SERIAL" shell cat /proc/sys/kernel/random/boot_id | tr -d '\r\n')
[[ "$boot_id" =~ ^[0-9a-f-]{36}$ ]] || { echo 'Cannot read phone boot ID' >&2; exit 1; }
export PHONE_DIR="/data/local/tmp/md_backend_v8_${boot_id}"
export DOMAIN=allocator_delegation_v8 USE_SU=1
export ALLOW_FALLBACK=${ALLOW_FALLBACK:-0}
export CPUS=${CPUS:-0} MIN_EXP=${MIN_EXP:-7} MAX_EXP=${MAX_EXP:-18}
export WARMUP=${WARMUP:-10} ITERATIONS=${ITERATIONS:-30} REPETITIONS=${REPETITIONS:-5}
echo "ABI v3 / broker v8; per-boot phone directory: $PHONE_DIR"
echo "Range: 2^$MIN_EXP .. 2^$MAX_EXP KiB; allow_fallback=$ALLOW_FALLBACK"
cd "$root"
exec bash tests/run_v2_android.sh
