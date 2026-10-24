#!/usr/bin/env bash
set -euo pipefail
root="$(cd "$(dirname "$0")/.." && pwd)"
ndk="${ANDROID_NDK_ROOT:-/home/lrc/patent/kernel/kernel_platform/prebuilts/ndk-r26}"
api="${ANDROID_API:-34}"
secondary_stats="${SCUDO_SECONDARY_STATS:-0}"
[[ "$secondary_stats" == 0 || "$secondary_stats" == 1 ]] || {
  echo 'SCUDO_SECONDARY_STATS must be 0 or 1' >&2; exit 2;
}
toolchain="$ndk/toolchains/llvm/prebuilt/linux-x86_64/bin"
cxx="$toolchain/aarch64-linux-android${api}-clang++"
scudo="${SCUDO_SOURCE:-/home/lrc/patent/llvm-20/compiler-rt/lib/scudo/standalone}"
jemalloc="${JEMALLOC_SOURCE:-/tmp/md-latency-jemalloc-android}"
[[ -x "$cxx" && -f "$scudo/scudo_delegation.cpp" && -f "$jemalloc/lib/libjemalloc.a" ]] || {
  echo 'Missing NDK, patched Scudo sources or patched Android jemalloc library.' >&2; exit 1;
}
mkdir -p "$root/work"
work=$(mktemp -d "$root/work/android-latency-v2.XXXXXX")
stage="$work/llvm/compiler-rt/lib/scudo/standalone"
mkdir -p "$stage" "$work/objects" "$root/build"
# Stage only source files; do not alter the source snapshot or its old backend.
cp "$scudo"/*.h "$scudo"/*.cpp "$scudo"/*.inc "$scudo"/*.def "$stage/"
cp -R "$scudo/include" "$stage/"
ln -sfn "$root" "$work/llvm/allocator-delegation"
# Use the maintained adapter, including allocation/session identity in each lease.
cp "$root/adapters/scudo_delegation.cpp" "$root/adapters/scudo_delegation.h" "$stage/"
build_id="$(python3 - "$root" "$stage" "$jemalloc" <<'PY'
import hashlib,sys
from pathlib import Path
h=hashlib.sha256()
import os
h.update(os.environ.get('MD_PROFILE_STAGE','0').encode())
h.update(os.environ.get('SCUDO_SECONDARY_STATS','0').encode())
for directory, names in [(sys.argv[1], ['tests/build_android_latency_bench.sh','include/latency_profile.h','src/delegation_backend.cpp','delegation_backend.h','src/kernel_bridge.cpp','kernel_bridge.h','src/jemalloc_adapter.cpp','tests/allocator_latency_bench.cpp']), (sys.argv[2], ['scudo_delegation.cpp','secondary.h']), (sys.argv[3], ['lib/libjemalloc.a'])]:
 for name in names:
  h.update(name.encode()); h.update((Path(directory)/name).read_bytes())
print(h.hexdigest()[:20])
PY
)"
# Android Scudo requires native TLS: emulated TLS calls malloc recursively.
[[ "$api" -ge 29 ]] || { echo "Native TLS requires Android API >= 29" >&2; exit 1; }
common=(-DSCUDO_SECONDARY_STATS="$secondary_stats" -fno-builtin-malloc -fno-builtin-free -DMD_PROFILE_STAGE=${MD_PROFILE_STAGE:-0} -O2 -flto=thin -g -DNDEBUG -fno-emulated-tls -std=c++17 -pthread -fPIC -DMD_BENCH_BUILD_ID=\"$build_id\")
sources=(checksum common condition_variable_linux crc32_hw flags_parser flags
         fuchsia linux mem_map mem_map_fuchsia mem_map_linux release report report_linux
         shared_arena_linux scudo_delegation string_utils timing wrappers_c wrappers_cpp)
if [[ -f "$scudo/alloc_trace.cpp" ]]; then sources+=(alloc_trace); fi
objects=()
for name in "${sources[@]}"; do
  extra=()
  [[ "$name" != crc32_hw ]] || extra=(-march=armv8-a+crc)
  "$cxx" "${common[@]}" -fno-builtin -fno-exceptions -fno-rtti "${extra[@]}" \
    -I"$stage" -I"$stage/include" -I"$root" -c "$stage/$name.cpp" -o "$work/objects/scudo_$name.o"
  objects+=("$work/objects/scudo_$name.o")
done
for name in delegation_backend kernel_bridge; do
  "$cxx" "${common[@]}" -I"$root" -c "$root/src/$name.cpp" -o "$work/objects/$name.o"
done
backend=("$work/objects/delegation_backend.o" "$work/objects/kernel_bridge.o")
"$cxx" "${common[@]}" -I"$root" "$root/tests/cpu_reader_test.cpp" \
  "${backend[@]}" -static-libstdc++ -ldl -latomic -o "$root/build/cpu_reader_test.aarch64"
# Guard the allocator-runtime objects, not prebuilt libc++ exception helpers.
# Losing native TLS here makes malloc recurse before main on Android.
if "$toolchain/llvm-nm" --undefined-only "${objects[@]}" "${backend[@]}" | grep '__emutls_get_address'; then
  echo 'Allocator runtime unexpectedly references emulated TLS; refusing to link.' >&2
  exit 1
fi

"$cxx" "${common[@]}" -DMD_BENCH_SCUDO -I"$stage" \
  -I"$root" \
  "$root/tests/allocator_latency_bench.cpp" "${objects[@]}" "${backend[@]}" \
  -static-libstdc++ -ldl -llog -landroid -latomic -o "$root/build/allocator_latency_scudo.aarch64.new"
"$cxx" "${common[@]}" -DMD_BENCH_JEMALLOC -I"$jemalloc/include" -I"$root" \
  "$root/tests/allocator_latency_bench.cpp" "$root/src/jemalloc_adapter.cpp" \
  "${backend[@]}" "$jemalloc/lib/libjemalloc.a" \
  -static-libstdc++ -ldl -llog -landroid -latomic -o "$root/build/allocator_latency_jemalloc.aarch64.new"
"$cxx" "${common[@]}" -I"$root" "$root/tools/memory_delegation_backend_broker.cpp" \
  "${backend[@]}" -static-libstdc++ -ldl -latomic -o "$root/build/memory_delegation_backend_broker.aarch64"
for allocator in scudo jemalloc; do
  mv "$root/build/allocator_latency_$allocator.aarch64.new" "$root/build/allocator_latency_$allocator.aarch64"
done
python3 - "$root" "$scudo" "$jemalloc" "$ndk" "$api" "$build_id" <<'PY'
import hashlib,json,sys
from pathlib import Path
from datetime import datetime,timezone
root=Path(sys.argv[1])
data=dict(kernel_abi_version=3, broker_version=8, scudo_secondary_stats=__import__('os').environ.get('SCUDO_SECONDARY_STATS','0') == '1', protocol='shared_rings_v3',lto='thin',profile_stage=__import__('os').environ.get('MD_PROFILE_STAGE','0'),tls_model='native_elf',built_at=datetime.now(timezone.utc).isoformat(),
          scudo_source=sys.argv[2],jemalloc_source=sys.argv[3],ndk=sys.argv[4],android_api=sys.argv[5],build_id=sys.argv[6])
data['binaries']={name:hashlib.sha256((root/'build'/f'allocator_latency_{name}.aarch64').read_bytes()).hexdigest() for name in ['scudo','jemalloc']}
(root/'build/allocator_latency_android_manifest.json').write_text(json.dumps(data,indent=2)+'\n')
print(json.dumps(data,indent=2))
PY
