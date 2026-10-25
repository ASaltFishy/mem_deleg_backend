# Post-milestone capacity/performance follow-up

This is AFTER `md-milestone-20261025`; that annotated tag is not moved.

## Implemented

- Every quick/full/thread case has an index/total progress display and an
  explicit PASS/FAIL marker; the CSV parser validates completed sequences.
- Arena default payload is 256 MiB per CPU. Pool acquisition accounts for
  requested extent bytes, headers/alignment and existing live/free layout.
  Valid requests that exceed capacity return ENOMEM, as do smaller requests
  when a live block fills the pool. Adapters use existing native fallback;
  there is no benchmark-side 256 MiB switch or forced baseline data rewrite.
- PERF_MINIMAL=1 compiles out ordinary backend diagnostic counter writes,
  lock wait timing, profiling and slow/touch trace, and extra Scudo Secondary
  statistics. Metadata identity/lease transitions, error receipts, explicit
  zeroing, actual-route counts and worker context switches remain intact.
- When available, the phone script disables kernel cycle stats temporarily and
  restores the saved debugfs setting on exit. It reports unavailable otherwise.
- Domain/path namespace contains c256m and the runner checks compiled Arena
  capacity and performance mode before running. A phone with registered old
  2 GiB arenas must be manually rebooted before the new smaller backing is used.
  No automatic reset, reboot or flash was performed.

## Completed checks

- Android Scudo/jemalloc/broker built successfully with PERF_MINIMAL=1,
  native ELF TLS, ThinLTO, NDK r26/API 34; build identity is in the manifest.
- New backend_capacity_policy_test compiled with -fno-exceptions and passed
  locally on Ubuntu in both PERF_MINIMAL=0 and PERF_MINIMAL=1. It checks valid
  over-capacity ENOMEM, malformed-alignment EINVAL, live-block exhaustion,
  reuse after free, preserved MD_ZERO and disabled observation counters.
- Python AST and shell syntax checks passed.
- Dry-run plan: exactly 600 consecutive cases for 16 KiB..256 MiB, touch=1,
  1/2/4/8 threads and five repeats. No phone benchmark was executed. The earlier
  MAX_EXP=17 limit still selects 560 cases through 128 MiB.
- Full Linux allocator build, QEMU, kernel build and new phone correctness/
  performance runs were NOT performed. Existing milestone phone timings cannot
  be reused as measurements of the new capacity/performance configuration.
