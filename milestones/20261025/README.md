# Memory delegation milestone 2026-10-25

Annotated tag: `md-milestone-20261025` in the kernel, LLVM/Scudo and Backend
repositories. This is a research milestone, not a general correctness-certified
production release. Cross-repository source hashes are recorded in manifest.json.

## Preserved state

- Shared rings ABI v3, broker v8, fixed VA, 2 GiB payload capacity per CPU Arena.
- Current Scudo Secondary integration and jemalloc 5.3.0 PAC/extent integration.
- mmap-backed benchmark statistics (sampling/aggregation do not use the tested
  allocator), wall-clock timing, warmup/measurement checkpoints, route and switch
  counts, balanced Scudo case ordering, jemalloc thread matrix and four metrics.
- Independent scheduler/fault/PTE diagnostic scripts and their source tests.
- Upstream jemalloc archive + exact PAC source patch, removing reliance on /tmp.

## Existing phone evidence

- Scudo: 675 successful cases, 16 KiB..256 MiB, 1/2/4 same-core processes, three
  configurations, five repeats. Actual routes distinguish Primary/cache/delegation.
- jemalloc: 560 successful cases, 16 KiB..128 MiB, touch=1, 1/2/4/8 same-core
  threads, traditional/delegated configuration, five repeats.
- jemalloc below 128 KiB uses ordinary allocations; 128 KiB and above in this
  sweep uses the delegation backend with no measured fallback. Corrected parser
  distinguishes fallback from allow_fallback; latency values are unchanged.
- Native jemalloc oversize threshold is 8 MiB. Traditional free eagerly purges
  above this threshold; subsequent touching faults pages back in. Compare memory
  residency alongside latency; alloc_only excludes touching and final checkpoint.

## Validation limits and known issues

- Device uname reports g8fe53571d89b; source HEAD before this marker is e1a5cdc9782b.
  A version string alone does not prove which uncommitted changes an image used.
  The correspondence between the running image and current source is unverified.
  The tag records current source, not a newly rebuilt/flashed kernel image.
- Stale session records can stop the normal ring across process exit. mmap-backed
  harness buffers remove a triggering benchmark lifecycle error; generic kernel
  exit/pending-log correctness still needs dedicated regression coverage.
- Existing latency sweeps do not prove cross-process revocation/data isolation.
- Android benchmark build passed for the captured manifest. Linux/QEMU were not
  rebuilt or rerun at this snapshot; SampleAllocator exception handling needs
  compatibility with the existing Linux -fno-exceptions build flag.
- Removed an isolated uncommitted backtick syntax typo in the kernel header;
  original was backed up. The source-tree Scudo header forwards to the maintained
  Backend adapter header, matching the Android staging build's declaration source.

The next requested changes (progress counters, unified performance switch,
256 MiB Arena and capacity-driven fallback to the original allocator path) are deliberately AFTER
this tag and must not be described as part of the tested 2 GiB milestone.
