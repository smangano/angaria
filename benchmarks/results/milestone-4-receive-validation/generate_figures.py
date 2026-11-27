#!/usr/bin/env python3
"""Plot receive performance and work from retained samples; requires matplotlib."""
import importlib.util
from pathlib import Path
import statistics

ROOT = Path(__file__).resolve().parent
spec = importlib.util.spec_from_file_location("previous_plot", ROOT.parent / "milestone-4-runtime-windows/generate_figure.py")
previous = importlib.util.module_from_spec(spec)
spec.loader.exec_module(previous)
plt = previous.plt
VARIANTS = (("before", "Before · 0.4.19", "#a14f0a", -0.18),
            ("after", "After · 0.4.20", "#195c9c", 0.18))


def serial_runs(path, history, collection="none"):
    return [run for run in previous.read_runs(path) if run["workload"] == "serial"
            and run["history"] == history and run["history_scope"] == "same"
            and run["payload_bytes"] == 8 and run["fields"] == 1
            and run["capacity_scale"] == 1 and run["collection"] == collection]


def style(axis):
    axis.grid(axis="y", alpha=0.18)
    axis.set_axisbelow(True)
    axis.spines[["top", "right"]].set_visible(False)
    axis.set_ylim(0, axis.get_ylim()[1] * 1.22)


def save(figure, name):
    target = ROOT / (name + ".svg")
    figure.savefig(target, metadata={"Date": None})
    target.write_text("\n".join(line.rstrip() for line in target.read_text().splitlines()) + "\n")
    figure.savefig(ROOT / (name + ".png"), dpi=160)


def performance():
    figure, axes = plt.subplots(1, 2, figsize=(11, 4.8), constrained_layout=True)
    for variant, label, color, shift in VARIANTS:
        for axis_index in range(2):
            medians, lows, highs = [], [], []
            for history in (0, 32, 128):
                runs = serial_runs(ROOT / "alternating" / (variant + "-normal"), history)
                assert len(runs) == 5 and all(run["outcome"] == "complete" for run in runs)
                values = ([previous.p50(run, "receive") for run in runs] if axis_index == 0 else
                          [run["completed"] * 1e9 / run["cycle_phase_ns"] for run in runs])
                median = statistics.median(values)
                medians.append(median)
                lows.append(median - min(values))
                highs.append(max(values) - median)
            bars = axes[axis_index].bar([i + shift for i in range(3)], medians, width=0.34,
                                        label=label, color=color, yerr=[lows, highs], capsize=3)
            for bar, value, high in zip(bars, medians, highs):
                axes[axis_index].annotate(f"{value:.3f}" if axis_index == 0 else f"{value:.0f}",
                    (bar.get_x() + bar.get_width() / 2, value + high),
                    xytext=(0, 4), textcoords="offset points", ha="center", fontsize=9)
    axes[0].set(title="Receive call · median of per-run p50", ylabel="Wall time (ms)")
    axes[1].set(title="Complete issue/send/receive/ack cycle", ylabel="Complete cycles per second")
    for axis in axes:
        axis.set_xticks(range(3), ("H=0", "H=32", "H=128"))
        axis.set_xlabel("Preloaded message history")
        style(axis)
        axis.legend(frameon=False, loc="upper left")
    figure.suptitle("Receive validation reuse · Apple M4 · normal builds · five alternating pairs per H", fontsize=12)
    figure.supxlabel("Bars: medians. Whiskers: observed five-run ranges, not confidence intervals. 16 messages per run.", fontsize=9)
    save(figure, "receive-performance")


def work():
    figure, axes = plt.subplots(1, 3, figsize=(13, 4.4), constrained_layout=True)
    for variant, label, color, shift in VARIANTS:
        path = (ROOT.parent / "milestone-4-operation-validation/macos-arm64-after-enabled" if variant == "before"
                else ROOT / "macos-arm64-after-enabled")
        runs = serial_runs(path, 128, "counts")
        assert len(runs) == 3
        samples = [next(s for s in run["samples"] if s["operation"] == "receive")["counters"] for run in runs]
        for axis, keys in zip(axes, (("topology_builds", "delivery_validations"),
                                     ("validated_publications",), ("allocations",))):
            assert all(all(sample[key] == samples[0][key] for key in keys) for sample in samples)
            values = [samples[0][key] for key in keys]
            bars = axis.bar([i + shift for i in range(len(keys))], values, width=0.34, label=label, color=color)
            for bar, value in zip(bars, values):
                axis.annotate(f"{value:,}", (bar.get_x() + bar.get_width() / 2, value),
                              xytext=(0, 4), textcoords="offset points", ha="center", fontsize=9)
    axes[0].set(title="Complete validation passes", ylabel="Passes in one receive call")
    axes[0].set_xticks(range(2), ("Topology builds", "Delivery validations"))
    axes[1].set(title="Retained Publications checked", ylabel="Publication checks")
    axes[1].set_xticks([0], ["First receive"])
    axes[2].set(title="C++ allocation requests", ylabel="Cumulative new requests, not live memory")
    axes[2].set_xticks([0], ["First receive"])
    for axis in axes:
        style(axis)
    axes[0].legend(frameon=False, loc="upper left")
    figure.suptitle("Apple M4 · first H=128 receive · counted builds · all three repetitions agree", fontsize=12)
    save(figure, "receive-work")


if __name__ == "__main__":
    plt.rcParams.update({"font.size": 10, "svg.hashsalt": "angaria-4.18"})
    performance()
    work()
    print("Wrote receive-performance and receive-work, SVG and PNG, from retained measurements.")
