#!/usr/bin/env python3
"""Failure/retry protocol tests; no phone or benchmark timings are involved."""
import importlib.util
from pathlib import Path
import shlex
import subprocess
import sys
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import patch

spec = importlib.util.spec_from_file_location('latency_runner', Path(__file__).with_name('run_allocator_latency_bench.py'))
runner = importlib.util.module_from_spec(spec)
sys.modules[spec.name] = runner
spec.loader.exec_module(runner)
OUTPUT = '''================== size=128.00 KiB, threads=1, unit=ns ==================
bench_config: mode=thread cores=1 workers_per_core=1 touch_pages=1
bench_lifecycle: protocol=async_checkpoint_v1 warmup=20 iterations=20 pre_checkpoint=ok post_checkpoint=ok
alloc_only mean=10.00 p50=10 p95=10 p99=10
'''

class ProtocolTests(unittest.TestCase):
    def run_fixture(self, folder, shell, retries=0):
        binary = folder / 'fake-binary'
        binary.write_text('fixture')
        scripts = folder / 'tests'; scripts.mkdir(exist_ok=True)
        argv = ['runner', '--target', 'host', '--allocators', 'jemalloc',
                '--jemalloc-binary', str(binary), '--execution-modes', 'userspace',
                '--touch-page-modes', '1', '--modes', 'thread', '--core-counts', '1',
                '--workers-per-core', '1', '--paths', 'delegated', '--sizes', '131072', '--iterations', '20',
                '--warmup', '20', '--repetitions', '1', '--cold-runs', '0', '--cpu-list', '0',
                '--cooldown', '0', '--command-retries', str(retries), '--results-dir', str(folder/'out')]
        with patch.object(runner, 'SCRIPT_DIR', scripts), patch.object(sys, 'argv', argv), \
             patch.object(runner, 'remote_shell', side_effect=shell), \
             patch.object(runner, 'environment_snapshot', return_value='stable'), \
             patch.object(runner, 'plot_allocator'):
            runner.main()

    def test_retry_has_new_domain_and_each_attempt_is_cleaned(self):
        calls, domains = [], []
        def shell(_host, command, **kwargs):
            calls.append(command)
            if command.startswith('rm -f'):
                return subprocess.CompletedProcess([], 0, '', '')
            domains.append(next(x.split('=',1)[1] for x in shlex.split(command) if x.startswith('MEMORY_DELEGATION_DOMAIN=')))
            return subprocess.CompletedProcess([], 1 if len(domains)==1 else 0, OUTPUT, '')
        with tempfile.TemporaryDirectory() as name:
            self.run_fixture(Path(name), shell, retries=1)
            self.assertEqual(len(domains), 2)
            self.assertNotEqual(*domains)
            self.assertEqual(sum(x.startswith('rm -f') for x in calls), 2)
            self.assertTrue(list((Path(name)/'out/raw').glob('*.failed')))

    def test_timeout_stops_without_unlinking_backing(self):
        calls = []
        def shell(_host, command, **kwargs):
            calls.append(command)
            raise subprocess.TimeoutExpired(command, 1)
        with tempfile.TemporaryDirectory() as name:
            with self.assertRaisesRegex(RuntimeError, '超时'):
                self.run_fixture(Path(name), shell)
            self.assertEqual(len(calls), 1)
            self.assertFalse(any(x.startswith('rm -f') for x in calls))

    def test_old_binary_output_is_rejected(self):
        with self.assertRaisesRegex(RuntimeError, '确认'):
            runner.parse_output('alloc_only mean=1 p50=1 p95=1 p99=1', 'a', 'kernel', 'delegated', 1, ())

    def test_kernel_probe_rejects_unsupported_kernel_before_matrix(self):
        args = SimpleNamespace(target='android', adb='adb', adb_serial='', remote_host='',
            remote_stage_dir='/tmp/stage', device_dir='/data/local/tmp/bench',
            use_su=True, device_shm_dir='/dev/md_shm', command_timeout=10)
        def shell(_host, command, **kwargs):
            if '--build-info' in command:
                return subprocess.CompletedProcess([],0,'bench_protocol=async_checkpoint_v1','')
            if '--path' in command:
                return subprocess.CompletedProcess([],1,'','kernel unsupported')
            return subprocess.CompletedProcess([],0,'','')
        with patch.object(runner,'remote_shell',side_effect=shell), \
             patch.object(runner,'adb_command',return_value=subprocess.CompletedProcess([],0,'','')), \
             patch.object(runner,'cleanup_domain') as cleanup:
            with self.assertRaisesRegex(RuntimeError,'内核 ABI / Arena'):
                runner.prepare_device(args,{'jemalloc':Path('/tmp/fixture')},0,['kernel'])
            cleanup.assert_called_once()

    def test_quick_defaults_and_five_warmups_are_accepted(self):
        with patch.object(sys, 'argv', ['runner', '--dry-run']), \
             patch.object(runner, 'run_matrix') as matrix:
            runner.main()
            args = matrix.call_args.args[0]
            self.assertEqual((args.warmup, args.iterations, args.repetitions, args.cold_runs),
                             (5, 10, 2, 0))
        with patch.object(sys, 'argv', ['runner', '--dry-run', '--warmup', '5']), \
             patch.object(runner, 'run_matrix') as matrix:
            runner.main()
            self.assertEqual(matrix.call_args.args[0].warmup, 5)

    def test_failure_reports_backend_stderr_and_log_path(self):
        def shell(_host, command, **kwargs):
            return subprocess.CompletedProcess([], 0 if command.startswith('rm -f') else 1,
                                               '', 'backend error=12')
        with tempfile.TemporaryDirectory() as name:
            with self.assertRaisesRegex(RuntimeError, '失败日志:.*failed\\nbackend error=12'):
                self.run_fixture(Path(name), shell)

    def test_long_domain_suffix_stays_unique(self):
        first = runner.make_domain('x'*120, 'scudo', 'touch0')
        second = runner.make_domain('x'*120, 'scudo', 'touch1')
        self.assertNotEqual(first, second)
        self.assertLessEqual(len(first), 96)

if __name__ == '__main__': unittest.main()
