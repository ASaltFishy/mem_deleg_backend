#!/usr/bin/env python3
"""Verify benchmark barriers using a counting transport, not allocator timings."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

class LifecycleTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory()
        cls.root = Path(cls.temp.name)
        cls.cxx = shutil.which('clang++') or shutil.which('g++')
        if not cls.cxx: raise unittest.SkipTest('host C++ compiler unavailable')
        (cls.root/'scudo_delegation.h').write_text('''#include <cstdint>
namespace scudo { inline bool scudoDelegationContains(uintptr_t) { return true; } }
''')
        (cls.root/'hook.cpp').write_text('''#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <fcntl.h>
#include <unistd.h>
extern "C" int __scudo_bench_is_primary(const void *) { return 0; }
extern "C" void __scudo_bench_cache_stats(uint64_t *Calls, uint64_t *Hits) { *Calls=0; *Hits=0; }
extern "C" int md_scudo_checkpoint() {
  int Fd = open(getenv("CHECKPOINT_TRACE"), O_WRONLY|O_APPEND|O_CREAT, 0600);
  if (Fd < 0) return 5;
  char Text[32]; int Count=snprintf(Text,sizeof(Text),"%d\\n",getpid());
  bool Ok=write(Fd,Text,Count)==Count; close(Fd); return Ok ? 0 : 5;
}
''')
        cls.binary = cls.root/'bench'
        source = Path(__file__).with_name('allocator_latency_bench.cpp')
        subprocess.run([cls.cxx,'-O2','-std=c++17','-pthread','-DMD_BENCH_SCUDO',
                        '-I'+str(cls.root),str(source),str(cls.root/'hook.cpp'),'-o',str(cls.binary)],check=True)
        cls.cpu = min(os.sched_getaffinity(0))

    @classmethod
    def tearDownClass(cls): cls.temp.cleanup()

    def run_case(self, mode, workers=2, cpu=None):
        trace = self.root/f'{mode}_{workers}_{cpu}.trace'
        command=[str(self.binary),'--mode',mode,'--path','delegated','--size','131072',
                 '--warmup','20','--iterations','50','--cpus',str(self.cpu if cpu is None else cpu),
                 '--workers-per-core',str(workers)]
        result=subprocess.run(command,env=dict(os.environ,CHECKPOINT_TRACE=str(trace)),
                              text=True,capture_output=True,timeout=10)
        return result, trace.read_text().splitlines() if trace.exists() else []

    def test_threads_checkpoint_once_per_boundary(self):
        result, calls = self.run_case('thread')
        self.assertEqual(result.returncode,0,result.stderr)
        self.assertEqual(len(calls),2)  # not per worker, not per allocation
        self.assertIn('pre_checkpoint=ok post_checkpoint=ok',result.stdout)
        self.assertIn('bench_noise:',result.stdout)

    def test_processes_checkpoint_each_mm_twice(self):
        result, calls = self.run_case('process')
        self.assertEqual(result.returncode,0,result.stderr)
        self.assertEqual(len(calls),4)
        self.assertEqual(len(set(calls)),2)
        self.assertTrue(all(calls.count(pid)==2 for pid in set(calls)))

    def test_pin_failure_reaps_process_workers(self):
        command=[str(self.binary),'--mode','process','--path','traditional','--size','131072',
                 '--warmup','20','--iterations','50','--cpus','1023','--workers-per-core','2']
        process=subprocess.Popen(command,stdout=subprocess.PIPE,stderr=subprocess.PIPE,start_new_session=True,text=True)
        process.communicate(timeout=10)
        self.assertNotEqual(process.returncode,0)
        with self.assertRaises(ProcessLookupError): os.killpg(process.pid,0)

if __name__ == '__main__': unittest.main()
