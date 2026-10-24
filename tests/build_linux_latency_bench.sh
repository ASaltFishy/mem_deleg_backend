#!/usr/bin/env bash
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
scudo=${SCUDO_SOURCE:-/home/lrc/patent/llvm-20/compiler-rt/lib/scudo/standalone}
cxx=${CXX:-aarch64-linux-gnu-g++}
work="$root/build/linux-v2"
stage="$work/scudo"
mkdir -p "$stage/include" "$work/objects"
cp "$scudo"/*.h "$scudo"/*.cpp "$scudo"/*.inc "$scudo"/*.def "$stage/"
cp -R "$scudo/include/." "$stage/include/"
cp "$root/adapters/scudo_delegation.cpp" "$root/adapters/scudo_delegation.h" "$stage/"
secondary_stats=${SCUDO_SECONDARY_STATS:-0}
[[ "$secondary_stats" == 0 || "$secondary_stats" == 1 ]] || exit 2
common=(-DSCUDO_SECONDARY_STATS="$secondary_stats" -std=c++17 -O2 -g -DNDEBUG -pthread -fno-exceptions -fno-rtti -I"$stage" -I"$stage/include" -I"$root")
sources=(checksum common condition_variable_linux crc32_hw flags_parser flags
         fuchsia linux mem_map mem_map_fuchsia mem_map_linux release report report_linux
         shared_arena_linux scudo_delegation string_utils timing wrappers_c wrappers_cpp)
if [[ -f "$scudo/alloc_trace.cpp" ]]; then sources+=(alloc_trace); fi
objects=()
for name in "${sources[@]}"; do
  extra=()
  [[ "$name" != crc32_hw ]] || extra=(-march=armv8-a+crc)
  "$cxx" "${common[@]}" -fno-builtin "${extra[@]}" -c "$stage/$name.cpp" -o "$work/objects/scudo_$name.o"
  objects+=("$work/objects/scudo_$name.o")
done
for name in delegation_backend kernel_bridge; do
  "$cxx" "${common[@]}" -c "$root/src/$name.cpp" -o "$work/objects/$name.o"
  objects+=("$work/objects/$name.o")
done
"$cxx" "${common[@]}" -DMD_BENCH_SCUDO -DMD_BENCH_BUILD_ID=\"shared_rings_v3\" \
  "$root/tests/allocator_latency_bench.cpp" "${objects[@]}" -static -ldl \
  -o "$root/build/allocator_latency_scudo.linux.aarch64"

if [[ -n "${JEMALLOC_LINUX_SOURCE:-}" ]]; then
  jemalloc="$JEMALLOC_LINUX_SOURCE"
  grep -q md_jemalloc_hooks "$jemalloc/src/pac.c"
  "$cxx" "${common[@]}" -DMD_BENCH_JEMALLOC -DMD_BENCH_BUILD_ID=\"shared_rings_v3\" -I"$jemalloc/include" \
    "$root/tests/allocator_latency_bench.cpp" "$root/src/jemalloc_adapter.cpp" \
    "$work/objects/delegation_backend.o" "$work/objects/kernel_bridge.o" \
    "$jemalloc/lib/libjemalloc.a" -static -ldl -o "$root/build/allocator_latency_jemalloc.linux.aarch64"
fi
