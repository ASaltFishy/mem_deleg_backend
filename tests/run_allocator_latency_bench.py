#!/usr/bin/env python3
"""Run matching Scudo/jemalloc delegation latency matrices and draw two figures.

This runner deliberately keeps every size/path/repetition in a fresh process.
The benchmark binary controls worker affinity and emits the same metric lines as
the historical Scudo SharedArena latency benchmark.
"""

import argparse
import csv
import os
import re
import shlex
import subprocess
import sys
from dataclasses import asdict, dataclass
from datetime import datetime
from pathlib import Path

import matplotlib.pyplot as plt
import pandas as pd


SCRIPT_DIR = Path(__file__).resolve().parent
SSH_ARGS = ["ssh", "-o", "BatchMode=yes", "-o", "StrictHostKeyChecking=accept-new"]
HEADER_RE = re.compile(r"=+ size=(?P<number>[0-9.]+) (?P<unit>\w+), threads=(?P<workers>\d+), unit=ns =+")
CONFIG_RE = re.compile(r"bench_config: mode=(?P<mode>\w+) cores=(?P<cores>\d+) workers_per_core=(?P<wpc>\d+) touch_pages=(?P<touch>[01])")
METRIC_RE = re.compile(r"^\s*(?P<metric>[a-z_]+)\s+mean=(?P<mean>[0-9.]+)\s+p50=(?P<p50>\d+)\s+p95=(?P<p95>\d+)\s+p99=(?P<p99>\d+)")


@dataclass
class Record:
    allocator: str
    execution: str
    path: str
    mode: str
    touch_pages: int
    cores: int
    workers_per_core: int
    workers: int
    size_bytes: int
    repetition: int
    metric: str
    mean_ns: float
    p50_ns: int
    p95_ns: int
    p99_ns: int


def split_csv(value, cast=str):
    return [cast(item.strip()) for item in value.split(",") if item.strip()]


def run(command, *, capture=False, check=True):
    return subprocess.run(command, text=True, capture_output=capture, check=check)


def remote_shell(host, command, *, capture=False):
    if host:
        return run([*SSH_ARGS, host, f"bash -lc {shlex.quote(command)}"], capture=capture)
    return run(["bash", "-lc", command], capture=capture)


def adb_command(args, fragment, *, capture=False):
    adb = shlex.quote(args.adb)
    serial = f" -s {shlex.quote(args.adb_serial)}" if args.adb_serial else ""
    return remote_shell(args.remote_host, f"{adb}{serial} shell {shlex.quote(fragment)}", capture=capture)


def detect_physical_cpus(args):
    if args.cpu_list:
        return split_csv(args.cpu_list, int)
    if args.target == "host":
        root = Path("/sys/devices/system/cpu")
        allowed = os.sched_getaffinity(0)
        entries = []
        for cpu_dir in sorted(root.glob("cpu[0-9]*"), key=lambda p: int(p.name[3:])):
            cpu = int(cpu_dir.name[3:])
            if cpu not in allowed:
                continue
            try:
                core = int((cpu_dir / "topology/core_id").read_text().strip())
                package = int((cpu_dir / "topology/physical_package_id").read_text().strip())
            except (OSError, ValueError):
                core, package = cpu, 0
            entries.append((cpu, package, core))
    else:
        script = (
            "allowed=$(awk '/Cpus_allowed_list/ {print $2}' /proc/self/status); "
            "for d in /sys/devices/system/cpu/cpu[0-9]*; do "
            "c=${d##*cpu}; test -f $d/online && test \"$(cat $d/online)\" = 0 && continue; "
            "case ,$allowed, in *,$c,*|*,$c-*) ;; *) "
            "echo $allowed | awk -v c=$c -F, '{for(i=1;i<=NF;i++){split($i,r,\"-\"); "
            "if((r[2]==\"\" && c==r[1]) || (r[2]!=\"\" && c>=r[1] && c<=r[2])) exit 0} exit 1}' "
            ">/dev/null || continue;; esac; "
            "core=$(cat $d/topology/core_id 2>/dev/null || echo $c); "
            "pkg=$(cat $d/topology/physical_package_id 2>/dev/null || echo 0); "
            "echo $c $pkg $core; done"
        )
        output = adb_command(args, script, capture=True).stdout
        entries = [tuple(map(int, line.split())) for line in output.splitlines() if line.strip()]
    selected, seen = [], set()
    for cpu, package, core in entries:
        key = (package, core)
        if key not in seen:
            seen.add(key)
            selected.append(cpu)
    if not selected:
        raise RuntimeError("没有发现可用 CPU；请显式传 --cpu-list")
    return selected


def deploy(args, binaries):
    adb_command(args, f"mkdir -p {shlex.quote(args.device_dir)}")
    for allocator, binary in binaries.items():
        destination = f"{args.device_dir}/{binary.name}"
        if args.remote_host:
            stage = f"{args.remote_stage_dir}/{binary.name}"
            remote_shell(args.remote_host, f"mkdir -p {shlex.quote(args.remote_stage_dir)}")
            run(["scp", str(binary), f"{args.remote_host}:{stage}"])
            source = stage
        else:
            source = str(binary)
        adb = shlex.quote(args.adb)
        serial = f" -s {shlex.quote(args.adb_serial)}" if args.adb_serial else ""
        remote_shell(args.remote_host,
                     f"{adb}{serial} push {shlex.quote(source)} {shlex.quote(destination)} >/dev/null && "
                     f"{adb}{serial} shell chmod 755 {shlex.quote(destination)}")
        print(f"[deploy] {allocator}: {destination}", flush=True)


def make_domain(session, allocator, execution):
    token = f"{session}_{allocator}_{execution}"
    return re.sub(r"[^A-Za-z0-9_]", "_", token)[:100]


def benchmark_command(args, binary, env, bench_args):
    env_text = " ".join(f"{shlex.quote(k)}={shlex.quote(v)}" for k, v in env.items())
    argv = " ".join(shlex.quote(str(item)) for item in bench_args)
    if args.target == "host":
        return f"env {env_text} {shlex.quote(str(binary))} {argv}"
    device_binary = f"{args.device_dir}/{binary.name}"
    device = f"cd {shlex.quote(args.device_dir)} && env {env_text} ./{shlex.quote(binary.name)} {argv}"
    adb = shlex.quote(args.adb)
    serial = f" -s {shlex.quote(args.adb_serial)}" if args.adb_serial else ""
    return f"{adb}{serial} shell {shlex.quote(device)}"


def parse_output(text, allocator, execution, path, repetition, expected):
    header = config = None
    records = []
    for line in text.splitlines():
        header = HEADER_RE.search(line) or header
        config = CONFIG_RE.search(line) or config
        metric = METRIC_RE.match(line)
        if not metric:
            continue
        if not header or not config:
            raise RuntimeError("metric 出现在 benchmark header/config 之前")
        unit_scale = {"B": 1, "KiB": 1024, "MiB": 1024 * 1024}[header.group("unit")]
        size_bytes = int(round(float(header.group("number")) * unit_scale))
        records.append(Record(
            allocator=allocator, execution=execution, path=path,
            mode=config.group("mode"), touch_pages=int(config.group("touch")),
            cores=int(config.group("cores")), workers_per_core=int(config.group("wpc")),
            workers=int(header.group("workers")), size_bytes=size_bytes,
            repetition=repetition, metric=metric.group("metric"),
            mean_ns=float(metric.group("mean")), p50_ns=int(metric.group("p50")),
            p95_ns=int(metric.group("p95")), p99_ns=int(metric.group("p99"))))
    if not records:
        raise RuntimeError("没有解析到 benchmark 指标")
    first = records[0]
    actual = (first.mode, first.touch_pages, first.cores, first.workers_per_core, first.size_bytes)
    if actual != expected:
        raise RuntimeError(f"benchmark 配置回显不匹配: expected={expected}, actual={actual}")
    return records


def summarize(records):
    frame = pd.DataFrame([asdict(record) for record in records])
    keys = ["allocator", "execution", "path", "mode", "touch_pages", "cores",
            "workers_per_core", "workers", "size_bytes", "metric"]
    summary = frame.groupby(keys, as_index=False).agg(
        mean_ns=("mean_ns", "mean"), std_ns=("mean_ns", "std"), samples=("mean_ns", "size"))
    summary["std_ns"] = summary["std_ns"].fillna(0.0)
    return frame, summary


def plot_allocator(summary, allocator, output):
    data = summary[summary["allocator"] == allocator].copy()
    conditions = data[["execution", "touch_pages", "mode"]].drop_duplicates().sort_values(
        ["execution", "touch_pages", "mode"]).itertuples(index=False, name=None)
    conditions = list(conditions)
    fig, axes = plt.subplots(max(1, len(conditions)), 2,
                             figsize=(14, max(5, 4.5 * len(conditions))), squeeze=False)
    colors = {"traditional": "#d62728", "delegated": "#1f77b4"}
    markers = ["o", "s", "^", "D", "v", "P", "X"]
    for row, (execution, touch, mode) in enumerate(conditions):
        subset = data[(data.execution == execution) & (data.touch_pages == touch) & (data["mode"] == mode)]
        for col, metric in enumerate(["alloc_only", "dealloc_only"]):
            ax = axes[row][col]
            metric_data = subset[subset.metric == metric]
            groups = metric_data[["path", "cores", "workers_per_core"]].drop_duplicates()
            for index, values in enumerate(groups.itertuples(index=False, name=None)):
                path, cores, wpc = values
                line = metric_data[(metric_data.path == path) & (metric_data.cores == cores) &
                                   (metric_data.workers_per_core == wpc)].sort_values("size_bytes")
                ax.plot(line.size_bytes / 1024, line.mean_ns, color=colors[path],
                        marker=markers[index % len(markers)], linewidth=1.8,
                        label=f"{path} {cores}c×{wpc}")
                ax.fill_between(line.size_bytes / 1024,
                                (line.mean_ns - line.std_ns).clip(lower=1),
                                line.mean_ns + line.std_ns, color=colors[path], alpha=0.10)
            ax.set_xscale("log", base=2)
            ax.set_yscale("log")
            ax.set_xlabel("Allocation size (KiB)")
            ax.set_ylabel(f"{metric} mean latency (ns)")
            ax.set_title(f"{execution}, touch={touch}, {mode}")
            ax.grid(True, which="both", linestyle="--", alpha=0.3)
            ax.legend(fontsize=8, ncol=2)
    fig.suptitle(f"{allocator} memory-delegation allocation latency")
    fig.tight_layout()
    fig.savefig(output, dpi=180, bbox_inches="tight")
    plt.close(fig)


def main():
    parser = argparse.ArgumentParser(description="Scudo/jemalloc 统一内存委托时延测试与绘图")
    parser.add_argument("--target", choices=["host", "android"], default="android")
    parser.add_argument("--allocators", default="scudo,jemalloc")
    parser.add_argument("--scudo-binary", default=str(SCRIPT_DIR.parent / "build/allocator_latency_scudo"))
    parser.add_argument("--jemalloc-binary", default=str(SCRIPT_DIR.parent / "build/allocator_latency_jemalloc"))
    parser.add_argument("--execution-modes", default="userspace,kernel",
                        help="userspace 设置 MD_KERNEL_BRIDGE=off；kernel 设置 required")
    parser.add_argument("--touch-page-modes", default="0,1")
    parser.add_argument("--modes", default="thread,process")
    parser.add_argument("--core-counts", default="1,2,4,8")
    parser.add_argument("--workers-per-core", default="1",
                        help="thread 模式为每核线程数，process 模式为每核进程数；1 core×N 对齐原同核竞争测试")
    parser.add_argument("--cpu-list", default="", help="候选物理核 CPU ID；默认按 topology 自动选每核一个 CPU")
    parser.add_argument("--sizes", default="65536,131072,262144,1048576")
    parser.add_argument("--iterations", type=int, default=200)
    parser.add_argument("--warmup", type=int, default=50)
    parser.add_argument("--repetitions", type=int, default=5)
    parser.add_argument("--cold-runs", type=int, default=1)
    parser.add_argument("--paths", default="traditional,delegated")
    parser.add_argument("--remote-host", default=os.environ.get("REMOTE_HOST", ""))
    parser.add_argument("--remote-stage-dir", default="/tmp/md-allocator-bench")
    parser.add_argument("--device-dir", default="/data/local/tmp/md-allocator-bench")
    parser.add_argument("--adb", default=os.environ.get("ADB_BIN", "adb"))
    parser.add_argument("--adb-serial", default=os.environ.get("ADB_SERIAL", ""))
    parser.add_argument("--skip-deploy", action="store_true")
    parser.add_argument("--results-dir", default="")
    parser.add_argument("--dry-run", action="store_true", help="只打印完整矩阵命令，不执行、不部署")
    args = parser.parse_args()

    allocators = split_csv(args.allocators)
    binaries = {"scudo": Path(args.scudo_binary), "jemalloc": Path(args.jemalloc_binary)}
    unknown = set(allocators) - set(binaries)
    if unknown:
        raise RuntimeError(f"不支持的 allocator: {sorted(unknown)}")
    for allocator in allocators:
        if not binaries[allocator].is_file():
            raise RuntimeError(f"未找到 {allocator} benchmark: {binaries[allocator]}")
    execution_modes = split_csv(args.execution_modes)
    if set(execution_modes) - {"userspace", "kernel"}:
        raise RuntimeError("--execution-modes 只支持 userspace,kernel")
    modes = split_csv(args.modes)
    if set(modes) - {"thread", "process"}:
        raise RuntimeError("--modes 只支持 thread,process")
    paths = split_csv(args.paths)
    if set(paths) - {"traditional", "delegated"}:
        raise RuntimeError("--paths 只支持 traditional,delegated")

    cpus = detect_physical_cpus(args)
    core_counts = split_csv(args.core_counts, int)
    if any(count < 1 or count > len(cpus) for count in core_counts):
        raise RuntimeError(f"--core-counts 超出可用物理核数量 {len(cpus)}: {core_counts}")
    worker_counts = split_csv(args.workers_per_core, int)
    touch_modes = split_csv(args.touch_page_modes, int)
    sizes = split_csv(args.sizes, int)
    if set(touch_modes) - {0, 1} or any(value < 1 for value in worker_counts):
        raise RuntimeError("touch 只能为 0/1，workers-per-core 必须大于 0")

    if args.target == "android" and not args.skip_deploy and not args.dry_run:
        deploy(args, {key: binaries[key] for key in allocators})

    stamp = datetime.now().strftime("%Y%m%d-%H%M%S")
    results = Path(args.results_dir) if args.results_dir else SCRIPT_DIR / "results" / stamp
    raw_dir = results / "raw"
    if not args.dry_run:
        raw_dir.mkdir(parents=True, exist_ok=True)

    records = []
    for allocator in allocators:
        for execution in execution_modes:
            for touch in touch_modes:
                for mode in modes:
                    for core_count in core_counts:
                        selected = cpus[:core_count]
                        for wpc in worker_counts:
                            for size in sizes:
                                for path in paths:
                                    for run_index in range(args.cold_runs + args.repetitions):
                                        cold = run_index < args.cold_runs
                                        repetition = 0 if cold else run_index - args.cold_runs + 1
                                        env = {
                                            "MD_KERNEL_BRIDGE": "off" if execution == "userspace" else "required",
                                            "MEMORY_DELEGATION_DOMAIN": make_domain(stamp, allocator, execution),
                                        }
                                        if allocator == "scudo":
                                            env.update({
                                                "SCUDO_DELEGATION_BACKEND": "1" if path == "delegated" else "0",
                                                "SCUDO_SHARED_ARENA_DISABLE": "1",
                                            })
                                        bench_args = [
                                            "--path", path, "--mode", mode, "--size", size,
                                            "--iterations", args.iterations, "--warmup", args.warmup,
                                            "--touch-pages", touch, "--cpus", ",".join(map(str, selected)),
                                            "--workers-per-core", wpc,
                                        ]
                                        command = benchmark_command(args, binaries[allocator], env, bench_args)
                                        label = (f"{allocator}/{execution}/{mode}/touch{touch}/"
                                                 f"{core_count}c×{wpc}/size{size}/{path}/"
                                                 f"{'cold' if cold else f'rep{repetition}'}")
                                        print(f"[run] {label}\n  {command}", flush=True)
                                        if args.dry_run:
                                            continue
                                        result = remote_shell(args.remote_host, command, capture=True)
                                        raw_name = re.sub(r"[^A-Za-z0-9_.-]", "_", label) + ".txt"
                                        (raw_dir / raw_name).write_text(result.stdout + result.stderr, encoding="utf-8")
                                        if cold:
                                            continue
                                        expected = (mode, touch, core_count, wpc, size)
                                        records.extend(parse_output(result.stdout, allocator, execution,
                                                                    path, repetition, expected))

    if args.dry_run:
        return
    if not records:
        raise RuntimeError("测试没有产生可汇总记录")
    raw, summary = summarize(records)
    raw.to_csv(results / "raw_results.csv", index=False, quoting=csv.QUOTE_MINIMAL)
    summary.to_csv(results / "summary.csv", index=False, quoting=csv.QUOTE_MINIMAL)
    for allocator in allocators:
        output = results / f"{allocator}_latency.png"
        plot_allocator(summary, allocator, output)
        print(f"[plot] {output}")
    print(f"[done] {results}")


if __name__ == "__main__":
    try:
        main()
    except Exception as error:
        print(f"错误：{error}", file=sys.stderr)
        sys.exit(1)
