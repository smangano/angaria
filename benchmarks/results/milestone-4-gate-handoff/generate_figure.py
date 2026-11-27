#!/usr/bin/env python3
"""Plot retained progress and ordinary-call cost; requires matplotlib."""
import gzip
import json
import math
from pathlib import Path
import statistics

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

ROOT = Path(__file__).resolve().parent
OPERATIONS = ("issue", "send", "receive", "acknowledge")


def read_runs(name):
    with gzip.open(ROOT / name / "reports.jsonl.gz", "rt", encoding="utf-8") as source:
        return [json.loads(line) for line in source]


def serial_runs(runs):
    return [run for run in runs if run["workload"] == "serial" and run["history"] == 128
            and run["history_scope"] == "same" and run["payload_bytes"] == 8
            and run["fields"] == 1 and run["capacity_scale"] == 1]


def p50(run, operation):
    values = sorted(sample["wall_ns"] / 1e6 for sample in run["samples"]
                    if sample["operation"] == operation)
    return values[math.ceil(len(values) / 2) - 1]


def main():
    plt.rcParams.update({"font.size": 10, "svg.hashsalt": "angaria-4.15"})
    figure, axes = plt.subplots(1, 2, figsize=(11, 4.5), constrained_layout=True)
    for name, label, color, shift in (("macos-arm64-before-normal", "Before · 0.4.16", "#a14f0a", -0.18),
                                       ("macos-arm64-after-normal", "After · 0.4.17", "#195c9c", 0.18)):
        runs = read_runs(name)
        loaded = sorted((run for run in runs if run["workload"] == "stream"
                         and run["idle_pollers"] == 3), key=lambda run: run["repetition"])
        assert len(loaded) == 3 and all(run["messages"] == 16 for run in loaded)
        bars = axes[0].bar([i + shift for i in range(3)], [run["completed"] for run in loaded],
                           width=0.34, label=label, color=color)
        for bar, run in zip(bars, loaded):
            axes[0].text(bar.get_x() + bar.get_width() / 2, bar.get_height() + 0.25,
                         f"{run['completed']}/{run['accepted']}", ha="center", fontsize=9)
        variant = "before-normal" if "before" in name else "after-normal"
        serial = serial_runs(read_runs("alternating/" + variant))
        assert len(serial) == 5 and all(run["outcome"] == "complete" for run in serial)
        values = [statistics.median(p50(run, operation) for run in serial) for operation in OPERATIONS]
        axes[1].bar([i + shift for i in range(4)], values, width=0.34, label=label, color=color)
    axes[0].axhline(16, color="#6c757d", linestyle="--", linewidth=1)
    axes[0].set(title="Loaded stream · three idle pollers", ylabel="Messages acknowledged (16 requested)",
                xticks=range(3), xticklabels=("Run 1", "Run 2", "Run 3"), ylim=(0, 21))
    axes[0].set_yticks((0, 4, 8, 12, 16))
    axes[0].set_xlabel("Labels show acknowledged / accepted")
    axes[1].set(title="Serial cost · alternating pairs · H=128", ylabel="Median of per-run p50 wall time (ms)",
                xticks=range(4), xticklabels=("Issue", "Send", "Receive", "Ack"))
    axes[1].set_ylim(0, axes[1].get_ylim()[1] * 1.25)
    for axis in axes:
        axis.legend(frameon=False, loc="upper left")
        axis.grid(axis="y", alpha=0.18)
        axis.set_axisbelow(True)
        axis.spines[["top", "right"]].set_visible(False)
    figure.suptitle("Local gate handoff · Apple M4 · H=128 · normal builds · 16-message windows", fontsize=12)
    target = ROOT / "gate-handoff.svg"
    figure.savefig(target, metadata={"Date": None})
    target.write_text("\n".join(line.rstrip() for line in target.read_text().splitlines()) + "\n")
    figure.savefig("/private/tmp/angaria-415-gate-handoff.png", dpi=160)
    print("Wrote gate-handoff.svg; incomplete runs are progress observations, not throughput samples.")


if __name__ == "__main__":
    main()
