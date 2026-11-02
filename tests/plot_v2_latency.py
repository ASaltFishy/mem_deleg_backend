#!/usr/bin/env python3
"""Plot quick/full latency results without combining distinct workloads."""
import argparse
import csv
import math
import statistics
from collections import defaultdict
from pathlib import Path

METRICS = {
    'alloc_only': ('malloc', 'Malloc'),
    'dealloc_only': ('free', 'Free'),
    'hold_only': ('hold', 'Hold'),
    'all': ('all', 'Round trip: malloc + hold + free'),
}


def write_csv(path, rows):
    with path.open('w', newline='') as stream:
        writer = csv.DictWriter(stream, fieldnames=list(rows[0]))
        writer.writeheader()
        writer.writerows(rows)


def summarize(rows):
    groups = defaultdict(list)
    keys = ('allocator', 'mode', 'touch_pages', 'cores', 'workers_per_core', 'size', 'path')
    for row in rows:
        key = tuple(row[k] if k in ('allocator', 'mode', 'path') else int(row[k]) for k in keys)
        groups[key].append(row)
    summaries = []
    for key, runs in sorted(groups.items()):
        for metric in METRICS:
            means = [float(row[metric + '_mean_ns']) for row in runs]
            if any(not math.isfinite(value) or value < 0 for value in means):
                raise ValueError('Invalid latency value')
            summary = dict(zip(keys, key))
            summary.update(metric=metric, mean_ns=statistics.mean(means),
                           std_ns=statistics.stdev(means) if len(means) > 1 else 0,
                           p50_run_median_ns=statistics.median(float(row[metric + '_p50_ns']) for row in runs),
                           p95_run_median_ns=statistics.median(float(row[metric + '_p95_ns']) for row in runs),
                           p99_run_median_ns=statistics.median(float(row[metric + '_p99_ns']) for row in runs),
                           max_ns=max(float(row[metric + '_max_ns']) for row in runs),
                           repetitions=len(runs),
                           samples=sum(int(row['iterations']) * int(row['cores']) * int(row['workers_per_core']) for row in runs),
                           voluntary_cs=sum(int(row['voluntary_cs']) for row in runs),
                           involuntary_cs=sum(int(row['involuntary_cs']) for row in runs))
            routes_known = all('route_samples' in row for row in runs)
            n = sum(int(row['route_samples']) for row in runs) if routes_known else 0
            summary['delegated_pct'] = 100 * sum(int(row['route_delegated']) for row in runs) / n if n else None
            summary['fallback_pct'] = 100 * sum(int(row['route_fallback']) for row in runs) / n if n else None
            summaries.append(summary)
    return summaries


def plot(summary, allocator, stage, output, statistic):
    import matplotlib
    matplotlib.use('Agg')
    import matplotlib.pyplot as plt

    rows = [row for row in summary if row['allocator'] == allocator and row['metric'] == stage]
    panels = sorted({(r['mode'], r['touch_pages'], r['cores'], r['workers_per_core']) for r in rows})
    columns = min(2, len(panels))
    nrows = (len(panels) + columns - 1) // columns
    fig, axes = plt.subplots(nrows, columns, figsize=(7.8 * columns, 4.7 * nrows), squeeze=False)
    labels = {'traditional': 'Traditional', 'delegated': 'Delegation enabled'}
    colors = {'traditional': '#d62728', 'delegated': '#1675b9'}
    for ax, panel in zip(axes.flat, panels):
        mode, touch, cores, workers = panel
        selected = [r for r in rows if (r['mode'], r['touch_pages'], r['cores'], r['workers_per_core']) == panel]
        sizes = sorted({r['size'] for r in selected})
        for path in sorted({r['path'] for r in selected}):
            data = sorted((r for r in selected if r['path'] == path), key=lambda r: r['size'])
            x = [r['size'] / 1024 for r in data]
            y = [r[statistic] for r in data]
            color = colors.get(path)
            ax.plot(x, y, marker='o' if path == 'delegated' else 's', color=color,
                    linewidth=2, label=labels.get(path, path))
            if statistic == 'mean_ns':
                ax.fill_between(x, [max(r['mean_ns'] - r['std_ns'], r['mean_ns'] * .01, .01) for r in data],
                                [r['mean_ns'] + r['std_ns'] for r in data], color=color, alpha=.12)
            if path == 'delegated':
                for row in data:
                    if row['fallback_pct']:
                        ax.annotate(f"F {row['fallback_pct']:.3g}%", (row['size'] / 1024, row[statistic]),
                                    xytext=(0, 7), textcoords='offset points', ha='center', fontsize=7,
                                    color=color)
        ax.set_xscale('log', base=2)
        ax.set_yscale('log')
        # Equal spacing per exponent; include unsampled powers as ticks only.
        first = math.floor(math.log2(min(sizes) / 1024))
        last = math.ceil(math.log2(max(sizes) / 1024))
        exponents = range(first, last + 1)
        ax.set_xticks([2 ** exponent for exponent in exponents],
                     [rf"$2^{{{exponent}}}$" for exponent in exponents])
        ax.set_xlabel('Allocation size (KiB)')
        ax.set_ylabel('Mean wall time (ns)' if statistic == 'mean_ns' else 'Median of per-run p50 wall time (ns)')
        ax.set_title(f'{mode} | touch={touch} | {cores} CPU(s), {workers} worker(s)/CPU')
        ax.grid(which='both', linestyle='--', alpha=.25)
        ax.legend()
    for ax in list(axes.flat)[len(panels):]:
        ax.set_visible(False)
    stem, label = METRICS[stage]
    fig.suptitle(f'{allocator}: {label}', fontsize=16)
    note = 'Band: ±1 SD of run means.' if statistic == 'mean_ns' else 'Median of run p50s; not a pooled percentile.'
    fig.text(.5, .015, note + ' F: measured fallback %. Hold includes touch/use and harness checks. Round trip excludes checkpoints.',
             ha='center', fontsize=9)
    fig.canvas.draw()
    fig.tight_layout(rect=(.025, .045, .99, .95))
    suffix = '' if statistic == 'mean_ns' else '_p50'
    paths = []
    for extension in ('png', 'pdf'):
        path = output / f'{allocator}_{stem}{suffix}.{extension}'
        fig.savefig(path, dpi=180, bbox_inches='tight')
        paths.append(path)
    plt.close(fig)
    return paths


def generate(rows, output):
    output = Path(output)
    output.mkdir(parents=True, exist_ok=True)
    summary = summarize(rows)
    write_csv(output / 'summary.csv', summary)
    for allocator in sorted({r['allocator'] for r in summary}):
        for stage in METRICS:
            for statistic in ('mean_ns', 'p50_run_median_ns'):
                paths = plot(summary, allocator, stage, output, statistic)
                print('Plot:', paths[0])
    return summary


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--csv', required=True, type=Path)
    parser.add_argument('--output-dir', required=True, type=Path)
    args = parser.parse_args()
    with args.csv.open() as stream:
        rows = list(csv.DictReader(stream))
    generate(rows, args.output_dir)


if __name__ == '__main__':
    main()
