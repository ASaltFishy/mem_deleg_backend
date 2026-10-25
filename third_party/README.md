# jemalloc source provenance

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
