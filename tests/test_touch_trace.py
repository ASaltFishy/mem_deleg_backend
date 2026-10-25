#!/usr/bin/env python3
"""Check opt-in per-touch sample transport through both worker modes."""
import os,re,subprocess,unittest
from pathlib import Path
from test_bench_lifecycle import LifecycleTests
class TouchTraceTests(LifecycleTests):
 @classmethod
 def setUpClass(cls):
  super().setUpClass()
  cls.binary=cls.root/'touch-bench'
  subprocess.run([cls.cxx,'-O2','-std=c++17','-pthread','-DMD_BENCH_SCUDO',
   '-DMD_TOUCH_TRACE=1','-DMD_SLOW_TRACE=1','-I'+str(cls.root),
   str(Path(__file__).with_name('allocator_latency_bench.cpp')),
   str(cls.root/'hook.cpp'),'-o',str(cls.binary)],check=True)
 def test_samples_survive_thread_and_process_transport(self):
  for mode in ['thread','process']:
   result,_=self.run_case(mode,3)
   self.assertEqual(result.returncode,0,result.stderr)
   ss=[dict((k,int(v)) for k,v in re.findall(r'(\w+)=(\d+)',l))
       for l in result.stdout.splitlines() if l.startswith('TOUCH_SAMPLE ')]
   self.assertEqual(len(ss),150)
   self.assertEqual(len({s['pid'] for s in ss}),3)
   for s in ss:
    self.assertEqual(s['wall_ns'],s['end_ns']-s['start_ns'])
    self.assertGreaterEqual(s['cpu_ns'],0)
    self.assertLess(s['index'],50)
 def test_optional_residency(self):
  old=os.environ.get('MD_TOUCH_MINCORE')
  os.environ['MD_TOUCH_MINCORE']='1'
  try:result,_=self.run_case('process',1)
  finally:
   if old is None:os.environ.pop('MD_TOUCH_MINCORE',None)
   else:os.environ['MD_TOUCH_MINCORE']=old
  self.assertEqual(result.returncode,0,result.stderr)
  ss=[dict((k,int(v)) for k,v in re.findall(r'(\w+)=(\d+)',l))
      for l in result.stdout.splitlines() if l.startswith('TOUCH_SAMPLE ')]
  self.assertEqual(len(ss),50)
  for s in ss:
   self.assertLessEqual(s['resident_pages'],s['range_pages'])
   self.assertGreater(s['range_pages'],0)
if __name__=='__main__':unittest.main()
