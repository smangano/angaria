#!/usr/bin/env python3
"""Recreate the figure from retained raw runs; requires matplotlib."""

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


def reports(name):
    with gzip.open(ROOT / name / "reports.jsonl.gz", "rt", encoding="utf-8") as source:
        return [json.loads(line) for line in source]


def matching(data, history):
    return [run for run in data if run["workload"] == "serial"
            and run["history"] == history and run["history_scope"] == "same"
            and run["payload_bytes"] == 8 and run["fields"] == 1
            and run["capacity_scale"] == 1 and run["outcome"] == "complete"]


def p50(run, operation):
    values = sorted(sample["wall_ns"] / 1e6 for sample in run["samples"]
                    if sample["operation"] == operation)
    return values[math.ceil(len(values) * 0.5) - 1]


def main():
    normal = reports("macos-arm64-disabled")
    counted = [run for run in reports("macos-arm64-enabled-bounded")
               if run["collection"] == "counts"]
    plt.rcParams.update({"font.size": 10, "svg.hashsalt": "angaria-4.12"})
    figure, axes = plt.subplots(1, 2, figsize=(11, 4.5), constrained_layout=True)
    colors = ("#195c9c", "#a14f0a", "#247456", "#81549d")
    for operation, color in zip(("issue", "send", "receive", "acknowledge"), colors):
        groups = [[p50(run, operation) for run in matching(normal, h)] for h in HISTORIES]
        assert all(len(group) == 3 for group in groups)
        centers = [statistics.median(group) for group in groups]
        errors = [[center - min(group) for group, center in zip(groups, centers)],
                  [max(group) - center for group, center in zip(groups, centers)]]
        axes[0].errorbar(HISTORIES, centers, yerr=errors, marker="o", capsize=3,
                         label=operation, color=color)
    axes[0].set(title="Public calls in the ordinary build", ylabel="Per-run p50 wall time (ms)")
    axes[0].legend(loc="upper left", frameon=False)
    for counter, label, color in zip(("file_stat_calls", "native_read_calls", "native_map_calls"),
                                      ("stat attempts", "read attempts", "map attempts"), colors):
        values = []
        for history in HISTORIES:
            calls = [next(sample for sample in run["samples"] if sample["operation"] == "send")
                     ["counters"][counter] for run in matching(counted, history)]
            assert len(calls) == 3 and len(set(calls)) == 1
            values.append(calls[0])
        axes[1].plot(HISTORIES, values, marker="o", label=label, color=color)
    axes[1].set(title="First send: native work in the counted build", ylabel="Attempts in one call")
    axes[1].legend(loc="upper left", frameon=False)
    for axis in axes:
        axis.set_xlabel("Acknowledged messages preloaded in the Session")
        axis.set_xticks(HISTORIES)
        axis.set_ylim(bottom=0)
        axis.grid(alpha=0.18)
        axis.spines[["top", "right"]].set_visible(False)
    figure.suptitle("Angaria 0.4.14 · Apple M4 · same Queue history · 16-message window", fontsize=12)
    svg_path = ROOT / "history-cost.svg"
    figure.savefig(svg_path, metadata={"Date": None})
    # Matplotlib pads SVG path lines; keep repository whitespace checks useful.
    svg_path.write_text("\n".join(line.rstrip() for line in svg_path.read_text().splitlines()) + "\n")
    figure.savefig("/private/tmp/angaria-412-history-cost.png", dpi=160)
    print("Wrote history-cost.svg; ranges show min/max of three run p50s, not confidence intervals.")


if __name__ == "__main__":
    main()
