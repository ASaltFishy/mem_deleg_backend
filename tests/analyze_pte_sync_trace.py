import re, json, sys, statistics as st
from pathlib import Path
from collections import defaultdict, Counter

root = Path(sys.argv[1])
bench = (root / 'bench.log').read_text()
samples = [dict((k, int(v)) for k,v in re.findall(r'(\w+)=(\d+)',line))
           for line in bench.splitlines() if line.startswith('ALLOC_SAMPLE ')]
windows = {}
for s in samples:
    p=s['pid']; a,b=windows.get(p,(s['start_ns'],s['end_ns']))
    windows[p]=(min(a,s['start_ns']),max(b,s['end_ns']))
active=defaultdict(list); off={}; off_intervals=defaultdict(list); calls=[]
for line in (root/'trace.txt').read_text().splitlines():
    m=re.search(r'-(\d+)\s+\[000\].*? (\d+)\.(\d+): (\w+): (.*)',line)
    if not m: continue
    pid=int(m[1]); t=int(m[2])*10**9+int(m[3].ljust(9,'0')); event,body=m[4],m[5]
    if event=='sched_switch':
        q=re.search(r'prev_pid=(\d+).*next_pid=(\d+)',body)
        prev,nxt=map(int,q.groups()); off[prev]=t
        if nxt in off: off_intervals[nxt].append((off.pop(nxt),t))
    elif event=='sync_enter': active[pid].append({'pid':pid,'start':t,'zaps':[]})
    elif event=='unmap_enter' and active[pid]:
        q=dict(re.findall(r'(start|len)=(\S+)',body))
        active[pid][-1]['zaps'].append((q.get('start'),q.get('len')))
    elif event=='sync_exit' and active[pid]:
        c=active[pid].pop(); c['end']=t; calls.append(c)

worker=[]; steady=[]
for c in calls:
    if c['pid'] not in windows: continue
    c['wall_ns']=c['end']-c['start']
    c['offcpu_ns']=sum(max(0,min(c['end'],b)-max(c['start'],a))
                       for a,b in off_intervals[c['pid']])
    c['scheduled_wall_ns']=c['wall_ns']-c['offcpu_ns']
    worker.append(c)
    a,b=windows[c['pid']]
    if a<=c['start'] and c['end']<=b: steady.append(c)

def summarize(cs):
    result={'calls':len(cs),'zaps':sum(len(c['zaps']) for c in cs),
            'distinct_exact_ranges_per_call_sum':sum(len(set(c['zaps'])) for c in cs)}
    for field in ['wall_ns','scheduled_wall_ns']:
        v=sorted(c[field] for c in cs)
        if v: result[field]={'mean':st.mean(v),'p50':v[int(.5*(len(v)-1))],
                            'p95':v[int(.95*(len(v)-1))],'max':max(v)}
    return result
v=sorted(s['wall_ns'] for s in samples)
result={'samples':len(v),'alloc_mean_ns':st.mean(v),'alloc_p50_ns':v[len(v)//2],
        'alloc_max_ns':max(v),'worker_sync':summarize(worker),'steady_sync':summarize(steady)}
(root/'sync-analysis.json').write_text(json.dumps(result,indent=2)+'\n')
print(json.dumps(result,indent=2))
for c in sorted(steady,key=lambda c:len(c['zaps']),reverse=True)[:3]:
    print('TOP_REPLAY',c['pid'],c['scheduled_wall_ns'],len(c['zaps']),Counter(c['zaps']).most_common(4))
