#!/usr/bin/env python3
"""Plot exact-link receive measurements from retained reports; needs matplotlib."""
import importlib.util
from pathlib import Path
import statistics

ROOT = Path(__file__).resolve().parent
spec = importlib.util.spec_from_file_location(
    "receive_plot_helpers_419", ROOT.parent / "milestone-4-receive-validation/generate_figures.py")
previous = importlib.util.module_from_spec(spec)
spec.loader.exec_module(previous)
# Reuse the previous plot's filtering, nearest-rank statistic, styling and
# deterministic output helpers, with this result directory as the destination.
previous.ROOT = ROOT
plt = previous.plt
VARIANTS = (("before", "Before · 0.4.20", "#a14f0a", -0.18),
            ("after", "After · 0.4.21", "#195c9c", 0.18))
HISTORIES = (0, 32, 128)


def performance():
    figure, axes = plt.subplots(1, 2, figsize=(11, 4.8), constrained_layout=True)
    for variant, label, color, shift in VARIANTS:
        for axis_index, axis in enumerate(axes):
            medians, lows, highs = [], [], []
            for history in HISTORIES:
                runs = previous.serial_runs(ROOT / "alternating" / (variant + "-normal"), history)
                assert len(runs) == 5 and all(
                    run["outcome"] == "complete" and run["completed"] == 16 for run in runs)
                values = ([previous.previous.p50(run, "receive") for run in runs]
                          if axis_index == 0 else
                          [run["completed"] * 1e9 / run["cycle_phase_ns"] for run in runs])
                median = statistics.median(values)
                medians.append(median)
                lows.append(median - min(values))
                highs.append(max(values) - median)
            bars = axis.bar([i + shift for i in range(len(HISTORIES))], medians,
                            width=0.34, label=label, color=color,
                            yerr=[lows, highs], capsize=3)
            for bar, value, high in zip(bars, medians, highs):
                axis.annotate(f"{value:.3f}" if axis_index == 0 else f"{value:.0f}",
                              (bar.get_x() + bar.get_width() / 2, value + high),
                              xytext=(0, 4), textcoords="offset points", ha="center", fontsize=9)
    axes[0].set(title="Receive call · median of per-run p50", ylabel="Wall time (ms)")
    axes[1].set(title="Complete issue/send/receive/ack cycle", ylabel="Complete cycles per second")
    for axis in axes:
        axis.set_xticks(range(len(HISTORIES)), [f"H={history}" for history in HISTORIES])
        axis.set_xlabel("Preloaded message history")
        previous.style(axis)
        axis.legend(frameon=False, loc="upper left")
    figure.suptitle("Exact-link receive lookup · Apple M4 · normal builds · five alternating pairs per H", fontsize=12)
    figure.supxlabel("Bars: medians. Whiskers: observed five-run ranges, not confidence intervals. 16 messages per run.", fontsize=9)
    previous.save(figure, "receive-performance")
    plt.close(figure)


def work():
    figure, axes = plt.subplots(1, 2, figsize=(11, 4.8), constrained_layout=True)
    for variant, label, color, shift in VARIANTS:
        samples_by_history = []
        for history in HISTORIES:
            runs = previous.serial_runs(ROOT / ("macos-arm64-" + variant + "-enabled"), history, "counts")
            assert len(runs) == 3 and all(
                run["outcome"] == "complete" and run["completed"] == 16 for run in runs)
            samples = [next(sample for sample in run["samples"] if sample["operation"] == "receive")["counters"]
                       for run in runs]
            for key in ("allocations", "allocated_bytes"):
                assert all(sample[key] == samples[0][key] for sample in samples)
            samples_by_history.append(samples[0])
        for axis, key in zip(axes, ("allocations", "allocated_bytes")):
            values = [sample[key] for sample in samples_by_history]
            bars = axis.bar([i + shift for i in range(len(HISTORIES))], values,
                            width=0.34, label=label, color=color)
            for bar, value in zip(bars, values):
                axis.annotate(f"{value:,}", (bar.get_x() + bar.get_width() / 2, value),
                              xytext=(0, 4), textcoords="offset points", ha="center", fontsize=9)
    axes[0].set(title="C++ allocation requests", ylabel="Successful cumulative new requests")
    axes[1].set(title="C++ requested bytes", ylabel="Cumulative bytes requested by new")
    axes[1].ticklabel_format(axis="y", style="plain")
    for axis in axes:
        axis.set_xticks(range(len(HISTORIES)), [f"H={history}" for history in HISTORIES])
        axis.set_xlabel("Preloaded message history")
        previous.style(axis)
        axis.legend(frameon=False, loc="upper left")
    figure.suptitle("Apple M4 · first receive · counted builds · all three repetitions agree at each H", fontsize=12)
    figure.supxlabel("Cumulative allocation requests and requested bytes are not live or peak memory. 16 messages per run.", fontsize=9)
    previous.save(figure, "receive-work")
    plt.close(figure)


if __name__ == "__main__":
    plt.rcParams.update({"font.size": 10, "svg.hashsalt": "angaria-4.19"})
    performance()
    work()
    print("Wrote receive-performance and receive-work, SVG and PNG, from retained measurements.")
