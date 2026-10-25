import re, json, sys, statistics
from pathlib import Path

root = Path(sys.argv[1])
text = (root / 'bench.log').read_text()
samples = [dict((k, int(v)) for k, v in re.findall(r'(\w+)=(\d+)', line))
           for line in text.splitlines() if line.startswith('ALLOC_SAMPLE ')]
events = []
off = {}
intervals = {}
for line in (root / 'trace.txt').read_text().splitlines():
    m = re.search(r'\s(\d+)\.(\d+): (\w+): (.*)', line)
    if not m:
        continue
    ts = int(m[1]) * 10**9 + int(m[2].ljust(9, '0'))
    event, body = m[3], m[4]
    events.append((ts, event, body))
    if event == 'sched_switch':
        p = re.search(r'prev_pid=(\d+).*prev_state=(\S+).*next_comm=(.*?) next_pid=(\d+)', body)
        if not p:
            continue
        prev, state, comm, nxt = int(p[1]), p[2], p[3], int(p[4])
        off[prev] = (ts, state, comm, nxt)
        if nxt in off:
            start, state, comm, other = off.pop(nxt)
            intervals.setdefault(nxt, []).append((start, ts, state, comm, other))

slow = []
for sample in samples:
    a, b = sample['start_ns'], sample['end_ns']
    matches = [(max(a, x), min(b, y), state, comm, other)
               for x, y, state, comm, other in intervals.get(sample['pid'], [])
               if x < b and y > a]
    sample['offcpu_ns'] = sum(y-x for x, y, *_ in matches)
    sample['on_cpu_wall_ns'] = sample['wall_ns'] - sample['offcpu_ns']
    if sample['wall_ns'] >= 100_000:
        sample['switches'] = matches
        sample['events'] = [(t, event, body) for t, event, body in events if a <= t <= b]
        slow.append(sample)

slow.sort(key=lambda x: x['wall_ns'], reverse=True)
out = {
    'samples': len(samples), 'median_ns': statistics.median(s['wall_ns'] for s in samples),
    'mean_ns': statistics.mean(s['wall_ns'] for s in samples),
    'max_ns': max(s['wall_ns'] for s in samples), 'slow_ge_100us': len(slow),
    'slow': slow,
}
(root / 'analysis.json').write_text(json.dumps(out, indent=2)+'\n')
print(json.dumps({k:v for k,v in out.items() if k != 'slow'}, indent=2))
for s in slow[:15]:
    print('SLOW', {k:v for k,v in s.items() if k not in ('events',)},
          'events=', len(s['events']))
    for e in s['events'][:8]: print(' ', e)
