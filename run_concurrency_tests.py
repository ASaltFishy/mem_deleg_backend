#!/usr/bin/env python3
import os
from pathlib import Path
import re
import subprocess

ROOT = Path(__file__).resolve().parent
OUT = ROOT / 'results/concurrency'
OUT.mkdir(parents=True, exist_ok=True)
cpus, seen = [], set()
for row in subprocess.check_output(['lscpu', '-p=CPU,CORE,SOCKET'], text=True).splitlines():
    if row.startswith('#'):
        continue
    cpu, core, socket = map(int, row.split(','))
    if cpu in os.sched_getaffinity(0) and (socket, core) not in seen:
        seen.add((socket, core))
        cpus.append(cpu)
if len(cpus) < 2:
    print('concurrency_test: SKIP needs two allowed physical cores')
    raise SystemExit(0)
base = dict(os.environ, MD_SINGLE_ARENA='0', MD_RSEQ='1', MD_EAGER_CLEAR='0')
command = [str(ROOT / 'build/concurrency_test')] + list(map(str, cpus[:2]))
available = False
for name, env in [('correctness', base),
                  ('correctness-no-rseq', dict(base, MD_RSEQ='0')),
                  ('correctness-unavailable-rseq', dict(base, GLIBC_TUNABLES='glibc.pthread.rseq=0'))]:
    text = subprocess.check_output(command, env=env, text=True, timeout=120)
    (OUT / f'{name}.txt').write_text(text)
    print(text, end='')
    if name == 'correctness':
        available = int(re.search(r'rseq=(\d+)', text)[1]) > 0
if available:
    text = subprocess.check_output([str(ROOT / 'build/rseq_abort_test')] + list(map(str, cpus[:2])),
                                   env=base, text=True, timeout=120)
    (OUT / 'rseq-abort.txt').write_text(text)
    print(text, end='')
else:
    print('rseq_abort_test: SKIP libc/kernel rseq registration unavailable')
