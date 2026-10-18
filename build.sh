#!/usr/bin/env bash
set -euo pipefail

root="$(cd "$(dirname "$0")" && pwd)"
build="$root/build"
cxx="${CXX:-clang++}"
common=(-O2 -std=c++17 -pthread -Wall -Wextra -Werror)
mkdir -p "$build"

compile_test() {
  local output="$1"
  local source="$2"
  shift 2
  "$cxx" "${common[@]}" "$@" \
    "$root/delegation_backend.cpp" "$root/kernel_bridge.cpp" "$root/$source" \
    -o "$build/$output"
}

compile_test backend_test backend_test.cpp
compile_test optimization_test optimization_test.cpp
compile_test handoff_test handoff_test.cpp
compile_test concurrency_test concurrency_test.cpp
compile_test rseq_abort_test rseq_abort_test.cpp -DMD_TEST_RSEQ_DELAY
compile_test first_open_test first_open_test.cpp -DMD_TEST_FIRST_OPEN_DELAY
compile_test preemption_test preemption_test.cpp -DMD_TEST_PREEMPT_LOCK
compile_test contention_bench contention_bench.cpp
compile_test structure_bench structure_bench.cpp

"$cxx" "${common[@]}" -c "$root/kernel_bridge.cpp" \
  -o "$build/kernel_bridge.o"
"$cxx" "${common[@]}" "$root/kernel_bridge.cpp" \
  "$root/kernel_bridge_test.cpp" -o "$build/kernel_bridge_test"
"$cxx" "${common[@]}" "$root/kernel_bridge.cpp" \
  "$root/kernel_external_backend_test.cpp" \
  -o "$build/kernel_external_backend_test"

if [[ -n "${KERNEL_TREE:-}" ]]; then
  cmp "$root/include/uapi/linux/memory_delegation.h" \
      "$KERNEL_TREE/include/uapi/linux/memory_delegation.h"
  grep -Eq '^#define[[:space:]]+PR_SET_MEMORY_DELEGATION_LOG[[:space:]]+74$' \
    "$KERNEL_TREE/include/uapi/linux/prctl.h"
  grep -Eq '^#define[[:space:]]+PR_MEMORY_DELEGATION_F_RESET_ARENA[[:space:]]+\(1UL << 0\)$' \
    "$KERNEL_TREE/include/uapi/linux/prctl.h"
  grep -Eq '^#define[[:space:]]+PR_MEMORY_DELEGATION_F_DRAIN_LOG[[:space:]]+\(1UL << 1\)$' \
    "$KERNEL_TREE/include/uapi/linux/prctl.h"
  grep -Eq '^#define[[:space:]]+PR_MEMORY_DELEGATION_F_EXTERNAL_BACKEND[[:space:]]+\(1UL << 2\)$' \
    "$KERNEL_TREE/include/uapi/linux/prctl.h"
  "$cxx" "${common[@]}" "$root/kernel_abi_test.cpp" \
    -o "$build/kernel_abi_test"
  "$build/kernel_abi_test"
else
  echo "KERNEL_TREE is unset; skipped live kernel-UAPI compile check"
fi

taskset -c "${TEST_CPU:-0}" "$build/backend_test"
taskset -c "${TEST_CPU:-0}" "$build/optimization_test"
MD_EAGER_CLEAR=0 taskset -c "${TEST_CPU:-0}" "$build/handoff_test"
MD_EAGER_CLEAR=1 taskset -c "${TEST_CPU:-0}" "$build/handoff_test"
"$build/first_open_test"
"$build/kernel_bridge_test"

echo "Core build and smoke tests passed"
