# Unified memory-delegation backend

This repository contains the allocator-independent delegation backend migrated
from the local `delegation-codebase` prototype. It deliberately does not carry
the historical Scudo source snapshot or its result archives.

The backend provides fixed-address, per-CPU shared arenas, generation-checked
leases, descriptor recycling, split/merge, remote-free mailboxes, and the rseq
lock-entry fast path. `jemalloc_adapter.*` is retained as the current non-Scudo
adapter example.

## Build and smoke test

```sh
KERNEL_TREE=/home/lrc/patent/kernel/kernel_platform/common ./build.sh
```

`KERNEL_TREE` enables a compile-time ABI check against the current kernel
worktree. Core allocator tests do not require the patched kernel.

## Kernel bridge status

`kernel_bridge.*` implements the current ownership-log ABI:

- `PR_SET_MEMORY_DELEGATION_LOG = 74`;
- reset and explicit-drain flags;
- the version-1 one-page `md_log_ring` layout;
- single-arena rings whose entries always use the registered arena CPU;
- synchronous drain after an ownership transition.

The bridge is not yet wired into `md_acquire`/`md_release`. That is intentional:
the kernel's process-exit path still mutates the old payload-resident intrusive
freelist, while this backend keeps authoritative descriptors in a separate
mapping. See `KERNEL_ALIGNMENT.md` before enabling production integration.
`md_get_arena_layout()` exposes the exact per-arena address and page count the
bridge will need once the VMA and owner-death protocol is selected.

## Repository boundary

Included:

- unified backend and public C API;
- correctness, concurrency, handoff, rseq and first-open tests;
- structure/contention benchmarks;
- jemalloc adapter sources;
- kernel UAPI snapshot, bridge and ABI test.

Excluded:

- the old LLVM/Scudo source snapshot;
- Scudo-only sweep scripts;
- bundled jemalloc source and generated build trees;
- historical benchmark reports and binaries.
