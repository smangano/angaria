#!/usr/bin/env python3
"""Plot native costs, counted allocations and matched Cachegrind work; needs matplotlib."""
import argparse
import importlib.util
from pathlib import Path
import sys

sys.dont_write_bytecode = True
ROOT = Path(__file__).resolve().parent
spec = importlib.util.spec_from_file_location(
    "manifest_validation_helpers_426", ROOT.parent / "milestone-4-single-destination/generate_figures.py")
previous = importlib.util.module_from_spec(spec)
spec.loader.exec_module(previous)
# Reuse the prior report validators, raw-profile/summary checks, range drawing
# and deterministic writer, rebinding only this population's paths/versions.
previous.ROOT = ROOT
previous.VERSIONS = {"before": "0.4.27", "after": "0.4.28"}
helpers = previous.helpers
helpers.ROOT = ROOT
plt = helpers.plt
OPERATIONS = previous.OPERATIONS
LABELS = previous.LABELS
HISTORIES = previous.HISTORIES
VARIANTS = (("before", "Before · 0.4.27", "#a14f0a", -0.18),
            ("after", "After · 0.4.28", "#195c9c", 0.18))
ALLOCATION_KEYS = {"allocations", "allocated_bytes"}


def validate_native_work():
    matched = 0
    for history in HISTORIES:
        before = previous.serial_runs("before", history, counted=True)
        after = previous.serial_runs("after", history, counted=True)
        for first, second in zip(before, after):
            assert len(first["samples"]) == len(second["samples"]) == 67
            for left, right in zip(first["samples"], second["samples"]):
                assert all(left[key] == right[key] for key in ("operation", "result", "actor", "index"))
                assert left["counters"].keys() == right["counters"].keys()
                first_counts = {key: value for key, value in left["counters"].items() if key not in ALLOCATION_KEYS}
                second_counts = {key: value for key, value in right["counters"].items() if key not in ALLOCATION_KEYS}
                assert first_counts == second_counts, (history, first["repetition"], left["operation"])
                matched += 1
    assert matched == 603
    return matched


def finish_axes(axes, ticks, labels):
    for axis in axes:
        axis.set_xticks(ticks, labels)
        helpers.style(axis)
        axis.legend(frameon=False, loc="upper left")


def performance():
    figure, axes = plt.subplots(1, 2, figsize=(12, 5.5), constrained_layout=True)
    for variant, label, color, shift in VARIANTS:
        # H128 was selected before measurement because retained manifests are
        # revisited during each complete-history scan.
        serial = previous.serial_runs(variant, 128)
        operations = [[previous.p50(run, operation) for run in serial] for operation in OPERATIONS]
        rates = [[run["completed"] * 1e9 / run["cycle_phase_ns"]
                  for run in previous.serial_runs(variant, history)] for history in HISTORIES]
        previous.plot_groups(axes[0], operations, label, color, shift, 3)
        previous.plot_groups(axes[1], rates, label, color, shift, 0)
    axes[0].set(title="H=128 · median of per-run p50", ylabel="Wall time (ms)")
    axes[0].set_xticks(range(len(OPERATIONS)), LABELS)
    axes[1].set(title="Complete issue/send/receive/ack cycle", ylabel="Complete cycles per second")
    axes[1].set_xticks(range(len(HISTORIES)), [f"H={history}" for history in HISTORIES])
    axes[1].set_xlabel("Preloaded message history")
    for axis in axes:
        helpers.style(axis)
        axis.legend(frameon=False, loc="upper left")
    figure.suptitle("Manifest validation · Apple M4 · normal builds · 20 alternating pairs per H", fontsize=12)
    figure.supxlabel("Bars: medians. Whiskers: full observed 20-run ranges, not confidence intervals.\n"
                      "H=128 selected before measurement: retained manifests grow with validated history.\n"
                      "H: messages already sent and acknowledged. 16 new messages per run. Complete prefix validation remains.", fontsize=9)
    helpers.save(figure, "manifest-validation-performance")
    plt.close(figure)


def allocations(matched):
    figure, axes = plt.subplots(1, 2, figsize=(12, 5.8), constrained_layout=True)
    figure.get_layout_engine().set(h_pad=0.12)
    for variant, label, color, shift in VARIANTS:
        runs = previous.serial_runs(variant, 128, counted=True)
        counters = [[next(sample for sample in run["samples"] if sample["operation"] == operation)["counters"]
                     for run in runs] for operation in OPERATIONS]
        for axis, key, divisor, decimals in ((axes[0], "allocations", 1, 0),
                                              (axes[1], "allocated_bytes", 1024, 1)):
            groups = [[sample[key] / divisor for sample in samples] for samples in counters]
            previous.plot_groups(axis, groups, label, color, shift, decimals)
    axes[0].set(title="First H=128 call · C++ allocation requests", ylabel="Successful cumulative new requests")
    axes[1].set(title="First H=128 call · C++ requested bytes", ylabel="Cumulative requested KiB (1 KiB = 1,024 bytes)")
    finish_axes(axes, range(len(OPERATIONS)), LABELS)
    figure.suptitle("Manifest validation · Apple M4 · counted builds · three repetitions", fontsize=12)
    figure.supxlabel("Bars: three-run medians. Whiskers: full observed ranges, not confidence intervals.\n"
                      f"All nonallocation counters agree across {matched} matched samples. Allocations/bytes are cumulative, not live or peak memory.\n"
                      "H: messages already sent and acknowledged. 16 new messages per run. Complete prefix validation remains.", fontsize=9)
    helpers.save(figure, "manifest-validation-allocations")
    plt.close(figure)


def cachegrind():
    figure, axes = plt.subplots(1, 2, figsize=(12, 5.5), constrained_layout=True)
    for variant, label, color, shift in VARIANTS:
        runs = previous.cachegrind_runs(variant)
        for axis, event in zip(axes, ("Ir", "Dw")):
            groups = [[run[event] / run["completed"] / 1e6 for run in runs if run["history"] == history]
                      for history in HISTORIES]
            previous.plot_groups(axis, groups, label, color, shift, 2)
    axes[0].set(title="Cachegrind instructions per completed cycle", ylabel="Ir / cycle (millions)")
    axes[1].set(title="Modeled data writes per completed cycle", ylabel="Dw / cycle (millions)")
    finish_axes(axes, range(len(HISTORIES)), [f"H={history}" for history in HISTORIES])
    for axis in axes:
        axis.set_xlabel("Previously sent and acknowledged messages")
    figure.suptitle("Manifest validation · Linux x86_64 · matched Cachegrind profiles · diagnostics OFF", fontsize=12)
    figure.supxlabel("Bars: three-profile medians. Whiskers: full observed ranges, not confidence intervals. 16 cycles per profile.\n"
                      "Cold simulated region; fixed model: 32 KiB I1/D1, 8 MiB LL, 64-byte lines.\n"
                      "Counts are not native latency or retired hardware instructions; x86 REP iterations are counted separately.", fontsize=9)
    helpers.save(figure, "manifest-validation-cachegrind")
    plt.close(figure)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--plot", choices=("native", "cachegrind", "all"), default="all")
    options = parser.parse_args()
    plt.rcParams.update({"font.size": 10, "svg.hashsalt": "angaria-4.26"})
    if options.plot in ("native", "all"):
        matched = validate_native_work()
        performance()
        allocations(matched)
    if options.plot in ("cachegrind", "all"):
        cachegrind()
    print(f"Wrote {options.plot} manifest-validation figures from retained measurements.")
