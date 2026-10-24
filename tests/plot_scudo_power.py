#!/usr/bin/env python3
"""Parse actual process benchmark output and plot the power-of-two sweep."""
import argparse
import csv
from pathlib import Path
import re
import statistics

VARIANTS = ('cache', 'mmap', 'delegated')
LABELS = {'cache': 'Traditional (cache enabled)', 'mmap': 'Traditional (cache disabled)',
          'delegated': 'Delegation enabled (fallback allowed)'}


def parse_log(text):
    rows = []
    current = None
    for line in text.splitlines():
        if line.startswith('POWER_CASE_BEGIN '):
            if current is not None:
                raise ValueError('Previous case has no PASS marker')
            current = {k: (int(v) if k != 'variant' else v)
                       for k, v in (item.split('=', 1) for item in line.split()[1:])}
        elif current is not None and line.startswith('bench_routes: '):
            current.update({k: int(v) for k, v in re.findall(r'(\w+)=(\d+)', line)})
        elif current is not None and line.startswith('bench_lifecycle: '):
            if 'pre_checkpoint=ok post_checkpoint=ok' not in line:
                raise ValueError('checkpoint failed')
            current['checkpoint_ok'] = 1
        elif current is not None and re.match(r'\s*alloc_only\s+mean=', line):
            current.update({'alloc_' + k: float(v) for k, v in
                            re.findall(r'(mean|p50|p95|p99)=([0-9.eE+-]+)', line)})
        elif line.startswith('POWER_CASE_FAIL'):
            raise ValueError(line)
        elif line.startswith('POWER_CASE_PASS '):
            if current is None or int(line.split('index=')[1]) != current['index']:
                raise ValueError('Mismatched PASS marker')
            needed = ('primary', 'delegated', 'secondary', 'cache_calls', 'cache_hits',
                      'fallback', 'samples', 'alloc_mean', 'checkpoint_ok')
            if any(k not in current for k in needed):
                raise ValueError(f'Incomplete case: {current}')
            if current['primary'] + current['delegated'] + current['secondary'] != current['samples']:
                raise ValueError('Route counts do not match samples')
            if current['cache_hits'] > current['secondary']:
                raise ValueError('Cache hits exceed ordinary Secondary allocations')
            rows.append(current)
            current = None
    if current is not None or not rows:
        raise ValueError('No complete results or unfinished last case')
    if len(rows) != rows[0]['total'] or any(r['index'] != i for i, r in enumerate(rows, 1)):
        raise ValueError('Sweep is incomplete')
    return rows


def summarize(rows):
    groups = {}
    for row in rows:
        groups.setdefault((row['processes'], row['exponent'], row['variant']), []).append(row)
    result = []
    for (processes, exponent, variant), runs in sorted(groups.items()):
        means = [r['alloc_mean'] for r in runs]
        n = sum(r['samples'] for r in runs)
        secondary = sum(r['secondary'] for r in runs)
        hits = sum(r['cache_hits'] for r in runs)
        result.append(dict(processes=processes, exponent=exponent, kib=2**exponent,
                           variant=variant, mean_ns=statistics.mean(means),
                           std_ns=statistics.stdev(means) if len(means) > 1 else 0,
                           repetitions=len(runs), samples=n,
                           primary_pct=100*sum(r['primary'] for r in runs)/n,
                           delegated_pct=100*sum(r['delegated'] for r in runs)/n,
                           fallback_pct=100*sum(r['fallback'] for r in runs)/n,
                           secondary_pct=100*secondary/n,
                           cache_hit_pct=100*hits/secondary if secondary else 0,
                           cache_hits=hits, secondary_allocations=secondary))
    return result


def write_csv(path, rows):
    with path.open('w', newline='') as f:
        writer = csv.DictWriter(f, fieldnames=list(rows[0]))
        writer.writeheader()
        writer.writerows(rows)


def plot(summary, output):
    import matplotlib
    matplotlib.use('Agg')
    import matplotlib.pyplot as plt
    colors = {'cache': '#d62728', 'mmap': '#ff8c00', 'delegated': '#1675b9'}
    markers = {'cache': 's', 'mmap': '^', 'delegated': 'o'}
    fig, axes = plt.subplots(1, 3, figsize=(21, 6), squeeze=False)
    for ax, processes in zip(axes[0], (1, 2, 4)):
        for variant in VARIANTS:
            data = [r for r in summary if r['processes'] == processes and r['variant'] == variant]
            x = [r['exponent'] for r in data]
            y = [r['mean_ns'] for r in data]
            sd = [r['std_ns'] for r in data]
            ax.plot(x, y, marker=markers[variant], color=colors[variant], linewidth=2,
                    markersize=4, label=LABELS[variant])
            ax.fill_between(x, [max(m-s, m*0.01) for m, s in zip(y, sd)],
                            [m+s for m, s in zip(y, sd)], color=colors[variant], alpha=.12)
            for r in data:
                note = ''
                if variant == 'cache' and r['secondary_allocations']:
                    note = f"H {r['cache_hit_pct']:.0f}%"
                elif variant == 'delegated' and r['fallback_pct']:
                    note = f"F {r['fallback_pct']:.0f}%"
                if note:
                    ax.annotate(note, (r['exponent'], r['mean_ns']), xytext=(0, 8),
                                textcoords='offset points', ha='center', fontsize=6,
                                color=colors[variant])
        by_exp = {}
        for r in summary:
            if r['processes'] == processes:
                by_exp.setdefault(r['exponent'], {})[r['variant']] = r
        for e, variants in by_exp.items():
            if set(variants) == set(VARIANTS):
                c, d = variants['cache'], variants['delegated']
                ax.annotate(f"{c['mean_ns']/d['mean_ns']:.2f}x", (e, d['mean_ns']),
                            xytext=(0, -13), textcoords='offset points', ha='center', fontsize=6)
        exps = sorted(by_exp)
        primary = [e for e, variants in by_exp.items()
                   if all(r['primary_pct'] == 100 for r in variants.values())]
        if primary:
            ax.axvspan(min(primary)-.25, max(primary)+.25, alpha=.07, color='green', label='Observed Primary only')
        ax.set_title(f'{processes} process' + ('es' if processes != 1 else '') + ' on one CPU')
        ax.set_yscale('log')
        ticks = sorted(set(exps[::2] + exps[-1:]))
        ax.set_xticks(ticks, [rf'$2^{{{e}}}$' for e in ticks])
        ax.set_xticks(exps, minor=True)
        ax.set_xlabel('Allocation size (KiB)')
        ax.set_ylabel('alloc_only mean latency (ns)')
        ax.grid(which='both', linestyle='--', alpha=.25)
        ax.legend(fontsize=7, loc='best')
    fig.suptitle('Scudo allocation latency: Primary / Secondary cache / mmap / delegation', fontsize=16)
    fig.text(.5, .01, 'Band: ±1 sample SD across repeated runs.  x: cache-enabled / delegation latency. '
             'H: measured Secondary cache hit %.  F: measured fallback %.  Green: observed Primary only.',
             ha='center', fontsize=9)
    fig.tight_layout(rect=(0, .045, 1, .94))
    for suffix in ('png', 'pdf'):
        fig.savefig(output / f'scudo_alloc_only.{suffix}', dpi=180)
    plt.close(fig)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--log', type=Path, required=True)
    parser.add_argument('--output-dir', type=Path, required=True)
    args = parser.parse_args()
    rows = parse_log(args.log.read_text())
    args.output_dir.mkdir(parents=True, exist_ok=True)
    summary = summarize(rows)
    write_csv(args.output_dir/'raw_results.csv', rows)
    write_csv(args.output_dir/'summary.csv', summary)
    plot(summary, args.output_dir)
    print(f'Plots: {args.output_dir / "scudo_alloc_only.png"}')
    print(f'Data: {args.output_dir / "raw_results.csv"}')


if __name__ == '__main__':
    main()
