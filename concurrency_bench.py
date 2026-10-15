#!/usr/bin/env python3
"""Same workload, historical/global/per-arena/rseq lock comparisons."""
import json
import os
from pathlib import Path
import random
import re
import statistics
import subprocess
import uuid

ROOT = Path(__file__).resolve().parent
OUT = ROOT / 'results/concurrency'
cpus = []
seen = set()
for row in subprocess.check_output(['lscpu', '-p=CPU,CORE,SOCKET'], text=True).splitlines():
    if row.startswith('#'):
        continue
    cpu, core, socket = map(int, row.split(','))
    if cpu in os.sched_getaffinity(0) and (socket, core) not in seen:
        seen.add((socket, core))
        cpus.append(cpu)
assert len(cpus) >= 2
cases = {
    'historical-global': (OUT / 'before/contention_bench', '1', '0'),
    'global-futex': (ROOT / 'build/contention_bench', '1', '0'),
    'per-arena': (ROOT / 'build/contention_bench', '0', '0'),
    'per-arena-rseq': (ROOT / 'build/contention_bench', '0', '1'),
}
records = []
for repeat in range(5):
    jobs = [(name, count) for name in cases for count in (1, 2, 4, 8)
            if count <= len(cpus)]
    random.Random(20261013 + repeat).shuffle(jobs)
    for name, count in jobs:
        binary, single, rseq = cases[name]
        env = dict(os.environ, MD_SINGLE_ARENA=single, MD_RSEQ=rseq, MD_EAGER_CLEAR='0')
        text = subprocess.check_output([str(binary)] + list(map(str, cpus[:count])),
                                       env=env, text=True, timeout=120)
        (OUT / f'{name}-{count}-{repeat}.log').write_text(text)
        values = {key: float(value) for key, value in re.findall(r'(\w+)=([\d.]+)', text)}
        records.append(dict(case=name, repeat=repeat, **values))
summary = []
for name in cases:
    for count in (1, 2, 4, 8):
        rows = [r for r in records if r['case'] == name and r['threads'] == count]
        if not rows:
            continue
        summary.append(dict(case=name, threads=count,
                            **{key: statistics.mean(r[key] for r in rows)
                               for key in rows[0] if key not in ('case', 'repeat', 'threads')}))
structures = []
for repeat in range(5):
    for name, binary in [('historical', OUT / 'before/structure_bench'),
                         ('current', ROOT / 'build/structure_bench')]:
        text = subprocess.check_output(['taskset', '-c', str(cpus[0]), str(binary)],
                                       env=dict(os.environ, MD_SINGLE_ARENA='0', MD_RSEQ='1'),
                                       text=True, timeout=60)
        structures.append(dict(case=name, repeat=repeat,
            **{k: float(v) for k, v in re.findall(r'(\w+)=([\d.]+)', text)}))
(OUT / 'cpus.json').write_text(json.dumps(cpus, indent=2) + '\n')
(OUT / 'samples.json').write_text(json.dumps(records, indent=2) + '\n')
(OUT / 'summary.json').write_text(json.dumps(summary, indent=2) + '\n')
(OUT / 'structures.json').write_text(json.dumps(structures, indent=2) + '\n')
print(json.dumps(summary, indent=2))
print(json.dumps(structures, indent=2))

allocator_records = []
for repeat in range(5):
    jobs = [(allocator, version) for allocator in ('scudo', 'jemalloc')
            for version in ('before', 'after', 'after-no-rseq')]
    random.Random(30261013 + repeat).shuffle(jobs)
    for allocator, version in jobs:
        binary = ((OUT / 'before' if version == 'before' else ROOT / 'build') /
                  f'{allocator}_test')
        env = dict(os.environ, MD_SINGLE_ARENA='0', MD_EAGER_CLEAR='0',
                   MD_RSEQ='0' if version == 'after-no-rseq' else '1',
                   MEMORY_DELEGATION_DOMAIN='md_concurrency_api_' + uuid.uuid4().hex)
        text = subprocess.check_output(['taskset', '-c', str(cpus[0]), str(binary), 'active'],
                                       env=env, text=True, timeout=120)
        assert 'allocator_api_test: PASS' in text
        (OUT / f'api-{allocator}-{version}-{repeat}.log').write_text(text)
        for size, mean in re.findall(r'size=(\d+) active=1 mean_ns=([\d.]+)', text):
            allocator_records.append(dict(allocator=allocator, version=version,
                repeat=repeat, size=int(size), mean_ns=float(mean)))
(OUT / 'allocator-samples.json').write_text(json.dumps(allocator_records, indent=2) + '\n')
