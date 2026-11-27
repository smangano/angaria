#!/usr/bin/env python3
"""Plot retained ordinary-call latency and counted native work; needs matplotlib."""
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


def serial_runs(runs, collection):
    return [run for run in runs if run["workload"] == "serial" and run["history"] == 128
            and run["history_scope"] == "same" and run["payload_bytes"] == 8
            and run["fields"] == 1 and run["capacity_scale"] == 1
            and run["collection"] == collection]


def p50(run, operation):
    values = sorted(sample["wall_ns"] / 1e6 for sample in run["samples"]
                    if sample["operation"] == operation)
    return values[math.ceil(len(values) / 2) - 1]


def main():
    plt.rcParams.update({"font.size": 10, "svg.hashsalt": "angaria-4.16"})
    figure, axes = plt.subplots(1, 2, figsize=(11, 4.5), constrained_layout=True)
    for variant, label, color, shift, counted in (
            ("before", "Before · 0.4.17", "#a14f0a", -0.18,
             "../milestone-4-gate-handoff/macos-arm64-after-enabled"),
            ("after", "After · 0.4.18", "#195c9c", 0.18, "macos-arm64-after-enabled")):
        serial = serial_runs(read_runs("alternating/" + variant + "-normal"), "none")
        assert len(serial) == 5 and all(run["outcome"] == "complete" for run in serial)
        values = [statistics.median(p50(run, operation) for run in serial) for operation in OPERATIONS]
        axes[0].bar([i + shift for i in range(4)], values, width=0.34, label=label, color=color)
        runs = serial_runs(read_runs(counted), "counts")
        assert len(runs) == 3
        counters = [next(s for s in run["samples"] if s["operation"] == "send")["counters"] for run in runs]
        keys = ("native_map_calls", "native_read_calls", "file_stat_calls")
        assert all(all(sample[key] == counters[0][key] for key in keys) for sample in counters)
        values = [counters[0][key] for key in keys]
        bars = axes[1].bar([i + shift for i in range(3)], values, width=0.34, label=label, color=color)
        for bar, value in zip(bars, values):
            axes[1].text(bar.get_x() + bar.get_width() / 2, bar.get_height() + 12,
                         str(value), ha="center", fontsize=9)
    axes[0].set(title="Ordinary cost · five alternating pairs", ylabel="Median of per-run p50 wall time (ms)",
                xticks=range(4), xticklabels=("Issue", "Send", "Receive", "Ack"))
    axes[1].set(title="First send · counted work · three repeats", ylabel="Native call attempts in one public call",
                xticks=range(3), xticklabels=("Map", "Read", "Stat"))
    for axis in axes:
        axis.set_ylim(0, axis.get_ylim()[1] * 1.25)
        axis.legend(frameon=False, loc="upper left")
        axis.grid(axis="y", alpha=0.18)
        axis.set_axisbelow(True)
        axis.spines[["top", "right"]].set_visible(False)
    figure.suptitle("Runtime metadata windows · Apple M4 · H=128 · 16-message windows", fontsize=12)
    target = ROOT / "runtime-windows.svg"
    figure.savefig(target, metadata={"Date": None})
    target.write_text("\n".join(line.rstrip() for line in target.read_text().splitlines()) + "\n")
    figure.savefig("/private/tmp/angaria-416-runtime-windows.png", dpi=160)
    print("Wrote runtime-windows.svg from ordinary latency and separate counted controls.")


if __name__ == "__main__":
    main()
