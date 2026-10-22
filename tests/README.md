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
python3 tests/run_allocator_latency_bench.py \
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
jemalloc binary must link `src/jemalloc_adapter.cpp`, this backend, and the patched
jemalloc PAC implementation that immediately publishes delegated extents.


## Android asynchronous backend (2026-10-21)

Build both reviewed Android executables from the current backend:

```sh
tests/build_android_latency_bench.sh
```

The helper uses NDK r26, Android API 34 and the existing patched Scudo/jemalloc
source snapshots; override ANDROID_NDK_ROOT, SCUDO_SOURCE or JEMALLOC_SOURCE if
those dependencies move. Source staging does not overwrite the original LLVM
snapshot. The binaries and SHA-256/source manifest are under build/.

Run on the server after the phone boots the matching asynchronous kernel:

```sh
export ADB_SERVER_SOCKET=tcp:127.0.0.1:15037
bash tests/run_phone_latency_suite.sh --cpu-list 0
```

Defaults: 128 KiB/256 KiB/1 MiB; 5 warmup rounds per worker; 10 measured
rounds; 2 repetitions with no extra discarded pre-run. No automatic retry. Override
SIZES, WARMUP, ITERATIONS, REPETITIONS or pass runner flags explicitly.

Sample buffers are allocated and touched before warmup. Thread workers stop at
shared pre/post barriers; one thread checkpoints all rings of their mm. Process
workers use Ready/Drain/Start/Done/Finish messages so every mm checkpoints after
all warmups and after all measurements. The measured alloc/free loop remains
asynchronous. Checkpoint drains ownership logs and reaps completed FREEs, without
purging caches or resetting arena truth. Lifecycle markers confirm success.

The runner checks boot_completed, binary protocol and a small real-kernel probe
before starting a kernel matrix; older kernels fail before the full matrix.
Each attempt has its own domain, including touch mode and attempt ID. Normal
completion cleans its backing on host and Android; a signal/timeout stops the
whole matrix and preserves backing for inspection instead of resetting over
possibly live workers. A local file lock prevents overlapping runners on the
same checkout. Do not run another allocator/reset workload concurrently.

Raw logs contain per-worker summed minor/major faults and context switches.
Environment sidecars record before/after boot ID, load, temperatures and CPU
frequency/governor; config.json and build_manifest.json record provenance.
The default 0.5-second cooldown is adjustable. Measurements do not discard slow
samples, force real-time scheduling, or disable the scheduler batching mechanism.

Regression commands:

```sh
python3 tests/test_latency_runner.py
python3 tests/test_bench_lifecycle.py
KERNEL_TREE=/home/lrc/patent/kernel/kernel_platform/common ./build.sh
```

The control-flow tests do not constitute Android timing validation. Final real
phone latency results must be collected after flashing/booting the new kernel.


### Android native TLS requirement

Scudo allocator runtime and backend objects must be compiled with
`-fno-emulated-tls` (Android API >= 29). Emulated TLS calls malloc to create
thread storage; a malloc implementation using that TLS recurses before main.
The Android builder enforces native TLS and rejects runtime objects referencing
`__emutls_get_address`. Prebuilt libc++ exception helpers are outside this check.

A phone with legacy md-camera-scudo enabled can already have a CPU arena
registered at 512 MiB minus a header page. The new backend uses a full 512 MiB payload and
cannot register against that persistent kernel metadata. Disable the legacy
camera probe module and reboot before the isolated kernel matrix. Removing
userspace backing files alone does not resize an existing kernel arena.
Do not reset live system delegation ownership to force the benchmark through.

### 一键准备 Android latency 环境

在连接手机 ADB 转发的 Ubuntu 上执行：

```sh
export ADB_SERVER_SOCKET=tcp:127.0.0.1:15037
bash tests/pretest_phone_latency.sh --cpu-list 0
bash tests/run_phone_latency_suite.sh --cpu-list 0
```

pretest 默认停用 `md-camera-scudo` 和 `md-system-scudo` 两个旧委托测试模块；
如果它们仍启用，会重启手机，确认 boot ID 已变化且 boot_completed=1 后再继续。
模块会保持停用，不会自动恢复；其他模块不受影响。该脚本不刷机。
使用 `--check-only` 不修改模块、不重启，但仍部署二进制并执行小型探测。
需要先建立 ADB 转发、构建好 Android benchmark；ADB 不通或二进制缺失会报错退出。

脚本复用矩阵的启动/异步内核检查，并在每个指定 CPU 上验证两个 allocator
的 process 模式：5 次预热、1 次测量、payload 读写和前后 checkpoint/drain。
只有全部通过才输出 `[ready]`。它清理自己的探测 backing，不清理其他运行的日志，
不 RESET 全局 Arena，也不强制清缓存或锁频。矩阵每组原有预热和 drain 仍会执行。
同 checkout 的准备/矩阵共用锁；测试时不要运行其他 checkout 的委托工作负载。

控制流程回归（模拟 ADB，不修改手机）：

```sh
python3 tests/test_phone_pretest.py
```

### Scudo 并发初始化修复（2026-10-22）

Android builder 在 staged scudo_delegation.cpp 中修正 getBackend：
InitState=1 表示另一线程正在打开后端，此时调用 initialize 等待发布最终状态，
避免误判为 unavailable 而返回普通分配。原始 snapshot 保持不变，重建会自动应用修复；
遇到未知的初始化实现会停止构建，要求复核补丁。

pretest 现增加每个 CPU 上 thread×4、thread×8 探测，保留 process×1，
均验证委托指针、payload 读写和前后 checkpoint。runner 在失败时直接显示 stderr
和完整失败日志路径，避免把 ENOTSUP 的系统描述误认为 ADB 连接故障。


### 快速配置与大块异步回收（2026-10-22）

默认 warmup=5、iterations=10、repetitions=2、cold-runs=0；
每组总共执行两遍，各自预热5次后测量10次。phone suite、通用 runner、
benchmark 二进制默认值和 pretest 预热次数已同步更新。

在此前 128 MiB 配置下，1 MiB 等大块可在日志环满之前耗尽 Arena，因为尚未确认的 FREE
不能发布复用。md_acquire_flags 现在只在 ENOMEM/ENOSPC 且当前 mm/arena
通道仍有未消费日志时，离开 metadata 锁后执行一次 pressure drain，再重试一次。
正常 ALLOC/FREE 保持异步，不能确认的其他进程 FREE 仍不复用；真实容量不足
仍返回 ENOMEM/ENOSPC，drain 错误原样传播。

Scudo 回退时会打印实际后端错误码和请求大小，便于区分
ENOMEM 与最终基准检测出的 ENOTSUP。


### 每核完整 512 MiB（2026-10-22）

Scudo/jemalloc 使用 `delegation_backend.h` 中统一的
`MD_DEFAULT_ARENA_CAPACITY`（512 MiB），共享元数据和 guard page 不占用这512 MiB。
runner 的容量检查同步为512 MiB，jemalloc 的页面索引窗口从后端实际布局计算。
内核注册上限已扩展到完整512 MiB；旧 Scudo 的512 MiB减一页布局保持兼容。
需要编译并刷入此次新内核，再重启清除旧128 MiB内核元数据，然后运行pretest。
只删除共享文件或运行RESET不能调整已注册的 nr_pages。
预热5次、测量10次、每组2遍的默认配置保持不变。

延迟 microbench 的 kernel 模式要求运行支持完整 512 MiB arena 的新内核；
运行完整矩阵前，phone pretest 会先验证实际设备 ABI 和分配路径。
