#!/usr/bin/env python3
"""Plot post-lock resource validation from retained reports; needs matplotlib."""
import importlib.util
from pathlib import Path
import statistics

ROOT = Path(__file__).resolve().parent
spec = importlib.util.spec_from_file_location(
    "gate_resource_plot_helpers_423", ROOT.parent / "milestone-4-prefix-buffers/generate_figures.py")
previous = importlib.util.module_from_spec(spec)
spec.loader.exec_module(previous)
# Reuse the existing report filter, nearest-rank p50, styling and deterministic
# output writer. Version and repetition checks belong to this new population.
helpers = previous.helpers
helpers.ROOT = ROOT
plt = helpers.plt
p50 = helpers.previous.p50
OPERATIONS = previous.OPERATIONS
LABELS = previous.LABELS
HISTORIES = previous.HISTORIES
VERSIONS = {"before": "0.4.24", "after": "0.4.25"}
VARIANTS = (("before", "Before · " + VERSIONS["before"], "#a14f0a", -0.18),
            ("after", "After · " + VERSIONS["after"], "#195c9c", 0.18))


def serial_runs(variant, history, counted=False):
    path = (ROOT / ("macos-arm64-" + variant + "-enabled") if counted else
            ROOT / "alternating" / (variant + "-normal"))
    runs = helpers.serial_runs(path, history, "counts" if counted else "none")
    repetitions = 3 if counted else 20
    assert len(runs) == repetitions
    assert {run["repetition"] for run in runs} == set(range(1, repetitions + 1))
    assert all(run["framework_version"] == VERSIONS[variant] and
               run["instrumentation_enabled"] is counted and
               run["outcome"] == "complete" and run["messages"] == 16 and
               run["accepted"] == 16 and run["completed"] == 16 and
               run["pending"] == 0 and run["unacknowledged_at_stop"] == 0
               for run in runs)
    assert all(sample["valid"] and not sample["overflow"]
               for run in runs for sample in run["samples"])
    return runs


def performance():
    figure, axes = plt.subplots(1, 2, figsize=(12, 5.5), constrained_layout=True)
    for variant, label, color, shift in VARIANTS:
        # H=0 was selected before measurement: a fixed saving per acquisition
        # is easier to see without a long retained-history scan dominating it.
        serial = serial_runs(variant, 0)
        operation_values = [[p50(run, operation) for run in serial]
                            for operation in OPERATIONS]
        rate_values = []
        for history in HISTORIES:
            runs = serial_runs(variant, history)
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
    axes[0].set(title="H=0 · median of per-run p50", ylabel="Wall time (ms)")
    axes[0].set_xticks(range(len(OPERATIONS)), LABELS)
    axes[1].set(title="Complete issue/send/receive/ack cycle", ylabel="Complete cycles per second")
    axes[1].set_xticks(range(len(HISTORIES)), [f"H={history}" for history in HISTORIES])
    axes[1].set_xlabel("Preloaded message history")
    for axis in axes:
        helpers.style(axis)
        axis.legend(frameon=False, loc="upper left")
    figure.suptitle("Gate resource checks · Apple M4 · normal builds · 20 alternating pairs per H", fontsize=12)
    figure.supxlabel("Bars: medians. Whiskers: full observed 20-run ranges, not confidence intervals.\n"
                      "H=0 selected before measurement to expose fixed acquisition cost.\n"
                      "H: messages already sent and acknowledged. 16 new messages per run.", fontsize=9)
    helpers.save(figure, "gate-resources-performance")
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
            for key in ("file_stat_calls", "allocations", "prefix_records", "prefix_scans"):
                assert all(sample[key] == samples[0][key] for sample in samples)
            samples_by_operation.append(samples[0])
        samples_by_variant[variant] = samples_by_operation
        for axis, key in zip(axes, ("file_stat_calls", "allocations")):
            values = [sample[key] for sample in samples_by_operation]
            bars = axis.bar([index + shift for index in range(len(OPERATIONS))], values,
                            width=0.34, label=label, color=color)
            for bar, value in zip(bars, values):
                axis.annotate(f"{value:,}", (bar.get_x() + bar.get_width() / 2, value),
                              xytext=(0, 4), textcoords="offset points", ha="center", fontsize=9)
    for before, after in zip(samples_by_variant["before"], samples_by_variant["after"]):
        assert before["file_stat_calls"] - after["file_stat_calls"] == 3
        assert before["prefix_records"] == after["prefix_records"]
        assert before["prefix_scans"] == after["prefix_scans"] == 1
    record_counts = "/".join(str(sample["prefix_records"]) for sample in samples_by_variant["before"])
    axes[0].set(title="Native file-stat attempts", ylabel="fstat / fstatat calls")
    axes[1].set(title="C++ allocation requests", ylabel="Successful cumulative new requests")
    for axis in axes:
        axis.set_xticks(range(len(OPERATIONS)), LABELS)
        helpers.style(axis)
        axis.legend(frameon=False, loc="upper left")
    figure.suptitle("Gate resource checks · Apple M4 · first H=128 calls · counted builds", fontsize=12)
    figure.supxlabel(f"All three repetitions agree. Three fewer stats per call; one prefix scan remains: {record_counts} records.\n"
                      "H: messages already sent and acknowledged. 16 new messages per run. Counts are cumulative, not live or peak memory.", fontsize=9)
    helpers.save(figure, "gate-resources-work")
    plt.close(figure)


if __name__ == "__main__":
    plt.rcParams.update({"font.size": 10, "svg.hashsalt": "angaria-4.23"})
    performance()
    work()
    print("Wrote gate-resources-performance and gate-resources-work, SVG and PNG, from retained measurements.")
