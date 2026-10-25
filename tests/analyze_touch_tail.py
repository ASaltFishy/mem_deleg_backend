#!/usr/bin/env python3
import re,json,csv,sys,statistics as st
from pathlib import Path
from collections import defaultdict,Counter
root=Path(sys.argv[1]);size=int(re.search(r'size=(\d+)',(root/'config.txt').read_text())[1])
samples=[{k:int(v) for k,v in re.findall(r'(\w+)=(\d+)',l)} for l in (root/'bench.log').read_text().splitlines() if l.startswith('TOUCH_SAMPLE ')]
assert samples
if (root/'trace.txt').exists():
 stats=(root/'trace-stats.txt').read_text()
 for key,value in re.findall(r'^(overrun|commit overrun|dropped events): (\d+)$',stats,re.M):
  assert int(value)==0, f'Trace loss: {key}={value}'
 for line in (root/'kprobe-profile.txt').read_text().splitlines():
  q=line.split()
  if len(q)==3 and q[0].endswith(('_enter','_exit')):
   assert int(q[2])==0, f'Missed probe: {line}'
pids={s['pid'] for s in samples};off={};intervals=defaultdict(list);stack=defaultdict(list);calls=defaultdict(list);unmatched=Counter();events=Counter();irqstack=defaultdict(list);irqs=[]
cpu_last={};runtime=[]
pattern=re.compile(r'-(\d+)\s+\[(\d+)\].*?\s(\d+)\.(\d+):\s+(\w+):\s+(.*)')
if (root/'trace.txt').exists():
 for line in (root/'trace.txt').open():
  m=pattern.search(line)
  if not m:continue
  pid,cpu=int(m[1]),int(m[2]);t=int(m[3])*10**9+int(m[4].ljust(9,'0'));e,b=m[5],m[6];events[e]+=1
  if e=='sched_switch':
   q=re.search(r'prev_pid=(\d+).*prev_state=(\S+).*next_comm=(.*?) next_pid=(\d+)',b)
   if not q:continue
   prev,state,comm,nxt=int(q[1]),q[2],q[3],int(q[4]);off[prev]=(t,state,comm,nxt)
   if cpu in cpu_last:runtime.append((cpu_last[cpu],t,prev))
   cpu_last[cpu]=t
   if nxt in off:
    a,state,comm,other=off.pop(nxt);intervals[nxt].append((a,t,state,comm,other))
  elif e in ['irq_handler_entry','softirq_entry']:
   kind='hard' if e=='irq_handler_entry' else 'soft';irqstack[(cpu,kind)].append(t)
  elif e in ['irq_handler_exit','softirq_exit']:
   kind='hard' if e=='irq_handler_exit' else 'soft'
   if irqstack[(cpu,kind)]:irqs.append((irqstack[(cpu,kind)].pop(),t))
  elif pid in pids:
   if e.endswith('_enter'):
    k=e[:-6];q=re.search(r'address=(0x[0-9a-f]+)',b);stack[(pid,k)].append((t,int(q[1],16) if q else None))
   elif e.endswith('_exit'):
    k=e[:-5]
    if stack[(pid,k)]:
     a,addr=stack[(pid,k)].pop();q=re.search(r'ret=(0x[0-9a-f]+)',b);calls[pid].append(dict(kind=k,start=a,end=t,address=addr,ret=int(q[1],16) if q else None))
    else:unmatched[k]+=1
   elif e in ['mm_vmscan_direct_reclaim_begin','mm_vmscan_memcg_reclaim_begin','mm_compaction_begin']:
    k=e.replace('_begin','');stack[(pid,k)].append((t,None))
   elif e in ['mm_vmscan_direct_reclaim_end','mm_vmscan_memcg_reclaim_end','mm_compaction_end']:
    k=e.replace('_end','')
    if stack[(pid,k)]:
     a,_=stack[(pid,k)].pop();calls[pid].append(dict(kind=k,start=a,end=t,address=None,ret=None))
def overlap(a,b,x,y):return max(0,min(b,y)-max(a,x))
def offns(pid,a,b):return sum(overlap(a,b,x,y) for x,y,*_ in intervals[pid])
merged_irqs=[]
for a,b in sorted(irqs):
 if merged_irqs and a<=merged_irqs[-1][1]:merged_irqs[-1]=(merged_irqs[-1][0],max(b,merged_irqs[-1][1]))
 else:merged_irqs.append((a,b))
irqs=merged_irqs
traced=(root/'trace.txt').exists()
for s in samples:
 pid=s['pid'];a,b=s['start_ns'],s['end_ns'];s['offcpu_ns']=offns(pid,a,b) if traced else None
 s['scheduled_wall_ns']=s['wall_ns']-s['offcpu_ns'] if traced else None
 s['wall_minus_thread_cpu_ns']=s['wall_ns']-s['cpu_ns']
 s['other_worker_ns']=sum(overlap(a,b,x,y) for x,y,p in runtime if p in pids and p!=pid) if traced else None
 s['background_ns']=sum(overlap(a,b,x,y) for x,y,p in runtime if p not in pids and p!=0) if traced else None
 matched=[c for c in calls[pid] if c['start']<b and c['end']>a]
 for k in ['sync','unmap','swapin','swapread','fault','mm_vmscan_direct_reclaim','mm_vmscan_memcg_reclaim','mm_compaction']:
  cs=[c for c in matched if c['kind']==k]
  if k=='fault':cs=[c for c in cs if c['address'] is not None and s['address']<=c['address']<s['address']+size]
  s[k+'_calls']=len(cs);s[k+'_wall_ns']=sum(overlap(a,b,c['start'],c['end']) for c in cs)
  s[k+'_scheduled_ns']=sum(overlap(a,b,c['start'],c['end'])-offns(pid,max(a,c['start']),min(b,c['end'])) for c in cs)
  s[k+'_max_wall_ns']=max([overlap(a,b,c['start'],c['end']) for c in cs] or [0])
  s[k+'_max_scheduled_ns']=max([overlap(a,b,c['start'],c['end'])-offns(pid,max(a,c['start']),min(b,c['end'])) for c in cs] or [0])
  if k=='fault':
   s['fault_major_returns']=sum(bool(c['ret']&4) for c in cs if c['ret'] is not None)
   s['fault_retry_returns']=sum(bool(c['ret']&0x400) for c in cs if c['ret'] is not None)
 s['switches']=[dict(start_ns=max(a,x),end_ns=min(b,y),state=state,next_comm=comm,next_pid=other) for x,y,state,comm,other in intervals[pid] if x<b and y>a]
 s['irq_while_scheduled_ns']=sum(overlap(a,b,x,y)-offns(pid,max(a,x),min(b,y)) for x,y in irqs if x<b and y>a) if traced else None
v=sorted(s['wall_ns'] for s in samples)
summary=dict(samples=len(samples),wall_p50_ns=st.median(v),wall_mean_ns=st.mean(v),wall_max_ns=max(v),minor=sum(s['minor'] for s in samples),major=sum(s['major'] for s in samples),trace_events=dict(events),unmatched_exits=dict(unmatched),unclosed_entries=sum(len(x) for x in stack.values()),slow=sorted(samples,key=lambda s:s['wall_ns'],reverse=True)[:12])
for name,ss in [('no_fault',[s for s in samples if s['minor']==0 and s['major']==0]),('many_faults',[s for s in samples if s['minor']>=size//4096//2])]:
 if ss:summary[name]=dict(samples=len(ss),wall_median_ns=st.median(s['wall_ns'] for s in ss),cpu_median_ns=st.median(s['cpu_ns'] for s in ss),minor_median=st.median(s['minor'] for s in ss))
(root/'analysis.json').write_text(json.dumps(summary,indent=2)+'\n')
flat=[{k:v for k,v in s.items() if k!='switches'} for s in samples]
with (root/'samples.csv').open('w') as f:w=csv.DictWriter(f,fieldnames=list(flat[0]));w.writeheader();w.writerows(flat)
print(root.name,{k:v for k,v in summary.items() if k not in ['slow','trace_events']})
for s in summary['slow'][:4]:print({k:v for k,v in s.items() if k in ['pid','index','wall_ns','cpu_ns','minor','major','offcpu_ns','scheduled_wall_ns','sync_calls','sync_scheduled_ns','unmap_scheduled_ns','fault_calls','fault_scheduled_ns','fault_max_wall_ns','fault_max_scheduled_ns','fault_major_returns','fault_retry_returns','other_worker_ns','background_ns','swapin_calls','swapread_calls','irq_while_scheduled_ns']})
