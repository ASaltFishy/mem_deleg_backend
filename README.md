# 内存委托后端与分配器时延 microbench

本仓库提供 Scudo / jemalloc 共用的内存委托后端，以及 Android 真机分配器时延实验。
代码位于 `lrc-ubuntu:/home/lrc/patent/memory-delegation-backend`。
当前配置为每 CPU Arena 256 MiB、内核共享日志 ABI v3、metadata/broker v9。
设计以 `/home/lrc/patent/kernel/kernel_platform/common/DESIGN.md` 为准。

## 1. 准备环境

以下命令均在 Ubuntu 服务器执行：

```bash
ssh lrc-ubuntu
cd /home/lrc/patent/memory-delegation-backend
```

需要 Bash、Python 3、matplotlib、ADB，以及支持当前委托协议的 Android 手机和 `su` 权限。
ADB 转发应已建立，入口默认使用：

```bash
export ADB_SERVER_SOCKET=tcp:127.0.0.1:15037
export ADB_SERIAL=3B15AL00K5D00000
```

可按实际连接覆盖这两个变量。入口自动读取 boot ID，使用
`/data/local/tmp/md_backend_v9_c256m_<boot_id>` 和域 `allocator_delegation_v9_c256m`。
已有兼容 broker 时复用，否则启动 broker；测试结束不销毁常驻共享池。
脚本不刷机、不重启，也不重置活跃 backing。旧版本或不兼容容量的 Arena
必须按设计完成停止/注销，或通过重启清除旧注册后再使用新 backing。

## 2. 构建 Android 程序

```bash
PERF_MINIMAL=1 bash tests/build_android_latency_bench.sh
```

| 配置变量 | 默认位置或值 |
| --- | --- |
| `ANDROID_NDK_ROOT` | `/home/lrc/patent/kernel/kernel_platform/prebuilts/ndk-r26` |
| `ANDROID_API` | `34` |
| `SCUDO_SOURCE` | `/home/lrc/patent/llvm-20/compiler-rt/lib/scudo/standalone` |
| `JEMALLOC_SOURCE` | `/tmp/md-latency-jemalloc-android` |

Scudo 必须包含委托补丁；jemalloc 必须已生成 `lib/libjemalloc.a`。
第三方源码包、补丁和恢复方法见 [third_party/README.md](third_party/README.md)。
`/tmp` 下的库不是永久归档，丢失后需按该说明重建。

构建生成：

- `build/allocator_latency_scudo.aarch64`
- `build/allocator_latency_jemalloc.aarch64`
- `build/memory_delegation_backend_broker.aarch64`
- `build/allocator_latency_android_manifest.json`

构建不连接手机。运行脚本部署已有产物，不自动编译；修改源码后应先重建。
构建和运行的 `PERF_MINIMAL` 必须一致。性能构建关闭额外计时探针及部分统计，
保留实际分配路线计数和协议检查；不用于要求完整分配器内存统计的业务测量。

## 3. 运行当前实验

```bash
PERF_MINIMAL=1 ALLOCATORS=scudo,jemalloc MATRIX=processes SIZE_SWEEP=power \
PROCESS_COUNTS=1,2,4,8 MIN_EXP=4 MAX_EXP=19 \
ALLOW_FALLBACK=1 CPUS=0 \
WARMUP=1 ITERATIONS=30 REPETITIONS=5 \
PRIMARY_WARMUP=1000 PRIMARY_ITERATIONS=10000 \
bash tests/run_phone_latency.sh
```

这是当前完整实验命令，不能仅运行入口脚本而省略参数：脚本自身的默认矩阵为 `power`，
与这里的双分配器 `processes` 矩阵不同。

上述配置共 **1280 组**：2 个分配器 × 16 档大小 × 4 档进程数 × 2 条路径 × 5 次重复。
请求大小为 16 KiB～512 MiB，worker 固定在 CPU0；每组触页，比较 traditional/delegated。
1/2/4/8 指 worker 进程数，不包含控制进程与 broker。16/32/64 KiB 使用至少
1000 次预热和 10000 次测量，其余大小使用 1 次预热、30 次测量。

`ALLOW_FALLBACK=1` 保留容量不足时的原生分配路径；delegated 配置不表示每次分配
都经过委托池。小块 Primary 与实际 fallback 均按日志记录，不能当成纯委托时延。

如只需查看计划，将命令最后一行替换为：

```bash
bash tests/run_android_latency.sh --dry-run
```

其余环境参数保持相同。这个入口只打印计划，不连接手机；不要向外层
`run_phone_latency.sh` 传 `--dry-run`，外层仍会读取设备 boot ID，且不转发参数。

## 4. 查看结果与重新绘图

默认结果目录为 `work/scudo-power-时间戳/`，名称沿用历史约定，双分配器结果也放在这里。
运行时可设置 `RESULTS_DIR` 指定新的目录；不要指向保留的论文数据目录。

成功运行后保存 `run.log`、`raw_results.csv`、`summary.csv`，并生成 Scudo/jemalloc 的
malloc、free、hold、all 四类 PNG/PDF 图及各自的 `_p50` 版本。
均值图默认显示跨轮标准差带；p50 为每轮 p50 的中位数，不是合并样本分位数。
hold 包含触页及测量框架辅助检查，all 为 malloc+hold+free，不包含预热和 checkpoint。

已有成功日志可单独汇总，无需重跑手机：

```bash
python3 tests/summarize_latency.py \
  --log /path/to/run.log \
  --output-dir /path/to/new-plots
```

详细脚本职责和参数说明见 [tests/README.md](tests/README.md)。

## 5. 论文数据与历史复现

- 论文数据：`work/scudo-power-20261025-222327/`，保留原始日志、CSV 与最终 PNG/PDF。
- 复现材料：`work/paper-reproduction-20261025-222327/`，包含历史源码、匹配的构建清单、
  旧构建对象、绘图处理记录、文件校验值及单独的绘图副本。先阅读其中的 `README.txt`。

论文数据的 build ID 为 `a8470476d331e565be41`，使用 **broker v8**；当前源码与程序
使用 **broker v9**。运行当前代码不能直接宣称复现了旧版本数据。
原始完整可执行文件和精确手机内核环境尚未在保留材料中确认，历史源码重建也未核对产物哈希。
论文图曾去除均值阴影并更新 fallback 标注，需按复现目录说明处理；普通绘图入口的样式不同。

## 目录用途

| 目录 | 内容 |
| --- | --- |
| `src/`、`include/`、`adapters/` | 后端、协议及分配器适配源码 |
| `tools/` | 常驻 broker 源码 |
| `tests/` | 当前 microbench 的构建、运行与汇总绘图 |
| `third_party/` | 可归档的第三方原始源码、补丁与来源记录 |
| `build/` | 当前可部署的 benchmark、broker 与构建清单 |
| `work/` | 论文数据、复现材料、恢复的 Scudo 源码，以及后续产生的临时构建和结果 |

旧相机、QEMU、独立回归与诊断入口已清理，旧结果目录亦已删除；Git 历史和标签保留。
`build.sh` 只用于本机后端编译及可选 UAPI 检查，不构建 Android benchmark。
本文按现有脚本整理，未因此执行构建、计划预演或设备测试。
