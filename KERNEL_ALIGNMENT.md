# Kernel alignment status

Target inspected and aligned on 2026-10-15:

- kernel tree: `/home/lrc/patent/kernel/kernel_platform/common`;
- kernel branch/worktree: `dev`, preserving its existing uncommitted work;
- backend tree: `/home/lrc/patent/memory-delegation-backend`;
- ownership UAPI: `include/uapi/linux/memory_delegation.h`;
- prctl UAPI: `PR_SET_MEMORY_DELEGATION_LOG` with RESET_ARENA,
  DRAIN_LOG, EXTERNAL_BACKEND and ASYNC_EXTERNAL flags.

## Implemented alignment

The backend opens one per-mm ownership-log bridge for every CPU arena.
ALLOC/FREE normally enqueue without an explicit drain. After releasing, the
shared descriptor stays Pending until the shared channel head confirms the FREE
has been consumed. Any attaching process can reap that confirmation before
acquiring, without another allocator call by the releasing process. Failed
channels remain poisoned and their pending blocks are not published. Forked
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

The 2026-10-21 update uses `PR_MEMORY_DELEGATION_F_ASYNC_EXTERNAL` for explicit
capability negotiation. An older kernel rejects the flag instead of silently
using its owner-CPU-only drain rule. At switch-out the kernel consumes all
registered asynchronous external rings of prev_mm; at switch-in it queues PTE
updates for changed asynchronous arenas. Legacy ring semantics are preserved.
A per-ctx irq-safe consumer lock prevents scheduler/overflow-drain duplication.

Rings reside in shared metadata so independent attaching processes can observe
completion. Metadata version is 5 and runtime pages up to 64 KiB are supported;
a v4 domain is rejected, not overwritten. Each arena reserves 128 lifetime
channels of 64 KiB virtual storage. Only the actual runtime page of a registered
channel is touched. Channels are not recycled because kernel pins and Pending
references outlive a userspace handle. Exhaustion returns ENOSPC. Optional
unsupported-kernel probes return their unused slot.

Only full-ring backpressure (yield, then drain if still full) and close retain
explicit drain. Normal allocation and both local/remote free are asynchronous.
Pending free may temporarily cause acquire to return ENOMEM until a scheduling
boundary. Kernel-backed release returns an invalid cache candidate; allocators
must retry through the backend after completion rather than reuse it directly.

## Deferred decisions

- kernel-to-broker reclaim ring, generation acknowledgement and final publish;
- Broker permission/audit enforcement for hostile metadata writers;
- unregister ABI for closing and reopening a backend in the same `mm`;
- Android broker distribution and protocol version negotiation.

## 2026-10-21 validation

- Host build/UAPI checks and existing smoke tests pass.
- Mock transport tests prove zero drain calls for normal ALLOC/local/remote FREE,
  producer serialization, full-ring yield/fallback, sticky errors and fork close.
- Two-process mock backend tests withhold completion: the full arena cannot be
  reacquired before acknowledgement, and another process can obtain the same
  zeroed address afterwards while the releaser makes no allocator calls.
- The arm64 kernel builds successfully. QEMU runs 10 repetitions each of backend
  local/remote asynchronous handoff, bridge context-switch/PTE isolation,
  owner-death quarantine/error propagation, and independent-exec isolation.
  All 40 invocations pass, without panic/oops/BUG/WARNING markers.
- No phone boot, flash, reboot or allocator replacement was performed this time.

The ABI still has no unregister. Do not close/reopen a kernel-enabled backend
inside the same mm; tests use fresh processes per scenario. The first QEMU
fixture hit this documented lifecycle limitation by reusing a registered ring
VA for different backing; separating the fixture's mm lifetimes fixed it.
No new throughput/latency claim is made; all-arena scans add scheduler work and
need workload measurement, especially with many configured CPUs. Active-mms'
remote PTE revocation retains the existing scheduling/task-work boundaries.

## Historical device validation (2026-10-18)

Validated on 2026-10-18 on a PLK110/OP60FFL1 test device using
`fastboot boot`, without writing any boot or DLKM partition. The active-kernel
test covers:

- normal external-backend ALLOC and FREE transitions;
- propagation of a rejected ALLOC as `EBUSY` to userspace;
- quarantine of a page still owned when its process exits;
- denial of a fault against the quarantined page;
- continued allocation and release of an unrelated page.

The test passed once interactively and then passed 100 consecutive stress
iterations. No memory-delegation-related warning, oops, panic or use-after-free
was observed. A pre-test SLUB warning came from the vendor
`oplus_bsp_schedinfo` tick hook, outside the memory-delegation path.

A separate same-CPU context-switch test uses two simultaneously live processes.
Both processes prefault the same two free pages to establish stale PTEs, append
ownership records without issuing an explicit DRAIN request, and then block on
pipes to force scheduler switches. It verified in both ownership directions
that switch-out drains the pending ALLOC, revokes the non-owner's stale PTE,
denies the non-owner fault, and preserves the owner's access. It also verified
that an asynchronously drained FREE makes the page faultable by the other
process again. The test passed once interactively and then passed 100
consecutive iterations. The denial diagnostics named two distinct live `mm`s
and the corresponding distinct owners; no memory-delegation error, oops, or
panic was emitted during the run.

An additional unrelated-process test starts two sibling workers through
separate `exec` calls, so neither worker is the other's parent and neither
inherits the other's address space or delegation context. A coordinator passes
the same memfd backing object to each worker independently using `SCM_RIGHTS`;
each worker maps it at the shared arena address and registers its own ring. The
same-CPU test verified asynchronous switch-out drain, stale-PTE revocation,
same-process owner access, bidirectional non-owner denial, and post-FREE
refault. It passed once interactively and then passed 100 consecutive
iterations. Kernel diagnostics show each direction using distinct worker PIDs
and `mm`s, with the peer `mm` recorded as owner.

The temporary kernel still exhibits the repository's previously documented
new-kernel/old-DLKM Wi-Fi symbol mismatch. No DLKM partition was changed for
this test. A normal reboot changed the boot ID, preserved slot `_a`, left the
`boot_a` SHA-256 unchanged, and the external-backend ABI probe returned to the
unsupported-kernel fallback, confirming rollback to the persistent kernel.

The device and QEMU checks above were performed with standalone validation
harnesses that are no longer retained in this repository. The maintained
measurement entry point is the allocator latency microbenchmark documented in
`tests/README.md`.
