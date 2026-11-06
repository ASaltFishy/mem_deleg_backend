# Allocator source provenance

## jemalloc

The pristine jemalloc 5.3.0 release archive is preserved here (COPYING is inside).
SHA-256: `2db82d1e7119df3e71b7640219b6dfe84789bc0537983c3b7ac4f7189aecfeaa`.
`patches/jemalloc-5.3.0-delegation.patch` captures every upstream source difference
in `/tmp/md-latency-jemalloc-android`: only `src/pac.c` differs. Generated platform
headers/build products are not vendored. The native 8 MiB oversize eager purge
and its default threshold are unchanged from upstream.

Recreate the Android static library on the Ubuntu build host:

```bash
mkdir -p work/jemalloc-rebuild
tar -xjf third_party/jemalloc-5.3.0.tar.bz2 -C work/jemalloc-rebuild
patch -d work/jemalloc-rebuild/jemalloc-5.3.0 -p1 < third_party/patches/jemalloc-5.3.0-delegation.patch
cd work/jemalloc-rebuild/jemalloc-5.3.0
CC=/home/lrc/patent/kernel/kernel_platform/prebuilts/ndk-r26/toolchains/llvm/prebuilt/linux-x86_64/bin/aarch64-linux-android34-clang \
CXX=/home/lrc/patent/kernel/kernel_platform/prebuilts/ndk-r26/toolchains/llvm/prebuilt/linux-x86_64/bin/aarch64-linux-android34-clang++ \
./configure --host=aarch64-linux-android --disable-shared --enable-static --disable-cxx --with-jemalloc-prefix=je_
make -j4
```

The captured configure used host `ar`/`ranlib`, default configure CFLAGS (-O3,
-g3, hidden visibility), Android NDK r26/API 34, and no user CFLAGS/LDFLAGS.
Set JEMALLOC_SOURCE to the recreated source directory when invoking
`tests/build_android_latency_bench.sh`. This recipe is recorded, not rerun as
part of the milestone snapshot; recreated binaries need not be byte-identical.

## Scudo

Scudo can be restored from this Backend repository without a separately modified
LLVM checkout. `scudo-source.json` records the LLVM base, captured commit, SHA-256
checksums, and expected patched Git tree. The source archive contains only
`compiler-rt/lib/scudo/standalone`, including its upstream license files.

- Base: `87f0227cb60147a26a1eeb4fb06e3b505e9c7261`.
- Captured source: `ddaece0ad5c1912baff327980c0da6e9596e7a4c`.
- The cumulative patch includes the earlier SharedArena prototype, delegation
  integration, instrumentation, and supporting tests/scripts.
- Backend and maintained adapters are still provided by this repository;
  Scudo forwarding translation units reference them instead of duplicating them.

Restore and select the source for the existing benchmark build scripts:

```bash
SCUDO_SOURCE="$(python3 third_party/prepare_scudo.py)"
export SCUDO_SOURCE
PERF_MINIMAL=1 bash tests/build_android_latency_bench.sh
```

The default destination is `work/scudo-source`. The helper refuses to overwrite
an existing destination; choose another with `--output work/scudo-source-new`.
It checks archive/patch hashes and verifies the complete restored Scudo Git tree,
including file modes and symlinks. Requires Git and Python with tarfile extraction
filter support (Python 3.12+, or supported backports).
Existing external-source build defaults remain compatible; explicitly set
`SCUDO_SOURCE` as above to select the archived source.

The captured `DESIGN.md` symlink may reference an unavailable external document;
it is preserved for source fidelity and is not required for compilation.
Update the cumulative patch and manifest together for future Scudo source changes.
The existing milestone tag and LLVM checkout are unchanged.
