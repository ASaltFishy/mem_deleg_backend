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

### 四进程大块容量与严格路线验证（2026-10-24）

当前默认已从上面的历史 512 MiB 配置调整为每 CPU Arena **2 GiB**，
broker、Scudo、jemalloc 与内核外部 Backend 上限一致。256 MiB 的 Scudo
请求含 Header/对齐实际需 268435600 字节，旧容量只够同时持有一个，
因此原 256 MiB×2/4 进程矩阵可能返回 ENOMEM 后走 secondary。
payload 仍按需分配，但内核页级元数据随容量增加。
必须刷入支持 2 GiB 的内核并重启，再使用新 backing，不能在同启动中替换旧 backing。

Ubuntu 上运行 `bash tests/run_phone_v3_tests.sh`：默认 128 KiB～256 MiB、
1/2/4 进程、216 组，`ALLOW_FALLBACK=0`。预热或测量期间一旦发生 fallback
即失败。通用 `run_v2_android.sh` 保留 `ALLOW_FALLBACK=1` 默认值，兼容
包含 primary 分配的旧矩阵；测纯委托性能应显式传 0 或使用上述严格入口。

新增 `scudo_capacity_test.cpp` 同时持有四个 128/256 MiB 块，读取后端失败
错误码并验证释放后的 checkpoint。构建脚本分别产出 Android/Linux 测试程序；
`RUN_CAPACITY=1` 启用 QEMU 容量回归，须先重建 Linux 二进制。
用 `--expect-exhaustion` 可验证旧 512 MiB 构建确实返回 ENOMEM。

### 慢样本与页表同步诊断（2026-10-25）

benchmark 现在输出 `max`，power CSV 保留 `alloc_max`，summary 保留重复中的
`max_ns`，旧日志没有 max 时留空。原有 p50/p95/p99 取样定义保持不变。

`MD_SLOW_TRACE=1 bash tests/build_android_latency_bench.sh` 编译逐次时间诊断版。
起止时间/PID 缓冲区预分配，所有 worker 完成后才输出 ALLOC_SAMPLE；
alloc 区间不新增 syscall 或打印。诊断构建在 manifest 中标记 slow_trace=true，
正式测时延前须重新使用默认 MD_SLOW_TRACE=0 构建。

将诊断二进制命名为 `/data/local/tmp/md-slow-tail/allocator_latency_scudo.trace.aarch64`，
并推送 `tests/phone/capture_alloc_tail.sh`，在 root 下运行该脚本可捕获 CPU0 的
sched/IRQ 和可选 kprobe 轨迹。需有本次启动的 v8 broker socket。
第一个参数为唯一 run 名，第二个参数 1 启用同步/撤销探针；REPEATS 控制重复数。
结果目录位于 `/data/local/tmp/md-slow-tail/<run名>/`。可用 FREQ 设置当前 policy
范围内的频率请求及 performance governor；Android 电源服务可能调整频率请求，
因此不应把它描述为严格硬件锁频。脚本结束恢复原调频配置、删除自建探针与 instance。

拉取结果后，运行 `python3 tests/analyze_alloc_tail.py <目录>` 可关联慢样本与
离 CPU 时间；`python3 tests/analyze_pte_sync_trace.py <目录>` 统计测量窗口内
同步次数、实际 zap 次数及扣除切出 CPU 后的同步墙钟耗时。后者仍包含 IRQ 和探针成本，
不是纯用户态指令 CPU 时间；有 overrun、dropped events 或 missed return probes 时
不能把不完整轨迹当完整归因。本轮数据上述丢失计数均为零。

### 触页长尾诊断（2026-10-25）

`MD_TOUCH_TRACE=1` 是默认关闭的诊断构建开关。它预分配逐次记录，在触页前后采集
`RUSAGE_THREAD` 和线程 CPU 时间，输出 `TOUCH_SAMPLE` 的单调时钟区间、CPU 时间、
minor/major fault 与上下文切换增量。`TOUCH_SAMPLE` 的 wall 区间只包围 touchPages；
诊断构建原有 `hold_only` 会额外包含这些探针，不能当作正式时延结果。所有样本在
worker 结束并 checkpoint 后输出，不在触页循环内打印。

构建脚本支持并将此开关纳入 build ID/manifest：

```sh
MD_TOUCH_TRACE=1 MD_SLOW_TRACE=1 bash tests/build_android_latency_bench.sh
```

本次实际诊断单独链接了已有对象，产物位于
`/home/lrc/patent/md-touch-diagnosis-20261025/allocator_latency_scudo.touch.aarch64`，
没有覆盖正式 `build/` 中的 benchmark。

将诊断二进制部署为 `/data/local/tmp/md-touch-bench.aarch64`，将
`tests/phone/capture_touch_tail.sh` 推送为 `/data/local/tmp/md-capture-touch.sh`，然后运行：

```sh
adb shell su -c 'sh /data/local/tmp/md-capture-touch.sh touch_light light 268435456 4 10 3'
```

参数为唯一名称、模式、请求字节数、同 CPU0 worker 数、每 worker 次数和重复次数。
模式 `none` 仅逐次计数；`light` 关联调度、IRQ、PTE 同步、zap、shmem 换入和回收；
`fault` 额外对 handle_mm_fault 入/返回逐次记录，扰动明显，只用于确认调用路径和
重试次数，不用于正式时延比较。每次 capture 使用独立 tracefs instance，只对该次
worker PID 启用动态 probe，并在退出时移除自己的 probe/instance。

可在启动 capture 时设置 `MD_TOUCH_MINCORE=1`，在触页前额外读取 file-backed 页的
驻留状态。驻留位不能说明 PTE 已存在；该探测本身也会改变时序。未启用时
`resident_pages=UINT64_MAX`，不能解释成实际驻留页数。探测缓冲使用独立匿名 mmap，
避免通过被测委托分配器额外持有活块。

脚本复用本次 boot_id 的常驻 v8 broker，不 reset backing、不停止 broker、不改频率。
输出在 `/data/local/tmp/md-touch-diagnosis/<名称>/`。拉取目录后执行：

```sh
python3 tests/analyze_touch_tail.py /path/to/capture
python3 tests/test_touch_trace.py
python3 tests/test_touch_analysis.py
```

分析器校验 trace 丢失及 kprobe miss，关联调度切出区间、其他 worker 的 CPU 占用，
按触页窗口和地址范围统计 fault/RETRY、PTE 同步和换入。返回值 RETRY 不计作第二个
minor fault；嵌套阶段不应简单相加。常规文本 trace 的时间戳精度为微秒，单次 fault
的亚微秒分位数不精确；重探针会明显放大总时延。


### 墙钟对照流程调整（2026-10-25，尚未构建/运行验证）

- 计时继续使用 steady_clock 墙钟。Scudo 预热实际全部为 Primary 时，跳过无关的测量前 backend checkpoint；若预热涉及 Secondary/委托则保留 checkpoint。测量结束仍保留原来的委托 checkpoint，覆盖初始化、样本缓冲区等其他后端活动的收尾。线程屏障汇总所有 worker 的需求，避免仅由最后到达者决定。无跳过 ownership 检查的委托快路径。
- power 矩阵每轮交错三种配置，按六种排列轮换，ORDER_OFFSET=0..5 可重复改变起始顺序。每组仍启动全新进程；broker/domain 保留。
- 新默认 WARMUP=10、ITERATIONS=30、REPETITIONS=5。power 的 16/32/64 KiB 使用至少 PRIMARY_WARMUP=1000 和 PRIMARY_ITERATIONS=10000；可用同名环境变量调整下限，实际次数记录在 POWER_CASE_BEGIN。大块不套用一万次下限。
- summary.csv 新增每轮 p50/p95/p99 的中位数、每轮均值的中位数，并保留墙钟均值、跨轮标准差及最大单次值。它们不是合并样本后的分位数。新增 scudo_alloc_only_p50.png/pdf；原均值图保留。raw_results.csv 保存上下文切换和缺页计数，不自动剔除长尾。
- 独立调度诊断入口 tests/phone/capture_alloc_sched.sh，要求 MD_SLOW_TRACE=1 构建的 benchmark，通过 BENCH 指定完整路径；VARIANT=cache|mmap|delegated、SIZE、WORKERS、ITERATIONS、WARMUP 可选。以 root 在手机执行，保存 sched/IRQ 与可用 mmap/munmap syscall 事件、逐次 ALLOC_SAMPLE、trace buffer stats；输出仅作诊断，不混入正式曲线。不自动改频率，不重启或清理 backing。
- 本次只修改源码，未构建、未运行单测/计划预演或设备测试。下一次运行前必须重建 Android benchmark；--build-info 应含 prepare_policy=primary_checkpoint_skip_v1，诊断程序还必须含 slow_trace=1。


### 采样缓冲区与停止日志诊断（2026-10-25）

真机只读现场：CPU 1 normal 日志 head=32/tail=48、state=STOPPED，首条 normal_error=-ESTALE(116)，记录为 ALLOC/session=210/allocation_id=17/start=0/nr_pages=20。再次注册时因日志已停止返回 EINVAL(22)，旧适配器曾将初始化错误覆盖为 ENODEV(19)。未重置或恢复该日志。

提高到 10000 次采样后，每个 u64 采样数组为 80000 字节，原 std::vector 会进入被测 Secondary/委托分配器。worker 数组在 post checkpoint 后析构，父进程的汇总和排序副本也会分配委托块；它们没有覆盖整个生命周期的 checkpoint，存在进程退出后残留过期 session 记录的漏洞。已改为 mmap/munmap 的独立 SampleStorage，覆盖采样、IPC 接收、父进程汇总和分位数排序副本。计时仍为墙钟，样本页仍在测量前初始化，不把统计内存分配计入被测路径。

md_scudo_checkpoint 在后端从未初始化时直接返回成功，不为纯 Primary 工作负载主动注册全部 Arena；已初始化或初始化失败的后端仍执行 checkpoint 或保留真实错误。原始初始化 errno 和 pre/post phase 诊断继续保留。

该改动仅构建通过，未运行真机 benchmark/回归，未修改内核。它隔离测试框架的无关分配，不代表内核退出会话与待处理日志的通用生命周期问题已通过验证。现有 STOPPED 日志不能通过换测试目录/重建用户态恢复；下次真实委托测试前需要用户手动重启手机，继续使用当前内核并由脚本按新 boot ID 部署。不要手工清零内核仍持有的 ring/backing。


### 四类时延图（2026-10-25）

power 入口和 quick/full 入口现在都在成功结束后自动生成 malloc、free、hold、all 四类折线图。每类提供均值/跨轮标准差带和每轮 p50 中位数两个版本，分别保存 PNG/PDF。jemalloc 按 mode、touch、cores、workers_per_core 分面，避免混合工作负载。p50 是每轮分位数的中位数，不是合并原始样本的分位数。

文件名为 allocator_malloc、allocator_free、allocator_hold、allocator_all，p50 版本追加 _p50。Scudo 保留原 scudo_alloc_only 文件名作为兼容副本。Scudo 原 summary.csv 仍为 malloc 汇总，四指标汇总另存 summary_all_metrics.csv；quick/full 的 summary.csv 包含 metric 列与四类指标。两种入口的原始 CSV 均保留上下文切换统计。

hold 为原 benchmark 的 touch/use 与辅助检查区间；all 为 malloc+hold+free 的墙钟测量区间，排除预热与 checkpoint。脚本只重新读取日志和绘图，无需重建二进制或重跑设备测试。

旧 quick/full 数据可用 python3 tests/summarize_v2_latency.py --log RESULTS/run.log --output-dir RESULTS 补画。旧 Scudo power 数据可用 python3 tests/plot_scudo_power.py --log RESULTS/run.log --output-dir RESULTS 补画。


### 里程碑之后：256 MiB 容量、进度与性能构建（2026-10-25）

`md-milestone-20261025` 保留此前 2 GiB 源码和既有真机结果，以下修改不属于该 tag 的验证结论。

- 每 CPU Arena 默认 payload 容量改为 256 MiB。没有人为的“请求 >=256 MiB 强制 baseline”分支；后端按照实际剩余空间、连续块、对齐/headers 和并发 live 占用判断。有效请求超过总容量或当前无法取得块时返回 ENOMEM，适配器继续走原分配器的缓存/mmap 等路径。低于 256 MiB 也可能因为空间不足回退。ALLOCATION 的 generation/session、日志提交、清零请求及仲裁检查不受性能开关影响。
- quick/full/threads 入口显示 [index/total] 和 workload 信息，并记录 PASS/FAIL。原始 CSV 保存 index/total 与完成标记，解析时验证序列完整。
- 构建和运行均指定 PERF_MINIMAL=1：编译移除 Backend 的普通性能计数更新、锁等待计时，以及 profile/slow/touch trace 和 Scudo Secondary 附加统计。保留真实路线计数、worker 上下文切换统计、故障回执及全部必要安全检查。路线分类仍位于 alloc 计时区间之后，属于 hold 的辅助工作；因此并非声称测量完全没有观测开销。PERF_MINIMAL=0 保留诊断计数；两类构建的标签与 manifest 分开记录，runner 拒绝参数与二进制不匹配。
- 性能运行时若 debugfs 的 switch_cycle_stats_enabled 可用，会暂时关闭内核周期统计并在脚本退出时恢复；无节点则如实报告 unavailable，不声称已更改内核配置。
- 使用独立 c256m domain/每 boot 目录，禁止在已注册的旧 2 GiB backing 上原地 reset。已有 2 GiB Arena 的手机需要用户先手动重启一次，再运行新配置；当前支持 2 GiB 上限的 ABI v3 内核可注册较小的 256 MiB Arena，无需仅为此重新刷机。
- 图表保留真实 fallback 比例，不把回退数据强制替换成 baseline 数值。内存驻留、缓存状态和调度均可能使相同原始路径的实测值不同。

构建示例：`PERF_MINIMAL=1 bash tests/build_android_latency_bench.sh`。
运行示例：`PERF_MINIMAL=1 ALLOCATORS=jemalloc MATRIX=threads SIZE_SWEEP=power THREAD_COUNTS=1,2,4,8 MIN_EXP=4 MAX_EXP=18 ALLOW_FALLBACK=1 CPUS=0 WARMUP=10 ITERATIONS=30 REPETITIONS=5 PRIMARY_WARMUP=1000 PRIMARY_ITERATIONS=10000 bash tests/run_phone_v3_tests.sh`。
MAX_EXP=18 包含 256 MiB 观察点；17 保留之前 128 MiB 上限。两者都不重写分配路径。


### 单独扫描多进程模式

`MATRIX=processes` 只运行 `--mode process`（fork worker），不混入线程模式。
`PROCESS_COUNTS=1,2,4,8` 指定每个 CPU 的进程 worker 数（默认 1、2、4、8）；
worker 总数为 CPU 数乘以每 CPU worker 数，不含 benchmark 控制进程和 broker。
该矩阵固定 `touch_pages=1`，每组比较 traditional/delegated。
`SIZE_SWEEP=power` 仍按 `2^MIN_EXP .. 2^MAX_EXP KiB` 生成大小。
`THREAD_COUNTS` 仅控制 `MATRIX=threads`，不控制进程矩阵。

```bash
PERF_MINIMAL=1 ALLOCATORS=scudo MATRIX=processes SIZE_SWEEP=power \
PROCESS_COUNTS=1,2,4,8 MIN_EXP=4 MAX_EXP=18 \
ALLOW_FALLBACK=1 CPUS=0 \
WARMUP=5 ITERATIONS=10 REPETITIONS=2 \
PRIMARY_WARMUP=1000 PRIMARY_ITERATIONS=10000 \
bash tests/run_phone_v3_tests.sh
```

仅查看计划、无需连接手机：把最后一行替换为
`bash tests/run_v2_android.sh --dry-run`。上述配置共 240 组。
现有 `MATRIX=power` 是独立的 Scudo cache/mmap/delegated 三路进程矩阵，保持原行为。


### 512 MiB 大小点

主机和手机 runner 支持 `MAX_EXP=19`（512 MiB），默认仍为 18。
请求大小不受 256 MiB Arena 容量限制；使用 `ALLOW_FALLBACK=1` 时，
池无法容纳的请求通过分配器原生路径回退。它仍是 delegated 配置的实测结果，
不改写为 traditional 数据。`touch_pages=1` 时，8 个 512 MiB worker 的
请求 payload 合计约 4 GiB，实际峰值还包括分配器缓存及其他开销。
双分配器、16 档大小、4 档进程数、2 条路径、5 次重复共 1280 组。
