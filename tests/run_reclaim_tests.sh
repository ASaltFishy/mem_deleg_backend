#!/usr/bin/env bash
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
mkdir -p "$root/build/reclaim"
cxx=${CXX:-clang++}
flags=(-std=c++17 -O2 -g -pthread -Wall -Wextra -Werror -I"$root")
"$cxx" "${flags[@]}" -DMD_TEST_RECLAIM "$root/src/delegation_backend.cpp" "$root/src/kernel_bridge.cpp" "$root/tests/backend_reclaim_test.cpp" -o "$root/build/reclaim/backend_reclaim_test"
timeout 30 "$root/build/reclaim/backend_reclaim_test"
