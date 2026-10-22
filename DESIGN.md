# Unified allocator delegation backend (protocol v3)

## Boundary

The backend owns per-CPU arenas in a fixed-address shared payload window and
all reusable-run metadata. Allocators own object layout, safety checks,
statistics, and small-object caches. Allocator integration delegates large runs;
the backend rounds requests to pages and also supports single-page tests.

Metadata is never stored in payload. A cached allocator entry is represented by
`{block_id, generation}` and is only a candidate. It must win an exclusive
claim before allocator headers or payload are accessed.

## Default delegation policy

Delegation is enabled for testing without a runtime pressure controller.
For jemalloc, call `md_jemalloc_init(domain, &arena)` once before allocation
workers start, then route payload allocations through
`mallocx(... MALLOCX_ARENA(arena) | MALLOCX_TCACHE_NONE ...)`.
The adapter opens the backend during initialization; qualifying large extents
use delegation immediately. Ordinary allocation remains the fallback.

Scudo enables the delegation backend by default and opens it on first use.
`SCUDO_DELEGATION_BACKEND=0` selects the ordinary path for baseline tests;
`SCUDO_DELEGATION_BACKEND=1` explicitly selects delegation. Configure the
process environment before allocator initialization. For this unified backend,
set `SCUDO_SHARED_ARENA_DISABLE=1` to disable the separate historical SharedArena
experiment. Failed backend initialization or allocation falls back to ordinary
allocation.

Opening the backend reserves its fixed-address window without preallocating
payload runs or touching payload pages. Ordinary allocator caches keep their
normal behavior. There are no pressure transition APIs or pressure-triggered
cache reclamation in this integration.

## CPU and capacity policy

The intended deployment uses one runnable hardware context per physical core:
SMT siblings are not part of the performance model. Arena IDs are Linux logical
CPU IDs. Tests select one allowed logical CPU per `(socket, core)` pair; the
implementation does not disable SMT or alter machine scheduling globally.

Normal acquire searches only the arena selected for the current CPU. There is
no cross-arena stealing. A migration after lock acquisition is safe because
the lock stays held; it does not change the selected lease's home arena.
Remote frees return to the original arena, not the current CPU's arena.

`md_config.capacity` now means capacity PER ARENA. The window is
`[fixed_address, fixed_address + capacity * arena_count)`. There are
`_SC_NPROCESSORS_CONF` arenas (up to 1024); CPU IDs outside that range are rejected.
The adapters use 512 MiB per arena. This increases reserved VA/file size, not
eager payload commitment; metadata descriptors are initialized lazily. Free
capacity on an idle CPU is not usable by another CPU. This intentional static
partitioning can strand capacity and is not a memory-balancing solution.

Block IDs encode `arena_id * 8192 + slot`. The lease ABI otherwise stays the
same. Version 3 domains cannot attach to version 2 metadata. Fresh domains are
required after upgrade; all participants must agree on CPU count and capacity.

## Descriptor and address indexes

Each arena owns up to 8192 descriptors. Allocation first pops a recycled slot
from `DescriptorHead`; if empty it consumes `NextUnused`. Both are O(1), with
no scan over active descriptors. Retirement increments generation and pushes
the slot back. A failed split returns any reserved prefix slot before exiting.
Descriptor exhaustion returns ENOSPC even if payload capacity remains.

Free runs participate in an address-ordered doubly linked list and 24 size bins.
Bin search remains linear within eligible bins. New untouched space uses bump
allocation. Split keeps prefixes/suffixes free; only address-adjacent FREE runs
inside the SAME arena may merge.

`InsertHint` is the most recently inserted descriptor. If its start is before
the incoming run, address search begins at its successor; otherwise it begins
at the head. Unlinking the hint selects its predecessor, else successor, else
invalid. Merge uses the same unlink helper before recycling descriptors, so a
hint never intentionally survives descriptor retirement. All maintenance is
under the arena lock. Ascending insertion benefits; reverse/random workloads
still have O(F) worst-case search. Hinting does not make the list a tree.

## Arena locks and rseq

One active thread per CPU does NOT make a multi-store list update atomic:
another thread can run while its predecessor is preempted. Each arena therefore
has its own process-shared futex lock. State 0 is unlocked, 1 is locked without
known waiters, and 2 is locked/contended. Uncontended acquisition uses CAS;
contenders exchange 2 and FUTEX_WAIT (not FUTEX_WAIT_PRIVATE). Unlock exchanges
0 and wakes a waiter if the previous state was 2. No infinite busy-wait on a
preempted same-CPU holder. There is no global allocation lock.

The x86-64/glibc rseq entry path uses the libc-registered rseq TLS area. It checks
the selected CPU and commits exactly ONE shared write: the lock CAS. The
post-commit label immediately follows cmpxchg. All prior instructions only
read memory or update registers, so abort requires no freelist rollback. The
kernel's signal/preemption/migration abort returns to a fallback path which
locks and rechecks CPU selection, retrying selection if needed.

This is a CPU-validated LOCK-ENTRY fast path, NOT a lock-free allocator or a
lock-free per-CPU object cache. Split/merge, bins, descriptor allocation, and
local publication remain under the lock. It intentionally uses the same atomic
gate as non-rseq users, remote administrative drains, and migrated lock holders.
Treating a CPU-local store as exclusion against those paths would be incorrect.
Disabled/unavailable rseq falls back safely; no private second rseq registration
is attempted. Rseq currently applies to normal acquire, not release or candidate
acquire. Signal-handler reentrancy into this allocator is unsupported.

## Remote-free mailbox

Release claims `ReleaseTicket` with a CAS from the exact lease generation to 0.
That single-use ticket rejects old generations and double releases BEFORE they
can change a newer allocation's state. The claimant validates owner, address,
size, and generation. Invalid parameters restore the ticket. Once claimed,
only that release may modify the run; acquire publishes a new ticket last.

Local release publishes under the arena lock. Remote release never acquires
the target ARENA lock. It prepares provenance/optional clearing, sets PENDING,
and enqueues the descriptor as a message. Queue links live in metadata, not
payload; at most one message per live descriptor can exist. Thus there is no
separately bounded message ring that can fill before the descriptors do.

The MPSC mailbox is protected by its own process-shared pthread mutex. It is
NOT a lock-free queue: producers can contend with one another or a drainer,
but they do not walk the freelist, split, or merge. Empty-mailbox detection is
an acquire load and avoids locking on the ordinary no-message path. A drainer
holds the arena lock, briefly detaches the mailbox under its mutex, then releases
the mailbox mutex before publishing/merging the batch. Producers acquire only
the mailbox mutex, never arena-after-mailbox: there is no reverse lock order.
The current drainer consumes the entire detached batch (at most 8192 messages),
without a time budget; fragmented large batches can lengthen arena lock holds.

Acquire and local release drain pending messages. `md_drain_remote` and
`md_get_stats` may also drain administratively from another CPU under the same
arena lock. A dormant arena can retain PENDING blocks until such a drain;
remote publication is not synchronous. A remote release returns success but
an invalid candidate `{UINT32_MAX, 0}`. Local candidates remain speculative and
can become stale through split/merge. Candidate acquire refuses another CPU's
arena with EAGAIN; callers must retry normal local allocation, not assume reuse.

## State machine

`FREE -> CLAIMING -> IN_USE(owner, generation)` on acquisition and
`IN_USE -> CLAIMING -> FREE(new generation)` on local publication, and
`IN_USE -> CLAIMING -> PENDING -> FREE` on remote publication. Split, merge, and
descriptor retirement also advance generation so stale cache candidates fail.

The arena lock protects freelist topology, bins, split, and merge. Payload
clearing occurs while the descriptor is `CLAIMING`, outside the lock. This
keeps the arena critical section short while preventing another process from
acquiring a partially cleared run.

Free runs retain their last `(attachment session, owner PID)`. Ordinary malloc
reuse by that same owner does not require zeroing. Foreign-owner acquisition,
mixed-owner merged runs, a new attachment session, and explicit `MD_ZERO`
requests are cleared before `IN_USE` is published. Split propagates provenance;
merge treats already-zero runs as neutral and otherwise preserves equal
provenance or marks the result mixed. PID reuse therefore cannot bypass clearing.
`MD_EAGER_CLEAR=1` is a safe experimental control that clears on release instead.

The entire returned lease is readable and writable. The backend does not
reserve guard pages, call `mprotect`, or track process-local protection bits.
The allocator adapter requests enough bytes for its headers and alignment,
then lays out its objects within the returned pages. Any future allocator-local
protection policy must restore the complete lease to readable/writable before
publication, so backend zeroing and subsequent reuse remain valid.

Generation and statistics counters remain atomic for readers, but increments
under the arena lock use load/store instead of redundant atomic RMW.
PID caching is reset in the child by an atfork handler. These changes preserve
exclusive claims and stale-generation rejection.

## Scudo adapter

Hook `MapAllocator::allocate()` before the private Secondary cache when the
delegation mode is active. On success, construct the existing LargeBlock header
inside the acquired run and record the backend lease in the header. Hook
`MapAllocator::deallocate()` after Scudo removes the block from `InUseBlocks`
and finishes its statistics/header work; publish the complete run instead of
placing it in `MapAllocatorCache`.

The delegated layout is `[alignment padding / headers / user area / slack]`
inside a single writable lease. `CommitBase = Lease.Base` and
`CommitSize = Lease.Size`; there is no extra two-page reservation. This
prototype's delegated path intentionally does not preserve Secondary boundary
guard-page detection. Scudo still owns its headers, alignment, fill policy,
checks, and accounting; guard pages are not part of the delegation contract.

Primary allocations and ordinary Secondary mappings are unchanged. A private
cache entry is never treated as permission to access a delegated run.

## jemalloc adapter

Use a dedicated arena with custom extent hooks plus the included PAC patch in
`third_party/jemalloc-5.3.0/src/pac.c`. Only payload requests of at least 128 KiB
are routed to the backend; arena metadata uses the original hooks. On freeing a
delegated extent, the PAC patch removes jemalloc's extent registration and
publishes immediately instead of entering the dirty/muzzy cache. This path does
not depend on decay being zero. The adapter indexes leases by start page for
constant-time lookup rather than scanning all descriptors. Zero requests are
forwarded explicitly; ordinary reused malloc extents are not reported as zero.
Disable tcache for benchmarked large allocations.

The adapter's lease table covers the complete multi-arena window and is keyed
by start page, not a globally fixed 8192-entry slot table. Exclusive extent
ownership protects each entry; deallocation clears it BEFORE backend publication.
No single adapter-wide spin lock remains. This does not remove jemalloc's own
internal arena locking; backend scaling numbers are not jemalloc scaling claims.

This prototype uses explicit `mallocx(... MALLOCX_ARENA ...)` routing. Making
all ordinary `malloc` calls automatically select the delegation arena requires
an additional policy patch in jemalloc arena selection and is intentionally
separate from the ownership backend.

## User-space limitation

The state machine prevents cooperative allocators from double-acquiring a run,
but user space cannot revoke an old process's PTEs. Kernel ownership and TLB
synchronization remain required before this can provide hostile-process or
use-after-transfer isolation.

This prototype targets Linux x86-64/glibc with 4 KiB pages. Process death while
holding either lock or after claiming a release ticket is not recovered. CPU
hotplug, dynamically changing topology, whole-window remapping, fork from a
multithreaded process holding backend locks, and signal-handler allocator calls
are unsupported. These are prototype limitations, not production guarantees.

## Measurement and tests

Counters distinguish arena lock acquisitions/contentions/wait nanoseconds,
mailbox contentions/wait nanoseconds, enqueue/drain totals, successful rseq
commits/aborts, descriptor pops, hint hits, and address search steps. The abort
counter counts kernel rseq redirects, not a plain CPU-comparison miss. Wait clocks
run only on contended paths. `md_get_stats` drains and samples each arena in
turn; it is NOT a globally atomic snapshot. Remote release call/success totals
are counted on drain. CLAIMING/PENDING space is conservatively live until drain.

`MD_SINGLE_ARENA=1` selects a shared-arena contention CONTROL, incompatible with
normal domains. `MD_RSEQ=0` disables rseq entry. `MD_EAGER_CLEAR=1` restores eager
clearing. These options are fixed at attach; never mix incompatible topology
settings in one domain. `MD_TEST_RSEQ_DELAY` is a compile-only test hook adding
register-only work before commit to expose real kernel aborts; production
binaries and performance measurements do not define it.
`MD_TEST_PREEMPT_LOCK` sleeps after partially linking a node while the arena
lock is held. These are test-only hooks and are not used by the latency
microbenchmark.

The maintained performance test is the allocator latency microbenchmark under
`tests/`. It compares Scudo and jemalloc using matching allocation/free timing
boundaries; the scripts and options are documented in `tests/README.md`.
Historical reports describe earlier experiments and are not maintained test
entry points.

### Concurrent first-open initialization

`md_backend_open` now opens/creates only the metadata inode before acquiring
its exclusive flock (retrying EINTR). All payload opens, size checks, sizing,
mutex initialization, header publication, and session allocation are performed
under that same lock. The first lock holder that sees a zero-length metadata
inode initializes the domain; it need not be the process that created the shm
name. A delayed creator must attach to the already initialized domain, never
truncate or reinitialize it. Existing nonempty metadata is validated, not reset.
The shared format remains v3 and successful open's allocation/free hot paths
are unchanged.

Failed opens close both descriptors and unmap any temporary mappings, including
when fixed-address mapping collides. Failure does not unlink shared names:
other processes may already be waiting on the existing inode, so unlink/recreate
could split one logical domain into independent locking/ownership universes.
This is initialization serialization, not crash recovery. Failure after sizing
metadata but before publishing its header can leave a nonempty, invalid domain;
later attach rejects it. Only after all participants stop may it be removed.
External shm unlink/reset while participants are live remains unsupported.

Initialization is serialized under the shared metadata lock so a delayed
creator attaches to an already initialized domain instead of truncating it.


### Full 512 MiB external payload (2026-10-22)

`MD_DEFAULT_ARENA_CAPACITY` is 512 MiB for both allocator adapters. Metadata is
outside the payload, and one guard page separates adjacent CPU arenas. At 4 KiB
page size the payload has 131072 pages. jemalloc derives its lease table span
from the final arena layout and native page size, including guard holes.
The kernel registration limit now accepts a full 512 MiB external payload;
legacy in-band-header layouts remain 512 MiB minus one page. This requires the
updated kernel. A reboot must clear any per-CPU ownership metadata initialized
with the old 128 MiB size before the new benchmark can register.
