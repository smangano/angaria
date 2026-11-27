#!/usr/bin/env python3
"""Plot retained bounded IPC cohorts; no timing outlier selection."""
import csv
import gzip
import json
from pathlib import Path
import statistics
import sys
import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT))
from run_ipc_comparison import common_report, validate_cached_summary
HERE = Path(__file__).resolve().parent
LABELS = {'angaria': 'Angaria 0.4.30: recorded, acknowledged',
          'nanomsg': 'libnanomsg 1.3.0 IPC: unrecorded, unacknowledged'}
COLORS = {'angaria': '#3456a4', 'nanomsg': '#cc702d'}
METRICS = [('logical_start_to_delivery_ns', 'Logical start → delivery (µs)', 1000),
           ('scheduled_to_delivery_ns', 'Scheduled release → delivery (µs)', 1000),
           ('bounded_cohort_verified_per_second', 'Verified publications/s', 1)]

def load(directory):
    with gzip.open(directory / 'reports.jsonl.gz', 'rt') as stream:
        reports = [json.loads(line) for line in stream]
    assert len(reports) == 180, 'Expected the entire five-block nine-cell matrix'
    for report in reports:
        assert report['status'] == 'complete', 'Incomplete attempts need separate presentation'
        workers = report['worker_reports']
        for role, worker in workers.items():
            common_report(worker, role, report['profile'], report, '0.4.30')
        verified = validate_cached_summary(report)
        assert verified['delivered'] == verified['accepted'] == report['messages'] == 16
        assert verified['pending'] == 0
        # Reject stale cached summaries before using reconstructed measurements.
        report.update(verified)
    return reports


def plot(directory):
    reports = load(directory)
    fig, axes = plt.subplots(3, 3, figsize=(13, 9.5), constrained_layout=True)
    for column, payload in enumerate((64, 256, 1024)):
        for row, (metric, label, divisor) in enumerate(METRICS):
            ax = axes[row, column]
            for profile in LABELS:
                medians, lower, upper = [], [], []
                for history in (0, 32, 128):
                    cells = [r for r in reports if r['profile'] == profile and
                             r['history'] == history and r['payload_bytes'] == payload]
                    assert len(cells) == 10
                    values = [(statistics.median_low(sorted(r['metrics'][metric])) if metric in r['metrics']
                               else r['rates'][metric]) / divisor for r in cells]
                    median = statistics.median(values)
                    medians.append(median)
                    lower.append(median - min(values))
                    upper.append(max(values) - median)
                ax.errorbar([0, 32, 128], medians, yerr=[lower, upper], marker='o',
                            capsize=3, linewidth=1.6, color=COLORS[profile], label=LABELS[profile])
            ax.set_yscale('log')
            ax.set_xticks([0, 32, 128])
            ax.grid(axis='y', alpha=.2)
            ax.set_xlabel('Prior messages H')
            if column == 0:
                ax.set_ylabel(label)
            if row == 0:
                ax.set_title(f'{payload}-byte payload')
    handles, labels = axes[0, 0].get_legend_handles_labels()
    fig.legend(handles, labels, loc='outside lower center', ncol=2, frameon=False)
    fig.suptitle(f'1:1 independent-process IPC — {directory.name}\n'
                 '16-message bursts · median across 10 runs/cell; bars show run range · logarithmic axes\n'
                 'Different guarantees; bounded-cohort rates are not sustained capacity', fontsize=12)
    for extension in ('png', 'svg'):
        target = HERE / f'comparison-{directory.name}.{extension}'
        fig.savefig(target, dpi=160)
        if extension == 'svg':
            target.write_text('\n'.join(line.rstrip() for line in target.read_text().splitlines()) + '\n')
    plt.close(fig)
    rows = []
    for profile in LABELS:
        for payload in (64, 256, 1024):
            for history in (0, 32, 128):
                cells = [r for r in reports if r['profile'] == profile and r['payload_bytes'] == payload and r['history'] == history]
                row = dict(host=directory.name, profile=profile, payload_bytes=payload, history=history, runs=len(cells))
                for metric, _, divisor in METRICS:
                    values = [(statistics.median_low(sorted(r['metrics'][metric])) if metric in r['metrics'] else r['rates'][metric]) / divisor for r in cells]
                    row[metric[:-3] + "_us" if metric.endswith("_ns") else metric] = statistics.median(values)
                rows.append(row)
    return rows


def main():
    rows = []
    for name in ('macos-arm64', 'linux-x86_64'):
        if (HERE / name / 'reports.jsonl.gz').is_file():
            rows.extend(plot(HERE / name))
    assert rows, 'No retained measurements'
    with (HERE / 'comparison.csv').open('w', newline='') as stream:
        writer = csv.DictWriter(stream, fieldnames=list(rows[0]), lineterminator="\n")
        writer.writeheader()
        writer.writerows(rows)

if __name__ == '__main__':
    main()
