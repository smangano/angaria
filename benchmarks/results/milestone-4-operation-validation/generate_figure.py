#!/usr/bin/env python3
"""Plot retained latency and prefix work; reuse preceding report's readers."""
import importlib.util
from pathlib import Path
import statistics

ROOT = Path(__file__).resolve().parent
spec = importlib.util.spec_from_file_location("previous_plot", ROOT.parent / "milestone-4-runtime-windows/generate_figure.py")
previous = importlib.util.module_from_spec(spec)
spec.loader.exec_module(previous)
plt = previous.plt
OPERATIONS = previous.OPERATIONS


def main():
    plt.rcParams.update({"font.size": 10, "svg.hashsalt": "angaria-4.17"})
    figure, axes = plt.subplots(1, 2, figsize=(11, 4.5), constrained_layout=True)
    for variant, label, color, shift, counted in (
            ("before", "Before · 0.4.18", "#a14f0a", -0.18,
             ROOT.parent / "milestone-4-runtime-windows/macos-arm64-after-enabled"),
            ("after", "After · 0.4.19", "#195c9c", 0.18, ROOT / "macos-arm64-after-enabled")):
        serial = previous.serial_runs(previous.read_runs(ROOT / "alternating" / (variant + "-normal")), "none")
        assert len(serial) == 5 and all(run["outcome"] == "complete" for run in serial)
        values = [statistics.median(previous.p50(run, operation) for run in serial) for operation in OPERATIONS]
        axes[0].bar([i + shift for i in range(4)], values, width=0.34, label=label, color=color)
        runs = previous.serial_runs(previous.read_runs(counted), "counts")
        assert len(runs) == 3
        values = []
        for operation in OPERATIONS:
            counts = [next(s for s in run["samples"] if s["operation"] == operation)["counters"]["prefix_records"]
                      for run in runs]
            assert len(set(counts)) == 1
            values.append(counts[0])
        bars = axes[1].bar([i + shift for i in range(4)], values, width=0.34, label=label, color=color)
        for bar, value in zip(bars, values):
            axes[1].text(bar.get_x() + bar.get_width() / 2, bar.get_height() + 7,
                         str(value), ha="center", fontsize=9)
    axes[0].set(title="Ordinary cost · five alternating pairs", ylabel="Median of per-run p50 wall time (ms)")
    axes[1].set(title="First call · counted work · three repeats", ylabel="Record visits in full-prefix scans")
    for axis in axes:
        axis.set_xticks(range(4), ("Issue", "Send", "Receive", "Ack"))
        axis.set_ylim(0, axis.get_ylim()[1] * 1.25)
        axis.legend(frameon=False, loc="upper left")
        axis.grid(axis="y", alpha=0.18)
        axis.set_axisbelow(True)
        axis.spines[["top", "right"]].set_visible(False)
    figure.suptitle("Operation validation reuse · Apple M4 · H=128 · 16-message windows", fontsize=12)
    target = ROOT / "operation-validation.svg"
    figure.savefig(target, metadata={"Date": None})
    target.write_text("\n".join(line.rstrip() for line in target.read_text().splitlines()) + "\n")
    figure.savefig("/private/tmp/angaria-417-operation-validation.png", dpi=160)
    print("Wrote operation-validation.svg from normal latency and separate counted controls.")


if __name__ == "__main__":
    main()
