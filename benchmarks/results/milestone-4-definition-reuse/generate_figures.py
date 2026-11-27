#!/usr/bin/env python3
"""Plot operation-local definition reuse from retained reports; needs matplotlib."""
import importlib.util
from pathlib import Path
import statistics

ROOT = Path(__file__).resolve().parent
spec = importlib.util.spec_from_file_location(
    "definition_plot_helpers_420", ROOT.parent / "milestone-4-delivery-lookup/generate_figures.py")
previous = importlib.util.module_from_spec(spec)
spec.loader.exec_module(previous)
# Reuse the existing report filters, nearest-rank statistic, styling and
# deterministic SVG/PNG writer. Absolute data paths retain their own population.
helpers = previous.previous
helpers.ROOT = ROOT
plt = helpers.plt
VARIANTS = (("before", "Before · 0.4.21", "#a14f0a", -0.18),
            ("after", "After · 0.4.22", "#195c9c", 0.18))
OPERATIONS = ("issue", "send", "receive", "acknowledge")
LABELS = ("Issue ID", "Send", "Receive", "Acknowledge")
HISTORIES = (0, 32, 128)


def performance():
    figure, axes = plt.subplots(1, 2, figsize=(12, 5.2), constrained_layout=True)
    for variant, label, color, shift in VARIANTS:
        serial = helpers.serial_runs(ROOT / "alternating" / (variant + "-normal"), 128)
        assert len(serial) == 5 and all(
            run["outcome"] == "complete" and run["completed"] == 16 for run in serial)
        operation_values = [[helpers.previous.p50(run, operation) for run in serial]
                            for operation in OPERATIONS]
        rate_values = []
        for history in HISTORIES:
            runs = helpers.serial_runs(ROOT / "alternating" / (variant + "-normal"), history)
            assert len(runs) == 5 and all(
                run["outcome"] == "complete" and run["completed"] == 16 for run in runs)
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
    figure.suptitle("Definition reuse · Apple M4 · normal builds · five alternating pairs per H", fontsize=12)
    figure.supxlabel("Bars: medians. Whiskers: observed five-run ranges, not confidence intervals.\n"
                      "H: messages already sent and acknowledged. 16 new messages per run. Complete history validation remains.", fontsize=9)
    helpers.save(figure, "definition-performance")
    plt.close(figure)


def work():
    figure, axes = plt.subplots(1, 2, figsize=(12, 5.2), constrained_layout=True)
    for variant, label, color, shift in VARIANTS:
        runs = helpers.serial_runs(ROOT / ("macos-arm64-" + variant + "-enabled"), 128, "counts")
        assert len(runs) == 3 and all(
            run["outcome"] == "complete" and run["completed"] == 16 for run in runs)
        samples_by_operation = []
        for operation in OPERATIONS:
            samples = [next(sample for sample in run["samples"] if sample["operation"] == operation)["counters"]
                       for run in runs]
            for key in ("task_slots", "allocations"):
                assert all(sample[key] == samples[0][key] for sample in samples)
            samples_by_operation.append(samples[0])
        for axis, key in zip(axes, ("task_slots", "allocations")):
            values = [sample[key] for sample in samples_by_operation]
            bars = axis.bar([index + shift for index in range(len(OPERATIONS))], values,
                            width=0.34, label=label, color=color)
            for bar, value in zip(bars, values):
                axis.annotate(f"{value:,}", (bar.get_x() + bar.get_width() / 2, value),
                              xytext=(0, 4), textcoords="offset points", ha="center", fontsize=9)
    axes[0].set(title="Task catalog work", ylabel="Task allocation slots examined")
    axes[1].set(title="C++ allocation requests", ylabel="Successful cumulative new requests")
    for axis in axes:
        axis.set_xticks(range(len(OPERATIONS)), LABELS)
        helpers.style(axis)
        axis.legend(frameon=False, loc="upper left")
    figure.suptitle("Definition reuse · Apple M4 · first H=128 calls · counted builds", fontsize=12)
    figure.supxlabel("All three repetitions agree. Complete history validation remains.\n"
                      "H: messages already sent and acknowledged. Allocation requests are cumulative, not live or peak memory.", fontsize=9)
    helpers.save(figure, "definition-work")
    plt.close(figure)


if __name__ == "__main__":
    plt.rcParams.update({"font.size": 10, "svg.hashsalt": "angaria-4.20"})
    performance()
    work()
    print("Wrote definition-performance and definition-work, SVG and PNG, from retained measurements.")
