# Kernel alignment status

Target inspected on 2026-10-15:

- kernel tree: `/home/lrc/patent/kernel/kernel_platform/common`;
- branch/worktree: `dev`, including its current uncommitted UAPI changes;
- ownership UAPI: `include/uapi/linux/memory_delegation.h`;
- prctl UAPI: `PR_SET_MEMORY_DELEGATION_LOG` plus RESET_ARENA and DRAIN_LOG.

## What is aligned now

The standalone `kernel_bridge` matches the current ring field sizes, offsets,
opcodes and prctl flags. A ring is always bound to one `(mm, arena CPU)` and the
bridge rejects CPU IDs that cannot fit the kernel's `u8 src_cpu`. Ring-full
handling asks the kernel to drain before reporting `ENOSPC`. A successful
transition call means the new tail was synchronously consumed by the kernel.

`kernel_abi_test.cpp` is compiled against the live kernel UAPI by `build.sh`
when `KERNEL_TREE` is set. This catches layout or constant drift without copying
private kernel implementation structures into the backend.

## Blocking runtime mismatch

The current kernel assumes the old SharedArena layout when a process exits:

1. one shared VMA per arena;
2. a one-page arena header immediately before `arena_data_base`;
3. an intrusive free-block header stored in payload pages;
4. kernel-side insertion and merge into that userspace freelist.

The unified backend intentionally has a different contract:

1. reusable-run descriptors and queues live in a separate shared metadata map;
2. payload contains allocator data only;
3. descriptor generation and release tickets are authoritative;
4. a single payload shm object is currently mapped as one contiguous window.

Registering the current payload directly would therefore fail for multiple
arenas (one wrapped VMA cannot carry several arena identities). More seriously,
the kernel's exit recovery would write the legacy freelist format into payload
while the backend descriptor still says `IN_USE`, causing leaked ownership and
two conflicting sources of truth.

## Recommended next protocol

Keep the unified descriptor backend authoritative and evolve the kernel ABI,
rather than reintroducing the legacy payload freelist into every allocator:

1. map each CPU arena as a distinct shared VMA/backing fd;
2. add an `EXTERNAL_BACKEND` registration flag so `mm_release` does not write
   the legacy freelist;
3. add a kernel-to-backend owner-death/reclaim queue carrying arena, page range,
   and ownership generation;
4. let the backend validate the generation, retire the dead lease, and publish
   through its normal descriptor state machine;
5. acknowledge reclaim completion before the range can be granted again.

An alternative is to change the backend back to the legacy header/freelist
layout. That is simpler for the current kernel but discards the main reason for
the refactor: allocator-independent metadata, release tickets and descriptor
generation. This repository does not take that alternative automatically.

## Other current limits

- The existing Android broker only distributes old per-CPU payload fds; it
  does not distribute the unified metadata object or negotiate a protocol
  version.
- There is no unregister prctl. Once a ring is registered, its page must remain
  valid until the mm exits; `md_kernel_bridge_close` therefore drains but leaves
  the ring mapping alive.
- Fork refresh and broker protocol versioning still need explicit design before
  Android integration.
