# Allocator latency benchmark

`run_allocator_latency_bench.py` drives the same isolated allocation-latency
matrix for Scudo and jemalloc. `allocator_latency_bench.cpp` is compiled twice,
once with `MD_BENCH_SCUDO` and once with `MD_BENCH_JEMALLOC`, so both allocator
figures use the same clock boundaries and workload.

The runner does not build or flash anything. It expects two reviewed benchmark
binaries and can deploy them to Android unless `--skip-deploy` is used.

Key matrix options:

- `--execution-modes userspace,kernel`: selects `MD_KERNEL_BRIDGE=off|required`;
- `--touch-page-modes 0,1`;
- `--modes thread,process` (`process` means synchronized forked worker processes);
- `--core-counts 1,2,4,8`;
- `--workers-per-core 1,2,4`: threads per core in thread mode and processes per
  core in process mode. `--core-counts 1 --workers-per-core 4` is the historical
  same-core contention shape;
- `--cpu-list`: optional explicit physical-core representatives. Otherwise one
  logical CPU per `(package_id, core_id)` is selected from sysfs;
- `--dry-run`: print the complete matrix without deployment or execution.

Each size/path/repetition runs in a fresh top-level process. The C++ benchmark
prints the historical `alloc_only`, `hold_only`, `dealloc_only`, and `all`
metric format. The runner saves raw logs, `raw_results.csv`, `summary.csv`, and
exactly one consolidated latency figure per selected allocator:

```text
results/<timestamp>/scudo_latency.png
results/<timestamp>/jemalloc_latency.png
```

Example (after both Android binaries have been built):

```sh
python3 test/run_allocator_latency_bench.py \
  --target android \
  --scudo-binary build/allocator_latency_scudo.aarch64 \
  --jemalloc-binary build/allocator_latency_jemalloc.aarch64 \
  --execution-modes userspace,kernel \
  --touch-page-modes 0,1 \
  --modes thread,process \
  --core-counts 1,2,4,8 \
  --workers-per-core 1,2 \
  --sizes 65536,131072,262144,1048576
```

The Scudo binary must link the Scudo adapter backed by this repository. The
jemalloc binary must link `jemalloc_adapter.cpp`, this backend, and the patched
jemalloc PAC implementation that immediately publishes delegated extents.
