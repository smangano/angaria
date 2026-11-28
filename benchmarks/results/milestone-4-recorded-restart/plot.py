#!/usr/bin/env python3
"""Plot all retained local restart block means; these are not IPC capacity."""

import os
from pathlib import Path
import statistics
import sys
import tempfile

os.environ.setdefault("MPLCONFIGDIR", str(Path(tempfile.gettempdir()) / "angaria-restart-matplotlib"))
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

ROOT = Path(__file__).resolve().parent
sys.path.insert(0, str(ROOT.parents[1]))
from recorded_restart_study import render
from producer_log_append_study import load_json

render(ROOT / "native-arm64")
rows = [load_json(line) for file in sorted((ROOT / "native-arm64/raw").glob("*.stdout.txt"))
        for line in file.read_text().splitlines() if line.startswith('{"study":"recorded_queue_replacement_cost"')]
histories = (0, 128, 4096)
phases = (("baseline_id_send_lookup_receive_ns", "Before replacement: 64 messages", "#3973ad"),
          ("recovered_id_send_lookup_receive_ns", "First after catch-up: 64 messages", "#d27b28"),
          ("following_stream_id_send_lookup_receive_ns", "Following stream: 4,096 messages", "#258567"))
fig, axes = plt.subplots(2, 1, figsize=(10, 8))
for phase, (field, label, color) in enumerate(phases):
    for axis, conversion in zip(axes, (lambda ns: ns / 1000, lambda ns: 1000 / ns)):
        medians = []
        for x, history in enumerate(histories):
            values = [conversion(v) for row in rows if row["history_before_baseline"] == history for v in row[field]]
            middle = statistics.median(values)
            medians.append(middle)
            location = x + (phase - 1) * 0.13
            jitter = [(i - (len(values) - 1) / 2) * 0.004 for i in range(len(values))]
            axis.scatter([location + offset for offset in jitter], values, s=18, alpha=0.45, color=color)
            axis.errorbar(location, middle, yerr=[[middle - min(values)], [max(values) - middle]],
                          fmt="o", color=color, capsize=4, linewidth=1.2)
        axis.plot([x + (phase - 1) * 0.13 for x in range(len(histories))], medians,
                  color=color, linewidth=1.4, label=label)
for axis in axes:
    axis.set_xticks(range(len(histories)), ["0", "128", "4,096"])
    axis.set_xlabel("Initial retained messages before the baseline (H)")
    axis.set_yscale("log")
    axis.grid(True, axis="y", which="both", alpha=0.18)
    axis.spines[["top", "right"]].set_visible(False)
axes[0].set_ylabel("ID / send / lookup / receive / verify\nblock mean (µs per publication)")
axes[1].set_ylabel("Verified local block rate\n(million publications/s)")
handles, labels = axes[0].get_legend_handles_labels()
fig.legend(handles, labels, loc="upper center", bbox_to_anchor=(0.5, 0.96), fontsize=9, frameon=False)
fig.suptitle("Recorded Queue restart: native macOS ARM64, 8-byte payload, two credits", fontsize=13, y=0.99)
fig.text(0.5, 0.018, "Dots retain every block mean; whiskers show full ranges. Three invocations × five samples per history.\n"
         "Intervals have different message counts. Same-process serial costs; no IPC capacity or tail-latency claim.",
         ha="center", fontsize=8.5, color="#444444")
fig.subplots_adjust(left=0.14, right=0.97, top=0.82, bottom=0.12, hspace=0.36)
fig.savefig(ROOT / "restart-costs.png", dpi=160)
fig.savefig(ROOT / "restart-costs.svg", metadata={"Date": None})
