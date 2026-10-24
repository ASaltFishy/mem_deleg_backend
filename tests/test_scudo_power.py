#!/usr/bin/env python3
import subprocess
import unittest
from pathlib import Path
import plot_scudo_power as plot

ROOT = Path(__file__).resolve().parents[1]


class PowerSweepTests(unittest.TestCase):
    def test_default_plan(self):
        text = subprocess.check_output(['bash', str(ROOT/'tests/run_v2_android.sh'), '--dry-run'], text=True)
        lines = [line for line in text.splitlines() if line.startswith('POWER_CASE_BEGIN')]
        self.assertEqual(len(lines), 270)
        cases = [dict(item.split('=', 1) for item in line.split()[1:]) for line in lines]
        self.assertEqual({int(c['exponent']) for c in cases}, set(range(4, 19)))
        self.assertEqual({c['processes'] for c in cases}, {'1', '2', '4'})
        self.assertEqual({c['variant'] for c in cases}, {'cache', 'mmap', 'delegated'})
        self.assertEqual({c['repetition'] for c in cases}, {'1', '2'})
        self.assertTrue(all(int(c['bytes']) == 1024*(2**int(c['exponent'])) for c in cases))
        self.assertEqual(len({(c['exponent'], c['processes'], c['variant'], c['repetition']) for c in cases}), 270)

    def example(self):
        return '''POWER_CASE_BEGIN index=1 total=1 exponent=7 kib=128 bytes=131072 processes=1 cpu=0 variant=delegated repetition=1
bench_lifecycle: warmup=5 iterations=10 pre_checkpoint=ok post_checkpoint=ok
bench_routes: primary=2 delegated=5 secondary=3 cache_calls=3 cache_hits=1 fallback=3 samples=10
 alloc_only mean=120 p50=100 p95=200 p99=200
POWER_CASE_PASS index=1
'''

    def test_actual_routes_kept(self):
        summary = plot.summarize(plot.parse_log(self.example()))[0]
        self.assertEqual(summary['primary_pct'], 20)
        self.assertEqual(summary['delegated_pct'], 50)
        self.assertEqual(summary['fallback_pct'], 30)
        self.assertEqual(summary['cache_hits'], 1)

    def test_reject_incomplete_or_inconsistent(self):
        for text in (self.example().replace('POWER_CASE_PASS index=1', ''),
                     self.example().replace('samples=10', 'samples=11'),
                     self.example().replace('total=1', 'total=2'),
                     self.example().replace('POWER_CASE_PASS index=1', 'POWER_CASE_FAIL index=1')):
            with self.subTest(text=text):
                with self.assertRaises(ValueError): plot.parse_log(text)


if __name__ == '__main__':
    unittest.main()
