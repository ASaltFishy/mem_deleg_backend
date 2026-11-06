#!/usr/bin/env bash
set -euo pipefail

root="$(cd "$(dirname "$0")" && pwd)"
build="$root/build"
cxx="${CXX:-clang++}"
common=(-O2 -std=c++17 -pthread -Wall -Wextra -Werror)
mkdir -p "$build"

"$cxx" "${common[@]}" -I"$root" -c "$root/src/delegation_backend.cpp" \
  -o "$build/delegation_backend.o"
"$cxx" "${common[@]}" -I"$root" -c "$root/src/kernel_bridge.cpp" \
  -o "$build/kernel_bridge.o"

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
  grep -Eq '^#define[[:space:]]+PR_MEMORY_DELEGATION_F_SHARED_RINGS[[:space:]]+\(1UL << 4\)$' \
    "$KERNEL_TREE/include/uapi/linux/prctl.h"
else
  echo "KERNEL_TREE is unset; skipped live kernel-UAPI check"
fi

echo "Backend compile checks completed"
