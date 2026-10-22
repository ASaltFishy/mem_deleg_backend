#!/usr/bin/env python3
"""Prepare an isolated Android latency run; may disable old test modules/reboot."""
import argparse
import fcntl
import os
from pathlib import Path
import shlex
import subprocess
import sys
import tempfile
import time
import uuid

import run_allocator_latency_bench as bench

ROOT = Path(__file__).resolve().parent.parent
MODULES = ('md-camera-scudo', 'md-system-scudo')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--check-only', action='store_true',
                        help='只检查和运行微型探测；不修改模块、不重启')
    parser.add_argument('--cpu-list', default='0')
    parser.add_argument('--adb', default=os.environ.get('ADB_BIN', 'adb'))
    parser.add_argument('--adb-serial', default=os.environ.get('ADB_SERIAL', ''))
    parser.add_argument('--device-dir', default='/data/local/tmp/md-allocator-bench')
    parser.add_argument('--device-shm-dir', default=os.environ.get('DEVICE_SHM_DIR', '/dev/md_shm'))
    parser.add_argument('--scudo-binary', default=os.environ.get('SCUDO_BENCH_BINARY', str(ROOT / 'build/allocator_latency_scudo.aarch64')))
    parser.add_argument('--jemalloc-binary', default=os.environ.get('JEMALLOC_BENCH_BINARY', str(ROOT / 'build/allocator_latency_jemalloc.aarch64')))
    args = parser.parse_args()
    args.target, args.remote_host, args.use_su = 'android', '', True
    args.command_timeout = 180
    binaries = {'scudo': Path(args.scudo_binary).resolve(),
                'jemalloc': Path(args.jemalloc_binary).resolve()}
    for name, path in binaries.items():
        if not path.is_file():
            raise RuntimeError(f'{name} 二进制不存在：{path}；先运行 bash tests/build_android_latency_bench.sh')
    cpus = bench.split_csv(args.cpu_list, int)
    if not cpus or any(cpu < 0 for cpu in cpus):
        raise RuntimeError('--cpu-list 必须为非负 CPU ID 列表')
    lock_path = ROOT / 'build/latency-bench.lock'
    lock_path.parent.mkdir(parents=True, exist_ok=True)
    with lock_path.open('a') as lock:
        try:
            fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BlockingIOError:
            raise RuntimeError('已有 latency 测试/准备运行，不能修改环境')
        prepare(args, binaries, cpus)


def prepare(args, binaries, cpus):
    adb = [args.adb] + (['-s', args.adb_serial] if args.adb_serial else [])
    def call(*parts, timeout=30):
        result = subprocess.run([*adb, *parts], text=True, capture_output=True, timeout=timeout)
        if result.returncode:
            raise RuntimeError(f'ADB 失败：{result.stdout}{result.stderr}\n'
                               '若 15037 connection refused，请先在 Mac 建立 ADB SSH 转发。')
        return result.stdout.strip()
    call('get-state')
    serial = call('get-serialno')
    # Pin subsequent calls to the verified device, including after reconnect.
    args.adb_serial = serial
    adb[:] = [args.adb, '-s', serial]
    print(f'[device] {serial}', flush=True)
    call('shell', 'mkdir -p ' + shlex.quote(args.device_dir))
    # Keep compound/root phone operations in an inspectable script.
    pending = args.device_dir + '/pretest_pending_boot'
    script = '''#!/system/bin/sh
set -eu
[ "$(id -u)" = 0 ] || { echo "root required" >&2; exit 1; }
mode="$1"
boot=$(cat /proc/sys/kernel/random/boot_id)
pending="$2"
need=0
for name in md-camera-scudo md-system-scudo; do
  d=/data/adb/modules/$name
  if [ -d "$d" ] && [ ! -f "$d/disable" ] && [ ! -f "$d/remove" ]; then
    echo "ACTIVE_MODULE=$name"
    if [ "$mode" = prepare ]; then
      printf '%s\\n' "$boot" > "$pending"
      touch "$d/disable"
    fi
    need=1
  fi
done
if [ -f "$pending" ] && [ "$(cat "$pending")" = "$boot" ]; then need=1; fi
if [ "$need" = 1 ]; then echo REBOOT_REQUIRED=1; fi
if [ "$mode" = prepare ] && [ "$need" = 0 ]; then rm -f "$pending"; fi
'''
    destination = args.device_dir + '/pretest_modules.sh'
    with tempfile.TemporaryDirectory(prefix='md-pretest-') as tmp:
        source = Path(tmp) / 'pretest_modules.sh'
        source.write_text(script)
        call('push', str(source), destination)
    mode = 'check' if args.check_only else 'prepare'
    command = 'sh ' + ' '.join(shlex.quote(x) for x in (destination, mode, pending))
    state = call('shell', 'su -c ' + shlex.quote(command))
    if state:
        print(state, flush=True)
    if 'REBOOT_REQUIRED=1' in state:
        if args.check_only:
            raise RuntimeError('旧委托模块仍启用或尚未重启；去掉 --check-only 运行本脚本完成准备')
        print('[prepare] 已停用旧委托测试模块，重启手机以清除内核旧 Arena；不会自动恢复这些模块。', flush=True)
        old_boot = call('shell', 'cat /proc/sys/kernel/random/boot_id')
        call('reboot')
        deadline = time.monotonic() + 240
        while True:
            try:
                new_boot = call('shell', 'cat /proc/sys/kernel/random/boot_id', timeout=5)
                completed = call('shell', 'getprop sys.boot_completed', timeout=5)
                if new_boot and new_boot != old_boot and completed == '1':
                    break
            except (RuntimeError, subprocess.TimeoutExpired):
                pass
            if time.monotonic() >= deadline:
                raise RuntimeError('等待手机重启并完成开机超时；解锁手机后重跑本脚本')
            time.sleep(2)
    bench.deploy(args, binaries)
    bench.prepare_device(args, binaries, cpus[0], ['userspace', 'kernel'])
    # Check every selected CPU, including immediate payload touch and checkpoint.
    for allocator, binary in binaries.items():
        for cpu in cpus:
            for mode, workers in [('thread', 4), ('thread', 8), ('process', 1)]:
                domain = f'md_pretest_{uuid.uuid4().hex}'
                env = {'MD_KERNEL_BRIDGE': 'required', 'MEMORY_DELEGATION_DOMAIN': domain,
                       'MD_SHM_DIR': args.device_shm_dir, 'SCUDO_DELEGATION_BACKEND': '1',
                       'SCUDO_SHARED_ARENA_DISABLE': '1'}
                command = bench.benchmark_command(args, binary, env,
                    ['--path', 'delegated', '--mode', mode, '--cpus', cpu,
                     '--workers-per-core', workers, '--size', 131072, '--warmup', 5,
                     '--iterations', 1, '--touch-pages', 1])
                result = bench.remote_shell('', command, capture=True, check=False, timeout=180)
                if result.returncode < 0 or result.returncode >= 128:
                    raise RuntimeError(f'探测异常终止，保留 domain={domain}\n{result.stdout}{result.stderr}')
                bench.cleanup_domain(args, domain)
                if result.returncode or 'pre_checkpoint=ok post_checkpoint=ok' not in result.stdout:
                    raise RuntimeError(f'{allocator} CPU{cpu} 探测失败；若旧模块已停用但 Arena 仍冲突，请重启手机再运行。\n{result.stdout}{result.stderr}')
                print(f'[ok] {allocator} CPU{cpu} {mode}×{workers}: 5 次预热、payload 读写、前后 drain', flush=True)
    print(bench.environment_snapshot(args), flush=True)
    print('[ready] pretest 通过，可以运行 bash tests/run_phone_latency_suite.sh --cpu-list ' + args.cpu_list)


if __name__ == '__main__':
    try:
        main()
    except (Exception, KeyboardInterrupt) as error:
        print(f'pretest 失败：{error}', file=sys.stderr)
        sys.exit(1)
