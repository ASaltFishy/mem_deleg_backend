#!/usr/bin/env bash
set -euo pipefail
root="$(cd "$(dirname "$0")/.." && pwd)"
ndk="${ANDROID_NDK_ROOT:-/home/lrc/patent/kernel/kernel_platform/prebuilts/ndk-r26}"
api="${ANDROID_API:-34}"
toolchain="$ndk/toolchains/llvm/prebuilt/linux-x86_64/bin"
cxx="$toolchain/aarch64-linux-android${api}-clang++"
scudo="${SCUDO_SOURCE:-/tmp/md-latency-llvm-20261018/compiler-rt/lib/scudo/standalone}"
jemalloc="${JEMALLOC_SOURCE:-/tmp/md-latency-jemalloc-android}"
[[ -x "$cxx" && -f "$scudo/scudo_delegation.cpp" && -f "$jemalloc/lib/libjemalloc.a" ]] || {
  echo 'Missing NDK, patched Scudo sources or patched Android jemalloc library.' >&2; exit 1;
}
work="$root/work/android-latency"
stage="$work/llvm/compiler-rt/lib/scudo/standalone"
mkdir -p "$stage" "$work/objects" "$root/build"
# Stage only source files; do not alter the source snapshot or its old backend.
cp "$scudo"/*.h "$scudo"/*.cpp "$scudo"/*.inc "$scudo"/*.def "$stage/"
cp -R "$scudo/include" "$stage/"
ln -sfn "$root" "$work/llvm/allocator-delegation"
python3 - "$stage/scudo_delegation.cpp" <<'PY'
from pathlib import Path
import sys
p = Path(sys.argv[1]); s = p.read_text()
if '#if defined(__ANDROID__)' not in s:
    s = s.replace('  Config.fixed_address = 0x280000000000ULL;',
        '#if defined(__ANDROID__)\n  Config.fixed_address = 0x2000000000ULL;\n#else\n  Config.fixed_address = 0x280000000000ULL;\n#endif')
s = s.replace('Config.capacity = 128ULL << 20;',
              'Config.capacity = MD_DEFAULT_ARENA_CAPACITY;')
if 'Config.capacity = MD_DEFAULT_ARENA_CAPACITY;' not in s:
    raise SystemExit('Unknown Scudo capacity configuration')
# A concurrent caller must wait while another thread opens the backend.
old = """  if (atomic_load(&InitState, memory_order_acquire) == 0u)
    initialize();"""
new = """  const u32 State = atomic_load(&InitState, memory_order_acquire);
  if (State == 0u || State == 1u)
    initialize();"""
if old in s:
    s = s.replace(old, new, 1)
elif new not in s:
    raise SystemExit('Unknown Scudo initialization implementation; review the concurrency fix')
# Report actual backend errors only on fallback, without allocating memory.
old_acquire = """  if (md_acquire(Current, Size + HeadersSize + Alignment,
                 Max(Alignment, PageSize),
                 md_process_id(), &Acquired) != 0)
    return false;"""
new_acquire = """  const int AcquireError = md_acquire(Current, Size + HeadersSize + Alignment,
                 Max(Alignment, PageSize), md_process_id(), &Acquired);
  if (AcquireError) {
    char Message[160];
    int Length = snprintf(Message, sizeof(Message),
        "scudo delegation acquire failed: error=%d request=%zu\\n",
        AcquireError, static_cast<size_t>(Size + HeadersSize + Alignment));
    if (Length > 0) (void)write(2, Message, Min(static_cast<uptr>(Length), sizeof(Message) - 1));
    return false;
  }"""
if old_acquire not in s:
    raise SystemExit('Unknown Scudo acquire implementation')
s = s.replace(old_acquire, new_acquire, 1)
s = s.replace('#include <stdlib.h>', '#include <stdlib.h>\n#include <stdio.h>', 1)
s += '\n#include <errno.h>\nextern "C" int md_scudo_checkpoint() {\n  return scudo::getBackend() ? md_backend_checkpoint(scudo::getBackend()) : ENODEV;\n}\n'
p.write_text(s)
PY
build_id="$(python3 - "$root" "$stage" "$jemalloc" <<'PY'
import hashlib,sys
from pathlib import Path
h=hashlib.sha256()
for directory, names in [(sys.argv[1], ['tests/build_android_latency_bench.sh','src/delegation_backend.cpp','delegation_backend.h','src/kernel_bridge.cpp','kernel_bridge.h','src/jemalloc_adapter.cpp','tests/allocator_latency_bench.cpp']), (sys.argv[2], ['scudo_delegation.cpp']), (sys.argv[3], ['lib/libjemalloc.a'])]:
 for name in names:
  h.update(name.encode()); h.update((Path(directory)/name).read_bytes())
print(h.hexdigest()[:20])
PY
)"
# Android Scudo requires native TLS: emulated TLS calls malloc recursively.
[[ "$api" -ge 29 ]] || { echo "Native TLS requires Android API >= 29" >&2; exit 1; }
common=(-O2 -g -DNDEBUG -fno-emulated-tls -std=c++17 -pthread -fPIC -DMD_BENCH_BUILD_ID=\"$build_id\")
sources=(alloc_trace checksum common condition_variable_linux crc32_hw flags_parser flags
         fuchsia linux mem_map mem_map_fuchsia mem_map_linux release report report_linux
         shared_arena_linux scudo_delegation string_utils timing wrappers_c wrappers_cpp)
objects=()
for name in "${sources[@]}"; do
  extra=()
  [[ "$name" != crc32_hw ]] || extra=(-march=armv8-a+crc)
  "$cxx" "${common[@]}" -fno-builtin -fno-exceptions -fno-rtti "${extra[@]}" \
    -I"$stage" -I"$stage/include" -c "$stage/$name.cpp" -o "$work/objects/scudo_$name.o"
  objects+=("$work/objects/scudo_$name.o")
done
for name in delegation_backend kernel_bridge; do
  "$cxx" "${common[@]}" -I"$root" -c "$root/src/$name.cpp" -o "$work/objects/$name.o"
done
backend=("$work/objects/delegation_backend.o" "$work/objects/kernel_bridge.o")
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
for allocator in scudo jemalloc; do
  mv "$root/build/allocator_latency_$allocator.aarch64.new" "$root/build/allocator_latency_$allocator.aarch64"
done
python3 - "$root" "$scudo" "$jemalloc" "$ndk" "$api" "$build_id" <<'PY'
import hashlib,json,sys
from pathlib import Path
from datetime import datetime,timezone
root=Path(sys.argv[1])
data=dict(protocol='async_checkpoint_v1',tls_model='native_elf',built_at=datetime.now(timezone.utc).isoformat(),
          scudo_source=sys.argv[2],jemalloc_source=sys.argv[3],ndk=sys.argv[4],android_api=sys.argv[5],build_id=sys.argv[6])
data['binaries']={name:hashlib.sha256((root/'build'/f'allocator_latency_{name}.aarch64').read_bytes()).hexdigest() for name in ['scudo','jemalloc']}
(root/'build/allocator_latency_android_manifest.json').write_text(json.dumps(data,indent=2)+'\n')
print(json.dumps(data,indent=2))
PY
