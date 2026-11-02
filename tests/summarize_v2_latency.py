#!/usr/bin/env python3
"""Save quick/full case metrics and worker measurement-loop switch counters."""
import argparse
import csv
import re
from pathlib import Path


def parse(text):
    rows, current = [], None

    def finish():
        if current is None:
            return
        required = ['voluntary_cs', 'involuntary_cs', 'iterations', 'warmup']
        required += [metric + '_mean_ns' for metric in ('alloc_only', 'hold_only', 'dealloc_only', 'all')]
        if any(key not in current for key in required):
            raise ValueError('Incomplete case: ' + str(current))
        if 'index' in current and not current.get('case_pass'):
            raise ValueError('Case has no PASS marker')
        rows.append(current.copy())

    for line in text.splitlines():
        if line.startswith('V2_PHONE_CASE '):
            finish()
            current = dict(re.findall(r'(\w+)=(\S+)', line))
        elif line.startswith('V2_PHONE_CASE_FAIL '):
            raise ValueError(line)
        elif line.startswith('V2_PHONE_CASE_PASS '):
            if current is None or int(line.split('index=')[1]) != int(current['index']):
                raise ValueError('Mismatched case PASS index')
            current['case_pass'] = 1
        elif current is not None and line.startswith('bench_routes: '):
            current.update({'route_' + key: int(value) for key, value in
                            re.findall(r'\b(primary|delegated|secondary|fallback|samples)=(\d+)', line)})
            if sum(current['route_' + key] for key in ('primary', 'delegated', 'secondary')) != current['route_samples']:
                raise ValueError('Scudo route counts do not match samples')
            # Primary is a distinct route, not a failed delegation attempt.
            expected_fallback = current['route_secondary'] if current['path'] == 'delegated' else 0
            if current['route_fallback'] != expected_fallback:
                raise ValueError('Scudo fallback count does not match actual routes')
        elif current is not None and line.startswith('bench_jemalloc_routes: '):
            current.update({'route_' + key: int(value) for key, value in
                            re.findall(r'\b(delegated|ordinary|fallback|samples)=(\d+)', line)})
            if current['route_delegated'] + current['route_ordinary'] != current['route_samples']:
                raise ValueError('jemalloc route counts do not match samples')
            expected_fallback = current['route_ordinary'] if current['path'] == 'delegated' else 0
            if current['route_fallback'] != expected_fallback:
                raise ValueError('jemalloc fallback count does not match actual routes')
        elif current is not None and line.startswith('bench_noise: '):
            current.update({key: int(value) for key, value in
                            re.findall(r'(minor_faults|major_faults|voluntary_cs|involuntary_cs)=(\d+)', line)})
            current['switch_scope'] = 'worker_sum_measurement_loop'
        elif current is not None and line.startswith('bench_lifecycle: '):
            if 'pre_checkpoint=ok post_checkpoint=ok' not in line:
                raise ValueError('Checkpoint failed')
            current.update(dict(re.findall(r'(warmup|iterations|build_id|perf_minimal|arena_capacity)=([^\s]+)', line)))
        elif current is not None and line.startswith('bench_config: '):
            current.update({key: int(value) for key, value in
                            re.findall(r'(cores|workers_per_core|touch_pages)=(\d+)', line)})
        elif current is not None and line.startswith('bench_trace: ') and 'diagnostic_only=1' in line:
            raise ValueError('Diagnostic instrumentation cannot be used for performance results')
        elif current is not None:
            match = re.match(r'\s*(alloc_only|hold_only|dealloc_only|all)\s+mean=', line)
            if match:
                current.update({match[1] + '_' + key + '_ns': float(value)
                                for key, value in re.findall(r'(mean|p50|p95|p99|max)=([0-9.eE+-]+)', line)})
    finish()
    if not rows or 'V2_PHONE_ALL_PASS' not in text:
        raise ValueError('No complete successful sweep')
    if 'total' in rows[0]:
        if len(rows) != int(rows[0]['total']) or [int(row['index']) for row in rows] != list(range(1, len(rows)+1)):
            raise ValueError('Matrix progress indices are incomplete')
    return rows


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--log', required=True, type=Path)
    parser.add_argument('--output-dir', required=True, type=Path)
    args = parser.parse_args()
    rows = parse(args.log.read_text())
    args.output_dir.mkdir(parents=True, exist_ok=True)
    destination = args.output_dir / 'raw_results.csv'
    fields = list(dict.fromkeys(key for row in rows for key in row))
    with destination.open('w', newline='') as stream:
        writer = csv.DictWriter(stream, fieldnames=fields)
        writer.writeheader()
        writer.writerows(rows)
    print('Data:', destination)
    from plot_v2_latency import generate
    generate(rows, args.output_dir)


if __name__ == '__main__':
    main()
