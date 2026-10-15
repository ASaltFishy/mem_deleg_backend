#!/usr/bin/env python3
import hashlib
import json
import os
from pathlib import Path
import random
import re
import statistics
import subprocess
import uuid

ROOT = Path(__file__).resolve().parent
CHECKOUT = ROOT.parent
OUT = ROOT / 'results' / 'optimization'
OUT.mkdir(exist_ok=True)
SAVED = ROOT / 'results' / 'pre-optimization'
CURRENT = CHECKOUT / 'compiler-rt/build-delegation-scudo/lib/scudo/standalone/tests/ScudoSharedArenaLatencyBench-x86_64-Test'
LEGACY = Path('/home/lrc/patent/llvm-20/compiler-rt/build-scudo-bench-user-only/lib/scudo/standalone/tests/ScudoSharedArenaLatencyBench-x86_64-Test')
PARAMS = ['--allocator', 'combined', '--iterations', '100000', '--warmup', '10000',
          '--threads', '1', '--touch-pages', '0', '--touch-pages-warmup', '0',
          '--sizes', '131072,262144']
CASES = {
    'legacy-disabled': (LEGACY, False, 'traditional', False),
    'legacy-active': (LEGACY, False, 'delegated', False),
    'before-active': (SAVED / 'scudo_historical', True, 'traditional', False),
    'after-inactive': (CURRENT, False, 'traditional', False),
    'after-active': (CURRENT, True, 'traditional', False),
    'after-eager': (CURRENT, True, 'traditional', True),
}
records = []


def environment(active, eager=False):
    result = os.environ.copy()
    result.update(SCUDO_SHARED_ARENA_ALLOW_NO_KERNEL='1',
                  MEMORY_DELEGATION_DOMAIN='md_opt_bench_' + uuid.uuid4().hex,
                  SCUDO_DELEGATION_BACKEND=str(int(active)),
                  MD_EAGER_CLEAR=str(int(eager)))
    result.pop('MEMORY_DELEGATION_RESET', None)
    return result


for repeat in range(5):
    names = list(CASES)
    random.Random(2026 + repeat).shuffle(names)
    for name in names:
        binary, active, bench_path, eager = CASES[name]
        text = subprocess.check_output(['taskset', '-c', '0', str(binary),
                                        '--path', bench_path] + PARAMS,
                                       env=environment(active, eager),
                                       stderr=subprocess.STDOUT, text=True, timeout=60)
        (OUT / f'scudo-{name}-{repeat}.log').write_text(text)
        rows = re.findall(r'^\s*all\s+mean=([\d.]+)\s+p50=(\d+)\s+p95=(\d+)\s+p99=(\d+)',
                          text, re.M)
        assert len(rows) == 2, text
        for size, row in zip((131072, 262144), rows):
            records.append(dict(allocator='scudo-historical', case=name, repeat=repeat,
                                size=size, mean_ns=float(row[0]), p50=int(row[1]),
                                p95=int(row[2]), p99=int(row[3])))

for repeat in range(5):
    cases = [('before-active', SAVED / 'jemalloc_test', True, False),
             ('before-inactive', SAVED / 'jemalloc_test', False, False),
             ('after-active', ROOT / 'build/jemalloc_test', True, False),
             ('after-inactive', ROOT / 'build/jemalloc_test', False, False),
             ('after-eager', ROOT / 'build/jemalloc_test', True, True)]
    random.Random(2126 + repeat).shuffle(cases)
    for name, binary, active, eager in cases:
        text = subprocess.check_output(['taskset', '-c', '0', str(binary),
                                        'active' if active else 'inactive'],
                                       env=environment(active, eager),
                                       stderr=subprocess.STDOUT, text=True, timeout=60)
        (OUT / f'jemalloc-{name}-{repeat}.log').write_text(text)
        assert 'allocator_api_test: PASS' in text
        for size, mean, p50, p95, p99 in re.findall(
                r'size=(\d+) active=\d mean_ns=([\d.]+) p50=(\d+) p95=(\d+) p99=(\d+)', text):
            records.append(dict(allocator='jemalloc-api', case=name, repeat=repeat,
                                size=int(size), mean_ns=float(mean), p50=int(p50),
                                p95=int(p95), p99=int(p99)))

summary = []
for key in sorted({(row['allocator'], row['case'], row['size']) for row in records}):
    rows = [row for row in records if (row['allocator'], row['case'], row['size']) == key]
    values = [row['mean_ns'] for row in rows]
    summary.append(dict(allocator=key[0], case=key[1], size=key[2],
                        mean_ns=statistics.mean(values), min_ns=min(values), max_ns=max(values),
                        mean_p99_ns=statistics.mean(row['p99'] for row in rows)))
(OUT / 'samples.json').write_text(json.dumps(records, indent=2) + '\n')
(OUT / 'summary.json').write_text(json.dumps(summary, indent=2) + '\n')
binaries = {str(binary) for binary, *_ in CASES.values()}
binaries.update(str(ROOT / 'build' / name) for name in ('scudo_test', 'jemalloc_test'))
binaries.update(str(SAVED / name) for name in ('scudo_test', 'jemalloc_test'))
(OUT / 'binaries.json').write_text(json.dumps(
    {name: hashlib.sha256(Path(name).read_bytes()).hexdigest() for name in binaries}, indent=2) + '\n')
print(json.dumps(summary, indent=2), flush=True)

for name in ('before-active', 'after-active', 'after-eager'):
    binary, active, bench_path, eager = CASES[name]
    params = ['--allocator', 'combined', '--path', bench_path, '--iterations', '1000',
              '--warmup', '100', '--threads', '1', '--touch-pages', '0', '--sizes', '262144']
    subprocess.run(['strace', '-f', '-c', '-e', 'trace=mprotect,getpid', '-o',
                    str(OUT / f'syscalls-{name}.txt'), 'taskset', '-c', '0', str(binary)] + params,
                   env=environment(active, eager), stdout=subprocess.DEVNULL,
                   stderr=subprocess.PIPE, check=True, timeout=60)
