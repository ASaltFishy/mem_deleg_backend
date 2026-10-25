# Unified memory-delegation backend

Current development after `md-milestone-20261025` uses **256 MiB per CPU Arena**
and capacity-driven native fallback. The tag retains the earlier 2 GiB research
snapshot and its phone evidence. See `tests/README.md` for `PERF_MINIMAL=1`,
progress output, actual route counts and the required fresh-boot transition.


最终设计以 `/home/lrc/patent/kernel/kernel_platform/common/DESIGN.md` 为准。
本次实现共享元数据版本 7、内核双 ring ABI v2；旧版域必须停用，使用新域。

- 每 Arena 一对共享日志，所有参与进程共用；正常更新使用 PTHREAD_PROCESS_SHARED Mutex。
- 普通 Mutex 持有者允许迁核。同核 FREE 发布日志后立即入池；已迁出 owner CPU 的普通 FREE 和跨核 FREE 等成功回执后入池。
- 远端释放只用已有租约原子投递，不拿 Arena Mutex、不改描述符。owner CPU 用户态在 Mutex 下核对并回收。
- 普通 ALLOC 异步发布；无逐次显式 DRAIN、无 append ProducerLock。槽位同时保留记录和逐条结果，用户态回收后才能复用。
- Android 应用必须 attach 常驻 `memory_delegation_backend_broker`，经 SCM_RIGHTS 获取 memfd；应用不创建 backing、不临时 fork broker。
- 异常退出保守停滞/隔离，不自动恢复；恶意发布者认证留待后续。既有 ownership 与 PTE 检查保留。

主机与 UAPI 检查：

```sh
KERNEL_TREE=/home/lrc/patent/kernel/kernel_platform/common ./build.sh
```

构建和运行 Linux AArch64/QEMU 快速正确性验证：

```sh
bash tests/build_linux_latency_bench.sh
KERNEL_IMAGE=/home/lrc/patent/md-v2-work/dist/Image bash tests/run_v2_qemu.sh
```

Scudo 编译使用实际 LLVM 工作树和 `adapters/scudo_delegation.*`。
QEMU 启动常驻 memfd broker，运行 Backend 正确性测试；latency 默认 128 KiB、双核 thread、触页、traditional/delegated，预热 5 次、测量 10 次、每组重复 2 次（Scudo 共 4 次进程运行）。
`RUN_JEMALLOC=1` 同时验证 jemalloc（共 8 次运行）；`MATRIX=full` 恢复完整矩阵，计数也可用 `WARMUP/ITERATIONS/REPETITIONS` 覆盖。
委托组逐次核对实际路径，失败或回退不能当成通过。QEMU 时延不代表真机性能。

Android 构建（不连接、重启或刷写设备）：

```sh
bash tests/build_android_latency_bench.sh
```

产物在 `build/`：Scudo/jemalloc benchmark 与 broker。Android 运行前先在匹配新内核的环境中启动常驻服务：

```sh
MD_KERNEL_BRIDGE=required build/memory_delegation_backend_broker.aarch64 \
  /data/local/tmp/md-backend.sock allocator_delegation
```

客户端配置相同 `MD_BROKER_SOCKET`、`MEMORY_DELEGATION_DOMAIN`，并设置
`MD_KERNEL_BRIDGE=required SCUDO_DELEGATION_BACKEND=1 SCUDO_SHARED_ARENA_DISABLE=1`。
每个 broker 当前服务一个固定配置的域；不允许仍有参与者时删除/重置 backing。
新的手机入口为 `USE_SU=1 CPUS=0 bash tests/run_v2_android.sh`。默认仅 Scudo、独立进程同核竞争（1/2/4 个进程），大小从 2^4 KiB 到 2^18 KiB，逐幂递增；每组 5 次预热、10 次正式测量、重复 2 次。
三条配置曲线为普通 Secondary cache 开启、普通 Secondary cache 关闭、统一委托开启（容量不足允许普通回退）。小块保持真实 Primary 行为，不人为强制进入 Secondary；每次测量输出实际 Primary/委托/普通 Secondary 数量、缓存命中和回退数量。
默认共 270 次 benchmark 运行，输出进度中的幂次、KiB、同核进程数、配置、重复次数。结束后自动生成 `scudo_alloc_only.png/.pdf`、`raw_results.csv` 和 `summary.csv`，默认目录为 `work/scudo-power-时间戳/`，原始输出为 `run.log`。`RESULTS_DIR` 可指定目录。
`bash tests/run_v2_android.sh --dry-run` 只打印计划，不连接设备。`MIN_EXP`/`MAX_EXP` 可缩小到 4..18 范围内的子区间；`MATRIX=quick` 或 `full` 仍可切回此前通用矩阵。
Android 二进制已重建并在一加 15 上完成手机矩阵验证；测试脚本本身不刷机或重启。
旧 Android runner 的每次 fresh-domain/删 backing 生命周期不能直接用于这个常驻域；不要直接复用旧命令声称验证。

实现/生命周期与验证范围详见 `KERNEL_ALIGNMENT.md`。第二适配器在
`src/jemalloc_adapter.cpp`；Scudo standalone 的旧 SharedArena 路径仍可单独使用，但不能与统一后端混用。


## 2026-10-22 性能实验更新

最终设计以内核 DESIGN.md §2.12 为准。同核普通 FREE 发布日志后、确认仍在 owner CPU，即可直接发布空闲候选；远端及已经迁核的 FREE 等回执。获取者在 Mutex 后重查 CPU，持锁迁核本身仍允许。
跨进程/跨 session/混合来源的自动整块清零已删除；MD_EAGER_CLEAR 自动策略已删除。显式 calloc、MD_ZERO 和调用方明确要求的清零保留语义。仍保留所有权、代际、日志与页表协议，未关闭内核桥接。
共享 metadata/broker 协议为 v8，默认域 allocator_delegation_v8、目录 md_backend_v8；内核 ABI 为 v3。必须使用匹配的新内核，不能复用当前启动的 v7 backing。普通成功日志由内核直接回收，远端回执仍由用户态确认。

切换域前必须停止旧参与者并注销旧 Arena；当前没有用户态注销入口时须重启清除旧注册，否则内核会返回 EXDEV。手机本轮已重启清理，v7 broker 已运行，可直接重跑完整矩阵。


## CPU 查询与热路径优化（2026-10-22）

最终设计见内核 DESIGN.md §2.13。Android 内核启用 rseq 后，每线程注册一次，热路径读取内核维护的 TLS CPU ID；不固定核号、不绑单核。注册失败会回退系统调用。每个 benchmark 输出 rseq_workers / fallback_cpu_workers；MD_CPU_QUERY=syscall 可用于对照。
Android 构建统一使用 ThinLTO（普通 Scudo 与委托路径相同参数）。本次还精简了操作局部视图、重复参数/启用检查、页数计算和 Mutex 内描述符内存序；共享 metadata v7 与内核 ring ABI v2 不变。

一加 15 当前内核 Build ID 为 886a06ac0115a469540d64e91bb3dbf25c5812f3；CPU 查询已通过真实迁核、fork、线程退出/TLS 复用测试。新内核启用 RSEQ 时必须同时使用配套 system_dlkm，不能只改 gki_defconfig 后忽略厂商 KABI。

当前协议细节见内核 DESIGN.md §2.17 与 tools/testing/memory_delegation/SHARED_LOG_V3.md。本次构建未部署到手机；新二进制不能在旧 v2 内核上运行。
