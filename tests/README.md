# Android 分配器时延测试

完整环境准备、构建和结果说明见 [仓库 README](../README.md)。以下命令在
`/home/lrc/patent/memory-delegation-backend` 执行。

## 构建与运行

```bash
PERF_MINIMAL=1 bash tests/build_android_latency_bench.sh

PERF_MINIMAL=1 ALLOCATORS=scudo,jemalloc MATRIX=processes SIZE_SWEEP=power \
PROCESS_COUNTS=1,2,4,8 MIN_EXP=4 MAX_EXP=19 \
ALLOW_FALLBACK=1 CPUS=0 \
WARMUP=1 ITERATIONS=30 REPETITIONS=5 \
PRIMARY_WARMUP=1000 PRIMARY_ITERATIONS=10000 \
bash tests/run_phone_latency.sh
```

构建与运行分别执行；运行入口不会自动重建程序。上述参数完整指定当前 1280 组实验，
不等同于入口的缺省参数。当前内核 ABI 为 v3，metadata/broker 为 v9，每 CPU Arena 256 MiB。

## 脚本职责

| 文件 | 用途 |
| --- | --- |
| `run_phone_latency.sh` | 用户入口：设置 ADB 默认连接、按 boot ID 选择目录与常驻域 |
| `run_android_latency.sh` | 主机部署、启动手机脚本、保存日志并调用汇总绘图；支持 `--dry-run` |
| `phone/run_latency_matrix.sh` | 手机端 ABI/构建配置检查、broker 启动或复用、测试矩阵执行 |
| `build_android_latency_bench.sh` | 构建两种分配器 benchmark、broker 和 manifest |
| `allocator_latency_bench.cpp` | worker、计时、触页及真实路线记录 |
| `summarize_latency.py` | 解析矩阵日志，输出 CSV 并调用绘图 |
| `plot_latency.py` | 按分配器与工作负载绘制四类时延图 |
| `plot_scudo_power.py` | 同一 runner 的 Scudo 三路径 `MATRIX=power` 分支所需解析与绘图 |

调用链：`run_phone_latency.sh` → `run_android_latency.sh` → `phone/run_latency_matrix.sh`。
当前 processes 矩阵结束后由 `summarize_latency.py` → `plot_latency.py` 汇总。
新日志使用 `LATENCY_` 标记；汇总器兼容旧 `V2_PHONE_` 日志。

## 当前实验参数

| 参数 | 当前命令值与含义 |
| --- | --- |
| `PERF_MINIMAL` | `1`，需匹配编译配置 |
| `ALLOCATORS` | `scudo,jemalloc` |
| `MATRIX` | `processes`，只运行 fork worker 进程模式 |
| `SIZE_SWEEP` | `power`，大小逐幂递增 |
| `MIN_EXP` / `MAX_EXP` | `4` / `19`，单位为 KiB 的指数，即 16 KiB～512 MiB |
| `PROCESS_COUNTS` | `1,2,4,8`，每 CPU worker 数，不含控制进程与 broker |
| `CPUS` | `0` |
| `ALLOW_FALLBACK` | `1`，记录容量不足等情况下的实际原生回退 |
| `WARMUP` / `ITERATIONS` | `1` / `30`，每轮每 worker 的预热与测量次数 |
| `REPETITIONS` | `5` |
| `PRIMARY_WARMUP` / `PRIMARY_ITERATIONS` | `1000` / `10000`，power 大小扫描中 ≤64 KiB 的次数下限 |
| `RESULTS_DIR` | 可选，覆盖默认 `work/scudo-power-时间戳/` |
| `LOG` | 可选，覆盖默认 `RESULTS_DIR/run.log` |

processes 矩阵固定触页，比较 traditional/delegated，每次重复交替两条路径的先后。
`THREAD_COUNTS` 不控制进程矩阵。`MATRIX=power` 则是独立的 Scudo cache/mmap/delegated
三路径实验，不能与 `MATRIX=processes SIZE_SWEEP=power` 混为一谈。

只打印计划时保留上面全部环境参数，将最后一行改为
`bash tests/run_android_latency.sh --dry-run`。外层手机入口不支持透传这个选项。

## 输出与论文数据

成功结束标记为 `LATENCY_ALL_PASS`。主机随后保存原始/汇总 CSV 并生成图表；
缺组或失败日志不会被当作完整成功矩阵汇总。各组显示进度与真实路线统计。
PERF_MINIMAL 运行会在内核统计节点存在时暂时关闭周期统计，并在退出时恢复；
节点缺失时日志记录 unavailable，不代表已关闭。

单独重新绘图：

```bash
python3 tests/summarize_latency.py --log /path/to/run.log --output-dir /path/to/new-plots
```

保留的论文数据在 `work/scudo-power-20261025-222327/`，复现材料在
`work/paper-reproduction-20261025-222327/`。原数据使用旧 broker v8，当前代码为 v9；
不要覆盖原始数据，也不要将当前测试结果当成旧构建的复现。
历史论文图的去阴影处理及复现限制见该材料目录的 `README.txt`。
本次仅更新文档，未运行构建、绘图或测试。
