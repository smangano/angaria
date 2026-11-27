#!/usr/bin/env python3
"""Plot matched native cycles and separate Cachegrind work; needs matplotlib."""
import argparse
import importlib.util
import json
from pathlib import Path
import re
import statistics
import sys

sys.dont_write_bytecode = True
ROOT = Path(__file__).resolve().parent


def load_script(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


previous = load_script("single_destination_helpers_425",
                       ROOT.parent / "milestone-4-gate-resources/generate_figures.py")
cache_plots = load_script("single_destination_cache_plots_425",
                          ROOT.parent / "milestone-4-cachegrind/generate_figures.py")
# The runner imports its own benchmark driver. Leave that driver's ROOT intact;
# only the existing figure writer is rebound to this result directory.
sys.path.insert(0, str(ROOT.parents[1]))
cache_runner = load_script("single_destination_cache_runner_425", ROOT.parents[1] / "run_cachegrind.py")
helpers = previous.helpers
helpers.ROOT = ROOT
plt = helpers.plt
p50 = previous.p50
OPERATIONS = previous.OPERATIONS
LABELS = previous.LABELS
HISTORIES = previous.HISTORIES
VERSIONS = {"before": "0.4.26", "after": "0.4.27"}
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
               run["accepted"] == run["completed"] == 16 and
               run["pending"] == run["unacknowledged_at_stop"] == 0 for run in runs)
    assert all(sample["valid"] and not sample["overflow"]
               for run in runs for sample in run["samples"])
    return sorted(runs, key=lambda run: run["repetition"])


def validate_native_work():
    matched = 0
    for history in HISTORIES:
        before = serial_runs("before", history, counted=True)
        after = serial_runs("after", history, counted=True)
        for first, second in zip(before, after):
            assert len(first["samples"]) == len(second["samples"]) == 67
            for left, right in zip(first["samples"], second["samples"]):
                assert all(left[key] == right[key] for key in ("operation", "result", "actor", "index"))
                assert left["counters"] == right["counters"]
                matched += 1
        if history == 128:
            for operation, records in zip(OPERATIONS, (319, 319, 321, 321)):
                samples = [next(sample for sample in run["samples"] if sample["operation"] == operation)["counters"]
                           for run in before + after]
                assert all(sample == samples[0] for sample in samples)
                assert samples[0]["prefix_scans"] == 1 and samples[0]["prefix_records"] == records
    assert matched == 603


def plot_groups(axis, groups, label, color, shift, decimals):
    medians, lows, highs = cache_plots.ranges(groups)
    bars = axis.bar([index + shift for index in range(len(groups))], medians,
                    width=0.34, label=label, color=color, yerr=[lows, highs], capsize=3)
    for bar, value, high in zip(bars, medians, highs):
        axis.annotate(f"{value:.{decimals}f}", (bar.get_x() + bar.get_width() / 2, value + high),
                      xytext=(0, 4), textcoords="offset points", ha="center", fontsize=9)


def performance():
    validate_native_work()
    figure, axes = plt.subplots(1, 2, figsize=(12, 5.5), constrained_layout=True)
    for variant, label, color, shift in VARIANTS:
        # H128 was selected before measurement because repeated bitmap clearing
        # grows with the number of Publications revisited during validation.
        serial = serial_runs(variant, 128)
        operations = [[p50(run, operation) for run in serial] for operation in OPERATIONS]
        rates = [[run["completed"] * 1e9 / run["cycle_phase_ns"] for run in serial_runs(variant, history)]
                 for history in HISTORIES]
        plot_groups(axes[0], operations, label, color, shift, 3)
        plot_groups(axes[1], rates, label, color, shift, 0)
    axes[0].set(title="H=128 · median of per-run p50", ylabel="Wall time (ms)")
    axes[0].set_xticks(range(len(OPERATIONS)), LABELS)
    axes[1].set(title="Complete issue/send/receive/ack cycle", ylabel="Complete cycles per second")
    axes[1].set_xticks(range(len(HISTORIES)), [f"H={history}" for history in HISTORIES])
    axes[1].set_xlabel("Preloaded message history")
    for axis in axes:
        helpers.style(axis)
        axis.legend(frameon=False, loc="upper left")
    figure.suptitle("Single-destination validation · Apple M4 · normal builds · 20 alternating pairs per H", fontsize=12)
    figure.supxlabel("Bars: medians. Whiskers: full observed 20-run ranges, not confidence intervals.\n"
                      "H=128 selected before measurement: bitmap work grows with validated history.\n"
                      "H: messages already sent and acknowledged. 16 new messages per run. Existing diagnostic work counters remain unchanged.", fontsize=9)
    helpers.save(figure, "single-destination-performance")
    plt.close(figure)


def cachegrind_runs(variant):
    directory = ROOT / "cachegrind-comparison" / variant
    runs = cache_plots.read_csv(directory / "runs.csv")
    assert len(runs) == 9
    seen = set()
    for run in runs:
        history, repetition = int(run["history"]), int(run["repetition"])
        assert (history, repetition) not in seen
        seen.add((history, repetition))
        run.update(history=history, repetition=repetition, completed=int(run["completed"]))
        assert run["outcome"] == run["benchmark_outcome"] == "complete"
        assert run["benchmark_validated"].lower() == "true" and not run["failure"]
        assert run["completed"] == 16 and int(run["returncode"]) == 0
        assert run["artifact_directory"] == f"h{history}-r{repetition}"
        attempt = directory / run["artifact_directory"]
        raw_path = attempt / "cachegrind.out"
        raw = cache_runner.parse_profile(raw_path)
        for metric in cache_runner.EVENTS + cache_runner.DERIVED:
            run[metric] = int(run[metric])
            assert run[metric] == raw[metric]
        text = raw_path.read_text(encoding="utf-8")
        for name, model in cache_runner.CACHE_MODEL.items():
            size, ways, line = model.split(",")
            assert re.search(rf"^desc: {name} cache:\s+{size} B, {line} B, {ways}-way associative$", text, re.M)
        report = json.loads((attempt / "report.json").read_text(encoding="utf-8"))
        assert report["framework_version"] == VERSIONS[variant]
        assert report["cachegrind_region"] == "cycle" and report["cachegrind_enabled"] is True
        assert report["instrumentation_enabled"] is False and report["collection"] == "none"
        assert report["outcome"] == "complete" and report["completed"] == 16
    assert seen == {(history, repetition) for history in HISTORIES for repetition in (1, 2, 3)}
    summaries = cache_plots.read_csv(directory / "summary.csv")
    expected = {(history, metric) for history in HISTORIES
                for metric in cache_runner.EVENTS + cache_runner.DERIVED}
    assert len(summaries) == len(expected)
    seen = set()
    for row in summaries:
        key = int(row["history"]), row["metric"]
        assert key in expected and key not in seen
        seen.add(key)
        assert int(row["completed_profiles"]) == 3 and int(row["failed_profiles"]) == 0
        values = [run[key[1]] for run in runs if run["history"] == key[0]]
        for name, value in (("median", statistics.median(values)), ("min", min(values)), ("max", max(values))):
            assert float(row[name + "_run_total"]) == value
            assert float(row[name + "_run_per_message"]) == value / 16
    return runs


def work():
    figure, axes = plt.subplots(1, 2, figsize=(12, 5.5), constrained_layout=True)
    for variant, label, color, shift in VARIANTS:
        runs = cachegrind_runs(variant)
        for axis, event in zip(axes, ("Ir", "Dw")):
            groups = [[run[event] / run["completed"] / 1e6 for run in runs if run["history"] == history]
                      for history in HISTORIES]
            plot_groups(axis, groups, label, color, shift, 2)
    axes[0].set(title="Cachegrind instructions per completed cycle", ylabel="Ir / cycle (millions)")
    axes[1].set(title="Modeled data writes per completed cycle", ylabel="Dw / cycle (millions)")
    for axis in axes:
        axis.set_xticks(range(len(HISTORIES)), [f"H={history}" for history in HISTORIES])
        axis.set_xlabel("Previously sent and acknowledged messages")
        helpers.style(axis)
        axis.legend(frameon=False, loc="upper left")
    figure.suptitle("Single-destination validation · Linux x86_64 · matched Cachegrind profiles · diagnostics OFF", fontsize=12)
    figure.supxlabel("Bars: three-profile medians. Whiskers: full observed ranges, not confidence intervals. 16 cycles per profile.\n"
                      "Cold simulated region; fixed model: 32 KiB I1/D1, 8 MiB LL, 64-byte lines.\n"
                      "Counts are not native latency or retired hardware instructions; x86 REP iterations are counted separately.", fontsize=9)
    helpers.save(figure, "single-destination-work")
    plt.close(figure)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--plot", choices=("all", "native", "cachegrind"), default="all")
    options = parser.parse_args()
    plt.rcParams.update({"font.size": 10, "svg.hashsalt": "angaria-4.25"})
    if options.plot in ("all", "native"):
        performance()
    if options.plot in ("all", "cachegrind"):
        work()
    print(f"Wrote {options.plot} single-destination comparison figures from retained measurements.")
