#!/usr/bin/env python3
"""Plot scan-local framing buffers from retained reports; needs matplotlib."""
import importlib.util
from pathlib import Path
import statistics

ROOT = Path(__file__).resolve().parent
spec = importlib.util.spec_from_file_location(
    "prefix_buffer_plot_helpers_422", ROOT.parent / "milestone-4-payload-preparation/generate_figures.py")
previous = importlib.util.module_from_spec(spec)
spec.loader.exec_module(previous)
# Reuse report filtering, nearest-rank p50, styling and deterministic output.
# Explicit absolute paths keep each plot tied to its own retained population.
helpers = previous.helpers
helpers.ROOT = ROOT
plt = helpers.plt
VARIANTS = (("before", "Before · 0.4.23", "#a14f0a", -0.18),
            ("after", "After · 0.4.24", "#195c9c", 0.18))
OPERATIONS = previous.OPERATIONS
LABELS = previous.LABELS
HISTORIES = previous.HISTORIES


def serial_runs(variant, history, counted=False, population="alternating", repetitions=5):
    path = (ROOT / ("macos-arm64-" + variant + "-enabled") if counted else
            ROOT / population / (variant + "-normal"))
    runs = helpers.serial_runs(path, history, "counts" if counted else "none")
    version = "0.4.23" if variant == "before" else "0.4.24"
    assert len(runs) == (3 if counted else repetitions)
    assert all(run["framework_version"] == version and
               run["instrumentation_enabled"] is counted and
               run["outcome"] == "complete" and run["messages"] == 16 and
               run["accepted"] == 16 and run["completed"] == 16 and
               run["pending"] == 0 and run["unacknowledged_at_stop"] == 0
               for run in runs)
    assert all(sample["valid"] and not sample["overflow"]
               for run in runs for sample in run["samples"])
    return runs


def performance(follow_up=False):
    population = "confirmation" if follow_up else "alternating"
    repetitions = 20 if follow_up else 5
    run_count = "20" if follow_up else "five"
    figure, axes = plt.subplots(1, 2, figsize=(12, 5.2), constrained_layout=True)
    for variant, label, color, shift in VARIANTS:
        serial = serial_runs(variant, 128, population=population, repetitions=repetitions)
        operation_values = [[helpers.previous.p50(run, operation) for run in serial]
                            for operation in OPERATIONS]
        rate_values = []
        for history in HISTORIES:
            runs = serial_runs(variant, history, population=population, repetitions=repetitions)
            rate_values.append([run["completed"] * 1e9 / run["cycle_phase_ns"] for run in runs])
        for axis, groups, is_latency in zip(axes, (operation_values, rate_values), (True, False)):
            medians = [statistics.median(values) for values in groups]
            lows = [median - min(values) for median, values in zip(medians, groups)]
            highs = [max(values) - median for median, values in zip(medians, groups)]
            bars = axis.bar([index + shift for index in range(len(groups))], medians,
                            width=0.34, label=label, color=color,
                            yerr=[lows, highs], capsize=3)
            for bar, value, high in zip(bars, medians, highs):
                axis.annotate(f"{value:.3f}" if is_latency else f"{value:.0f}",
                              (bar.get_x() + bar.get_width() / 2, value + high),
                              xytext=(0, 4), textcoords="offset points", ha="center", fontsize=9)
    axes[0].set(title="H=128 · median of per-run p50", ylabel="Wall time (ms)")
    axes[0].set_xticks(range(len(OPERATIONS)), LABELS)
    axes[1].set(title="Complete issue/send/receive/ack cycle", ylabel="Complete cycles per second")
    axes[1].set_xticks(range(len(HISTORIES)), [f"H={history}" for history in HISTORIES])
    axes[1].set_xlabel("Preloaded message history")
    for axis in axes:
        helpers.style(axis)
        axis.legend(frameon=False, loc="upper left")
    title = f"Prefix read buffers · Apple M4 · normal builds · {run_count} alternating pairs per H"
    figure.suptitle(title + (" · follow-up" if follow_up else ""), fontsize=12)
    figure.supxlabel(f"Bars: medians. Whiskers: observed {run_count}-run ranges, not confidence intervals.\n"
                      "H: messages already sent and acknowledged. 16 new messages per run. Complete prefix validation remains.", fontsize=9)
    name = "prefix-buffers-confirmation-performance" if follow_up else "prefix-buffers-performance"
    helpers.save(figure, name)
    plt.close(figure)


def work():
    figure, axes = plt.subplots(1, 2, figsize=(12, 5.2), constrained_layout=True)
    samples_by_variant = {}
    for variant, label, color, shift in VARIANTS:
        runs = serial_runs(variant, 128, counted=True)
        samples_by_operation = []
        for operation in OPERATIONS:
            samples = [next(sample for sample in run["samples"] if sample["operation"] == operation)["counters"]
                       for run in runs]
            for key in ("allocations", "allocated_bytes", "prefix_records", "prefix_scans"):
                assert all(sample[key] == samples[0][key] for sample in samples)
            samples_by_operation.append(samples[0])
        samples_by_variant[variant] = samples_by_operation
        for axis, key in zip(axes, ("allocations", "allocated_bytes")):
            values = [sample[key] for sample in samples_by_operation]
            bars = axis.bar([index + shift for index in range(len(OPERATIONS))], values,
                            width=0.34, label=label, color=color)
            for bar, value in zip(bars, values):
                axis.annotate(f"{value:,}", (bar.get_x() + bar.get_width() / 2, value),
                              xytext=(0, 4), textcoords="offset points", ha="center", fontsize=9)
    for before, after in zip(samples_by_variant["before"], samples_by_variant["after"]):
        assert before["prefix_records"] == after["prefix_records"]
        assert before["prefix_scans"] == after["prefix_scans"] == 1
    record_counts = "/".join(str(sample["prefix_records"]) for sample in samples_by_variant["before"])
    axes[0].set(title="C++ allocation requests", ylabel="Successful cumulative new requests")
    axes[1].set(title="C++ requested bytes", ylabel="Cumulative bytes requested by new")
    axes[1].ticklabel_format(axis="y", style="plain")
    for axis in axes:
        axis.set_xticks(range(len(OPERATIONS)), LABELS)
        helpers.style(axis)
        axis.legend(frameon=False, loc="upper left")
    figure.suptitle("Prefix read buffers · Apple M4 · first H=128 calls · counted builds", fontsize=12)
    figure.supxlabel(f"All three repetitions agree. One full prefix scan remains: {record_counts} records, in operation order.\n"
                      "H: messages already sent and acknowledged. 16 new messages per run. Requested bytes are cumulative, not live or peak memory.", fontsize=9)
    helpers.save(figure, "prefix-buffers-work")
    plt.close(figure)


if __name__ == "__main__":
    plt.rcParams.update({"font.size": 10, "svg.hashsalt": "angaria-4.22"})
    performance()
    work()
    performance(follow_up=True)
    print("Wrote primary performance/work and separate confirmation performance, SVG and PNG, from retained measurements.")
