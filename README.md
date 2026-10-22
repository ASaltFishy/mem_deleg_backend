# Unified memory-delegation backend

This repository contains the allocator-independent delegation backend migrated
from the local `delegation-codebase` prototype. It deliberately does not carry
the historical Scudo source snapshot or its result archives.

The backend implementation is in `src/delegation_backend.cpp`, with the kernel
log bridge in `src/kernel_bridge.cpp`. Public interfaces remain in the
repository root headers. The backend provides fixed-address, per-CPU shared
arenas, generation-checked leases, descriptor recycling, split/merge,
remote-free mailboxes, and the rseq lock-entry fast path.
`src/jemalloc_adapter.cpp` is the current non-Scudo adapter example.

## Build and latency benchmark checks

```sh
KERNEL_TREE=/home/lrc/patent/kernel/kernel_platform/common ./build.sh
```

This compiles the backend and kernel bridge, checks the kernel UAPI when
`KERNEL_TREE` is set, and runs the latency benchmark runner's control-flow tests.
Build the Android microbenchmark binaries separately with
`bash tests/build_android_latency_bench.sh`.

## Kernel bridge status

`kernel_bridge.*` is wired into acquire/release using version-1 ownership logs
and `PR_SET_MEMORY_DELEGATION_LOG = 74`. Registration requires the external
backend and ASYNC_EXTERNAL capability flags. Normal ALLOC and FREE only append
logs; the kernel consumes every registered asynchronous ring of the outgoing mm
at a context switch, including owner-arena logs produced by remote frees.

FREE descriptors stay Pending until another participant observes the kernel's
release-published shared ring head passing the FREE ticket, with no channel
error. Only then can a block enter the reusable index. An exhausted ring yields
before falling back to a batch drain; close also drains. Neither path introduces
an unconditional syscall for each allocation or free.

Set `MD_KERNEL_BRIDGE=required` for real-kernel tests. Optional mode falls back
on kernels without the new capability. Metadata is version 5; version-4 domains
must not be reused. There are 128 lifetime log channels per arena, deliberately
not recycled without an unregister/reclaim protocol. See `KERNEL_ALIGNMENT.md`
for validation and remaining lifecycle limits.

## Repository boundary

Included:

- unified backend implementation under `src/` and public C API headers;
- latency microbenchmark, build/runner/phone-preflight tools, and their
  control-flow regression tests;
- jemalloc adapter sources;
- kernel UAPI snapshot and bridge implementation.

Excluded:

- the old LLVM/Scudo source snapshot;
- Scudo-only sweep scripts;
- bundled jemalloc source and generated build trees;
- historical benchmark reports and binaries.
