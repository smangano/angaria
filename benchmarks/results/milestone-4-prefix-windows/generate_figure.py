#!/usr/bin/env python3
"""Plot retained normal-build runs; requires matplotlib, not used by the benchmark."""

import gzip
import json
import math
from pathlib import Path
import statistics

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

ROOT = Path(__file__).resolve().parent
HISTORIES = (0, 32, 128)


def read_runs(name):
    with gzip.open(ROOT / name / "reports.jsonl.gz", "rt", encoding="utf-8") as source:
        return [json.loads(line) for line in source]


def matching(runs, history):
    return [run for run in runs if run["workload"] == "serial"
            and run["history"] == history and run["history_scope"] == "same"
            and run["payload_bytes"] == 8 and run["fields"] == 1
            and run["capacity_scale"] == 1 and run["outcome"] == "complete"]


def send_p50(run):
    values = sorted(sample["wall_ns"] / 1e6 for sample in run["samples"]
                    if sample["operation"] == "send")
    return values[math.ceil(len(values) / 2) - 1]


def throughput(run):
    return run["completed"] * 1e9 / run["cycle_phase_ns"]


def main():
    plt.rcParams.update({"font.size": 10, "svg.hashsalt": "angaria-4.14"})
    figure, axes = plt.subplots(1, 2, figsize=(11, 4.5), constrained_layout=True)
    for name, label, color in (("macos-arm64-before-normal", "Before · 0.4.15", "#a14f0a"),
                               ("macos-arm64-after-normal", "After · 0.4.16", "#195c9c")):
        runs = read_runs(name)
        for axis, metric in zip(axes, (send_p50, throughput)):
            groups = [[metric(run) for run in matching(runs, history)] for history in HISTORIES]
            assert all(len(group) == 3 for group in groups)
            centers = [statistics.median(group) for group in groups]
            ranges = [[center - min(group) for group, center in zip(groups, centers)],
                      [max(group) - center for group, center in zip(groups, centers)]]
            axis.errorbar(HISTORIES, centers, yerr=ranges, marker="o", capsize=3,
                          label=label, color=color)
    axes[0].set(title="Public send latency", ylabel="Per-run p50 wall time (ms)")
    axes[1].set(title="Complete issue/send/receive/ack cycles", ylabel="Messages per second")
    for axis in axes:
        axis.set_xlabel("Acknowledged messages preloaded in the Session")
        axis.set_xticks(HISTORIES)
        axis.set_ylim(bottom=0)
        axis.grid(alpha=0.18)
        axis.spines[["top", "right"]].set_visible(False)
        axis.legend(frameon=False)
    figure.suptitle("Bounded metadata scan windows · Apple M4 · same Queue history · 16-message window", fontsize=12)
    target = ROOT / "prefix-windows.svg"
    figure.savefig(target, metadata={"Date": None})
    target.write_text("\n".join(line.rstrip() for line in target.read_text().splitlines()) + "\n")
    figure.savefig("/private/tmp/angaria-414-prefix-windows.png", dpi=160)
    print("Wrote prefix-windows.svg; ranges show three run values, not confidence intervals.")


if __name__ == "__main__":
    main()
