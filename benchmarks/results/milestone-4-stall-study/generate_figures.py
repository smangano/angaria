#!/usr/bin/env python3
"""Reconstruct retained study results before plotting ordinary IPC timings."""
import csv
import gzip
import json
from pathlib import Path
import statistics
import sys
import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt

sys.dont_write_bytecode = True
sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
import run_stall_study as study
HERE = Path(__file__).resolve().parent
PROFILES = study.PROFILES
COLORS = {'angaria': '#3456a4', 'recorded-spsc': '#168469', 'nanomsg': '#cc702d'}
LABELS = {'angaria': 'Current Angaria: recorded + acknowledged',
          'recorded-spsc': 'Experimental SPSC: current log, no application ACK',
          'nanomsg': 'libnanomsg IPC: unrecorded, unacknowledged'}
METRICS = (('logical_start_to_delivery_ns', 'Logical start → delivery (µs)', 1000),
           ('scheduled_to_delivery_ns', 'Scheduled release → delivery (µs)', 1000),
           ('bounded_cohort_verified_per_second', 'Verified publications/s', 1))


def reports(directory, leaf):
    with gzip.open(directory / (leaf + '.jsonl.gz'), 'rt') as stream:
        rows = [json.loads(line) for line in stream]
    for row in rows:
        case = {name: row[name] for name in study.ipc.DIMENSIONS + study.EXTRA_DIMENSIONS}
        for role, worker in row['worker_reports'].items():
            study.ipc.common_report(worker, role, row['profile'], case, '0.4.31')
        study.validate_attempt(row, case, leaf.startswith('diagnostics-'))
    return rows


def run_stat(row, metric):
    if metric in row['metrics']:
        return study.ipc.rank(row['metrics'][metric], 1, 2)
    return row['rates'][metric]


def save(fig, name):
    for suffix in ('png', 'svg'):
        target = HERE / (name + '.' + suffix)
        fig.savefig(target, dpi=160)
        if suffix == 'svg':
            target.write_text('\n'.join(line.rstrip() for line in target.read_text().splitlines()) + '\n')
    plt.close(fig)


def plot(directory):
    data = reports(directory, 'ordinary-concurrent')
    assert len(data) == 120 and all(row['status'] == 'complete' for row in data), 'Primary matrix must retain all 120 completed attempts'
    table = []
    fig, axes = plt.subplots(3, 2, figsize=(12.5, 9), constrained_layout=True)
    for column, size in enumerate((64, 256)):
        for index, (metric, label, scale) in enumerate(METRICS):
            ax = axes[index, column]
            for profile in PROFILES:
                center, low, high = [], [], []
                for history in (0, 128):
                    group = [row for row in data if row['profile'] == profile and row['history'] == history and row['payload_bytes'] == size]
                    assert len(group) == 10
                    values = [run_stat(row, metric) / scale for row in group]
                    median = statistics.median(values)
                    center.append(median); low.append(median - min(values)); high.append(max(values) - median)
                    table.append(dict(host=directory.name, profile=profile, history=history, payload_bytes=size,
                                      metric=metric.replace('_ns', '_us'), runs=10, median=median, minimum=min(values), maximum=max(values)))
                ax.errorbar([0, 128], center, yerr=[low, high], marker='o', capsize=3, linewidth=1.6,
                            color=COLORS[profile], label=LABELS[profile])
            ax.set_yscale('log'); ax.set_xticks([0, 128]); ax.grid(axis='y', alpha=.2)
            ax.set_xlabel('Prior logged messages H')
            if column == 0: ax.set_ylabel(label)
            if index == 0: ax.set_title(f'{size}-byte payload')
    handles, labels = axes[0, 0].get_legend_handles_labels()
    fig.legend(handles, labels, loc='outside lower center', ncol=1, frameon=False)
    fig.suptitle(f'1:1 IPC current-log SPSC experiment — {directory.name}\n'
                 '16-message bursts · 32-slot Angaria rings · median of 10 runs; bars show full run range\n'
                 'Different delivery guarantees; bounded-cohort rates are not sustained capacity', fontsize=12)
    save(fig, 'latency-throughput-' + directory.name)

    findings = []
    concurrent = reports(directory, 'diagnostics-concurrent')
    backlog = reports(directory, 'diagnostics-backlog')
    empty = reports(directory, 'diagnostics-empty')
    for name, rows in (('concurrent', concurrent), ('backlog', backlog), ('empty', empty)):
        for history in (0, 128):
            group = [row for row in rows if row['history'] == history]
            assert len(group) == 3
            totals = dict(wall_ns=0, native_ns=0, local_ns=0, hold_ns=0, empty_polls=0, prefix_scans=0,
                          consumer_cpu_ns=0, producer_cpu_ns=0)
            for row in group:
                resources = row['resources']['worker_process_phase_cpu_ns']
                totals['consumer_cpu_ns'] += resources['consumer']; totals['producer_cpu_ns'] += resources['producer']
                for role, worker in row['worker_reports'].items():
                    for op in worker['diagnostics']['operations']:
                        if role == 'producer':
                            totals['wall_ns'] += op['wall_ns']
                            totals['native_ns'] += op['phases']['native_gate_wait']['exclusive_ns']
                            totals['local_ns'] += op['phases']['local_gate_wait']['exclusive_ns']
                        else:
                            totals['hold_ns'] += op['counters']['native_gate_hold_ns']
                            totals['empty_polls'] += op['counters']['empty_polls']
                            totals['prefix_scans'] += op['counters']['prefix_scans']
            findings.append(dict(scenario=name, history=history, runs=3, **totals,
                                 native_fraction_of_producer_api=totals['native_ns'] / totals['wall_ns'] if totals['wall_ns'] else None))
    return table, findings


def main():
    table, findings = [], {}
    for host in ('macos-arm64', 'linux-x86_64'):
        directory = HERE / host
        if directory.is_dir():
            rows, diagnostic = plot(directory)
            table.extend(rows); findings[host] = diagnostic
    assert table, 'No retained study data'
    with (HERE / 'comparison.csv').open('w', newline='') as stream:
        writer = csv.DictWriter(stream, fieldnames=list(table[0]), lineterminator='\n')
        writer.writeheader(); writer.writerows(table)
    (HERE / 'diagnostic-findings.json').write_text(json.dumps(findings, indent=2) + '\n')

if __name__ == '__main__':
    main()
