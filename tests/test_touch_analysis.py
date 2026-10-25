import tempfile,unittest,subprocess,json
from pathlib import Path
class AnalysisTests(unittest.TestCase):
 def test_scheduled_time_and_retry_accounting(self):
  with tempfile.TemporaryDirectory() as name:
   p=Path(name)
   (p/'config.txt').write_text('size=4096\n')
   (p/'bench.log').write_text('TOUCH_SAMPLE pid=10 index=0 address=4096 start_ns=1000000000 end_ns=1010000000 wall_ns=10000000 cpu_ns=7000000 minor=1 major=0\n')
   (p/'trace-stats.txt').write_text('overrun: 0\ndropped events: 0\n')
   (p/'kprobe-profile.txt').write_text('fault_enter 2 0\nfault_exit 2 0\n')
   (p/'trace.txt').write_text('''task-10 [000] ..... 1.001000: fault_enter: address=0x1000 flags=1
task-10 [000] ..... 1.002000: sched_switch: prev_comm=task prev_pid=10 prev_state=R ==> next_comm=other next_pid=20
other-20 [000] ..... 1.005000: sched_switch: prev_comm=other prev_pid=20 prev_state=R ==> next_comm=task next_pid=10
task-10 [000] ..... 1.006000: fault_exit: ret=0x200
task-10 [000] ..... 1.006100: fault_enter: address=0x1000 flags=1
task-10 [000] ..... 1.006200: fault_exit: ret=0x400
''')
   script=Path(__file__).with_name('analyze_touch_tail.py')
   result=subprocess.run(['python3',str(script),str(p)],capture_output=True,text=True)
   self.assertEqual(result.returncode,0,result.stderr)
   s=json.loads((p/'analysis.json').read_text())['slow'][0]
   self.assertEqual(s['offcpu_ns'],3000000)
   self.assertEqual(s['fault_calls'],2)
   self.assertEqual(s['fault_retry_returns'],1)
   self.assertEqual(s['fault_scheduled_ns'],2100000)
   (p/'trace-stats.txt').write_text('overrun: 1\n')
   result=subprocess.run(['python3',str(script),str(p)],capture_output=True,text=True)
   self.assertNotEqual(result.returncode,0)
if __name__=='__main__':unittest.main()
