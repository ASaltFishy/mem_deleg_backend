#!/usr/bin/env python3
import argparse
import subprocess
import unittest
from pathlib import Path
from unittest.mock import patch
import pretest_phone_latency as pretest


class PreparationTests(unittest.TestCase):
    def exercise(self, active, check_only):
        args = argparse.Namespace(adb='adb', adb_serial='', device_dir='/tmp/bench',
            device_shm_dir='/dev/md_shm', check_only=check_only, cpu_list='0',
            target='android', remote_host='', use_su=True, command_timeout=180)
        calls = []
        rebooted = False
        def fake_run(command, **kwargs):
            nonlocal rebooted
            calls.append(command)
            tail = command[command.index('-s') + 2:] if '-s' in command else command[1:]
            out = ''
            if tail == ['get-serialno']: out = 'SERIAL\n'
            elif tail == ['get-state']: out = 'device\n'
            elif tail == ['reboot']: rebooted = True
            elif tail[:1] == ['shell']:
                fragment = tail[1]
                if 'pretest_modules.sh' in fragment:
                    out = 'ACTIVE_MODULE=md-camera-scudo\nREBOOT_REQUIRED=1\n' if active else ''
                elif fragment == 'cat /proc/sys/kernel/random/boot_id':
                    out = 'new\n' if rebooted else 'old\n'
                elif fragment == 'getprop sys.boot_completed': out = '1\n'
            return subprocess.CompletedProcess(command, 0, out, '')
        with patch.object(pretest.subprocess, 'run', side_effect=fake_run), \
             patch.object(pretest.bench, 'deploy') as deploy, \
             patch.object(pretest.bench, 'prepare_device') as prepare, \
             patch.object(pretest.bench, 'remote_shell', return_value=subprocess.CompletedProcess([], 0, 'pre_checkpoint=ok post_checkpoint=ok', '')), \
             patch.object(pretest.bench, 'cleanup_domain'), \
             patch.object(pretest.bench, 'environment_snapshot', return_value='snapshot'):
            if active and check_only:
                with self.assertRaisesRegex(RuntimeError, '旧委托模块'):
                    pretest.prepare(args, {'scudo': Path('scudo')}, [0])
                deploy.assert_not_called()
            else:
                pretest.prepare(args, {'scudo': Path('scudo')}, [0])
                prepare.assert_called_once()
                self.assertEqual(args.adb_serial, 'SERIAL')
        return calls, rebooted

    def test_check_only_blocks_active_module_without_reboot(self):
        calls, rebooted = self.exercise(True, True)
        self.assertFalse(rebooted)
        self.assertTrue(any(' check ' in ' '.join(cmd) for cmd in calls))

    def test_prepare_reboots_before_probe(self):
        calls, rebooted = self.exercise(True, False)
        self.assertTrue(rebooted)
        self.assertTrue(any('getprop sys.boot_completed' in cmd for cmd in calls))

    def test_ready_phone_does_not_reboot(self):
        _, rebooted = self.exercise(False, False)
        self.assertFalse(rebooted)


if __name__ == '__main__':
    unittest.main()
