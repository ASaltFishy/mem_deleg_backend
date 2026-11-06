# 内核 / Backend / Scudo 对接记录（2026-10-22）

> 本文保留 2026-10-22～24 的历史对接记录。下文的“当前”、版本号、测试入口和验收描述
> 均指当时状态，不适用于现在的工作区。当前为内核 ABI v3、metadata/broker v9；
> 构建与运行请使用 README.md，最终设计见
> `/home/lrc/patent/kernel/kernel_platform/common/DESIGN.md`。
> 原 §2.x 章节号也属于旧文档结构，不作为当前章节引用。

## 共享状态与协议

- 元数据版本 7，公共 md_lease 增加 allocation_id 和 kernel_session。描述符 generation、Arena 分配实例序号、内核 owner-slot generation 相互独立。
- 每 Arena 一对共享 FIFO，日志在 payload 外，由 broker backing 持有。桥接句柄只有私有 session，不拥有共享日志生命周期。
- 普通 ALLOC 在同一进程共享 Mutex 内准备描述符、完整日志、release tail，再完成状态发布。无 ProducerLock。
- 允许 Mutex 持有者迁核。普通 FREE 在 Mutex 内先准备 Pending、发布日志，再检查 CPU：仍在 owner CPU 则立即入池；已迁出则等回执。跨核 FREE 继续等回执。普通同核成功回执只回收日志，禁止重复入池。正常操作不设置 affinity。
- 初始就在远端 CPU 的释放只根据保存的完整 lease 构造请求、CAS 预订日志槽、写记录并 release ready。远端不读/写描述符，不拿 Arena Mutex，不设置 Pending，不推进 generation。
- owner CPU 用户态维护独立 reclaimed，acquire completed 后检查每 ticket 的 result、block、generation、allocation_id、session、范围和状态，再入池。消费完成本身不代表成功。
- 被拒绝记录保存在 16 条最近错误回执中并累计计数；匹配当前租约的错误不会入池，必要时隔离。普通日志错误使该 Arena fail closed。计数可经 md_get_stats 读取，kernel_enabled 报实际注册 Arena 数。
- 跨核清零请求使用 md_shared_slot.reserved 的用户态 opaque bit 0，内核不读取它。只有成功回执及租约核对之后，用户态才清零并发布；过期 FREE/clear 不会先写新租约 payload。record.reserved 仍必须为 0。
- kernel-off 模式同样使用原子 remote ring，由 owner 用户态模拟消费并按次序验证；这只是兼容用户态测试，不证明 PTE 隔离。

## 生命周期和入口

Linux 可用具名 shmem；Android 客户端只走 Unix SOCK_SEQPACKET + SCM_RIGHTS attach。
broker 自身创建 memfd、初始化元数据并持有 FD，验证域名、VA、容量、CPU 数与页大小后发放两个 FD。
没有应用侧自动 creator、临时 broker 或 fork-broker。

md_backend_close 只关闭私有句柄/映射/FD，不 drain、不重置共享日志。相同 mm 可以重开相同 backing；新映射仍使用同一日志页面和内核 session。不能更换为不同 backing 规避注册约束。
进程死亡、未完成预订、Mutex 持有者死亡均不自动修复。broker 死亡后重新建立新 backing 的恢复协议不在本次范围。

md_backend_checkpoint 是生产者暂停后的管理入口：暂时调度到各 owner CPU、让调度钩子消费，并在 Mutex 下回收；不调用 DRAIN prctl，不强制复用未完成记录。无法访问的 CPU 或发布空洞会返回错误/超时。它不在测量区间，不是每次 malloc/free 的路径。

rseq 锁入口原型暂不用于这个 pthread Mutex 版本，统计会如实显示 0；未声称保留旧 rseq 速度。
旧域/旧租约布局与 v7 不兼容，禁止在参与者存活时重置或删除 backing。

## 适配器

LLVM standalone 工作树已接入最小 Secondary hook，同时保留旧 inactive cache-first 实现。显式开启统一后端并关闭旧 SharedArena 后，新路径先从 Backend 获取 lease；释放完成原有 InUseBlocks/header/statistics 处理后才发布 FREE，之后不再访问 payload。
适配器源维护在 adapters/scudo_delegation.*，standalone 通过 wrapper 共享同一实现。
Linux 和 Android builder 都从实际 LLVM 工作树取源，不依赖临时旧 Scudo snapshot。

jemalloc 保存完整 lease，发布前清空自己的 lease 表条目；继续依赖现有 PAC 补丁保证不等 dirty/muzzy 淘汰才交接。
本原型沿用统一后端原有布局：委托 Secondary 没有额外边界 guard page，MTE 请求回退；不能据此声称完整分配器安全性已验证。

## 验证边界

主机编译/UAPI 对比、Python runner/lifecycle/preflight 控制流测试；AArch64 静态 Linux 和 Android native-TLS 编译。
QEMU 用实际新内核，常驻 memfd broker + 独立进程 FD attach，测试远端回执回收、stale/duplicate/clear、强制持锁迁核、split/merge 后复用、跨进程同 VA 清零、fork 隔离和同 mm 重开。
前一轮 Scudo 验证已覆盖 128 KiB、256 KiB、1 MiB，thread/process，单 worker/同核竞争/双核，触页/不触页，traditional/delegated 共 72 组（当时每组 5 轮预热、50 轮测量）。
当前入口按用户要求默认快速矩阵：128 KiB、双核 thread、触页、两种路径，预热 5 次、测量 10 次、每组重复 2 次；`MATRIX=full` 扩大矩阵。委托路由和前后 checkpoint 必须通过，不能用减小计数绕过校验。

QEMU 时延只用于暴露挂死/崩溃及完整链路成本，不代表手机性能。没有刷机、重启手机、替换设备分配器；没有宣称所有远端运行上下文的 PTE/TLB 隔离或恶意发布者安全性已经解决。
新的 `tests/run_android_latency.sh` 与 `tests/phone/run_latency_matrix.sh` 使用常驻域，不 reset/unlink/重启/刷机；当前只做语法检查，尚未实机执行。旧 Android runner 的 fresh-domain/清理流程不作为新入口。


## 2026-10-22 性能实验更新

最终设计以内核 DESIGN.md §2.12 为准。同核普通 FREE 发布日志后、确认仍在 owner CPU，即可直接发布空闲候选；远端及已经迁核的 FREE 等回执。获取者在 Mutex 后重查 CPU，持锁迁核本身仍允许。
跨进程/跨 session/混合来源的自动整块清零已删除；MD_EAGER_CLEAR 自动策略已删除。显式 calloc、MD_ZERO 和调用方明确要求的清零保留语义。仍保留所有权、代际、日志与页表协议，未关闭内核桥接。
共享 metadata/broker 协议为 v8，默认域 allocator_delegation_v8、目录 md_backend_v8；内核 ABI 为 v3。必须使用匹配的新内核，不能复用当前启动的 v7 backing。普通成功日志由内核直接回收，远端回执仍由用户态确认。

当前协议细节见内核 DESIGN.md §2.17 与 tools/testing/memory_delegation/SHARED_LOG_V3.md。本次构建未部署到手机；新二进制不能在旧 v2 内核上运行。
