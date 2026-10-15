# Kernel alignment status

Target inspected and aligned on 2026-10-15:

- kernel tree: `/home/lrc/patent/kernel/kernel_platform/common`;
- kernel branch/worktree: `dev`, preserving its existing uncommitted work;
- backend tree: `/home/lrc/patent/memory-delegation-backend`;
- ownership UAPI: `include/uapi/linux/memory_delegation.h`;
- prctl UAPI: `PR_SET_MEMORY_DELEGATION_LOG` with RESET_ARENA,
  DRAIN_LOG and EXTERNAL_BACKEND flags.

## Implemented alignment

The backend now opens one ownership-log bridge for every CPU arena. Successful
acquires are recorded in the kernel before payload initialization, and releases
are recorded before the descriptor is published as reusable. If either kernel
transition fails, the backend does not publish the affected run. Forked
children lazily register fresh per-mm rings before their first transition.

Every arena is mapped as its own shared VMA. One unmapped guard page separates
adjacent arenas, which prevents Linux from merging the mappings and lets the
kernel attach an independent ownership fault wrapper to each arena. The guard
is virtual address space only; it does not reserve physical payload memory and
is not a private allocation cache. Callers must use `md_get_arena_layout()`
rather than deriving arena addresses as `base + arena * capacity`.

`PR_MEMORY_DELEGATION_F_EXTERNAL_BACKEND` tells the kernel that reusable-run
metadata is stored outside payload pages. Normal FREE records therefore do not
prefault or write an intrusive payload freelist header.

## Deliberately incomplete owner-death behavior

The future broker/reclaimer protocol is intentionally not implemented yet.
When an externally-backed process exits, the kernel now:

1. drains its final ownership logs;
2. removes the dead `mm` from the owner table;
3. marks its remaining pages with the internal `RECLAIM_PENDING` sentinel;
4. revokes access and refuses new ALLOC records for those pages.

The userspace descriptor remains `IN_USE`, so the memory is safely quarantined
instead of being inserted into either the legacy freelist or the new global
free index. This leaks capacity after an abnormal process death until the
planned kernel-to-broker reclaim protocol is added, but it cannot create a
double allocation.

## Current runtime policy

Kernel integration defaults to optional so the prototype still runs on an
unpatched development host. Set `MD_KERNEL_BRIDGE=required` to fail backend
initialization unless the aligned kernel ABI is active, or
`MD_KERNEL_BRIDGE=off` to disable registration explicitly.

The bridge currently drains each transition synchronously. This makes the
first aligned version easy to validate and prevents cross-process handoff from
overtaking kernel ownership. Restoring context-switch batching without losing
handoff ordering is a separate performance change and is not assumed here.

## Deferred decisions

- kernel-to-broker reclaim ring, generation acknowledgement and final publish;
- Broker permission/audit enforcement for hostile metadata writers;
- asynchronous batching policy for normal ownership transitions;
- unregister ABI for closing and reopening a backend in the same `mm`;
- Android broker distribution and protocol version negotiation.
