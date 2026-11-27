#!/usr/bin/env python3
"""Plot retained Cachegrind cycle counts and simulated miss rates; needs matplotlib."""
import csv
import importlib.util
from pathlib import Path
import statistics

ROOT = Path(__file__).resolve().parent
DATA = ROOT / "linux-x86_64"
spec = importlib.util.spec_from_file_location(
    "cachegrind_plot_helpers_424", ROOT.parent / "milestone-4-gate-resources/generate_figures.py")
previous = importlib.util.module_from_spec(spec)
spec.loader.exec_module(previous)
helpers = previous.helpers
helpers.ROOT = ROOT
plt = helpers.plt
HISTORIES = (0, 32, 128)
EVENTS = ("Ir", "I1mr", "ILmr", "Dr", "D1mr", "DLmr", "Dw", "D1mw", "DLmw",
          "Bc", "Bcm", "Bi", "Bim")
DERIVED = ("data_references", "d1_data_misses", "ll_data_misses", "ll_total_misses",
           "branches", "branch_mispredictions")


def read_csv(path):
    with path.open(newline="", encoding="utf-8") as source:
        return list(csv.DictReader(source))


def raw_totals(path):
    events, totals = None, None
    for line in path.read_text(encoding="utf-8").splitlines():
        if line.startswith("events:"):
            assert events is None
            events = line.split()[1:]
        elif line.startswith("summary:"):
            assert totals is None
            totals = [int(value) for value in line.split()[1:]]
    assert events is not None and totals is not None
    assert len(events) == len(set(events)) == len(totals) == len(EVENTS)
    assert set(events) == set(EVENTS)
    return dict(zip(events, totals))


def validated_runs():
    runs = read_csv(DATA / "runs.csv")
    assert len(runs) == 9
    seen = set()
    for run in runs:
        run["history"] = int(run["history"])
        run["repetition"] = int(run["repetition"])
        run["completed"] = int(run["completed"])
        identity = (run["history"], run["repetition"])
        assert identity not in seen
        seen.add(identity)
        assert run["outcome"] == run["benchmark_outcome"] == "complete"
        assert run["benchmark_validated"].lower() == "true"
        assert run["completed"] == 16 and int(run["returncode"]) == 0 and not run["failure"]
        for metric in EVENTS + DERIVED:
            run[metric] = int(run[metric])
            assert run[metric] >= 0
        assert run["Ir"] > 0 and run["data_references"] > 0
        expected = dict(data_references=run["Dr"] + run["Dw"],
                        d1_data_misses=run["D1mr"] + run["D1mw"],
                        ll_data_misses=run["DLmr"] + run["DLmw"],
                        ll_total_misses=run["ILmr"] + run["DLmr"] + run["DLmw"],
                        branches=run["Bc"] + run["Bi"],
                        branch_mispredictions=run["Bcm"] + run["Bim"])
        assert all(run[metric] == value for metric, value in expected.items())
        assert run["artifact_directory"] == f"h{run['history']}-r{run['repetition']}"
        raw = raw_totals(DATA / run["artifact_directory"] / "cachegrind.out")
        assert all(run[metric] == raw[metric] for metric in EVENTS)
    assert seen == {(history, repetition) for history in HISTORIES for repetition in (1, 2, 3)}
    summaries = read_csv(DATA / "summary.csv")
    assert len(summaries) == len(HISTORIES) * len(EVENTS + DERIVED)
    seen_summary = set()
    for summary in summaries:
        history, metric = int(summary["history"]), summary["metric"]
        assert (history, metric) not in seen_summary
        seen_summary.add((history, metric))
        assert history in HISTORIES and metric in EVENTS + DERIVED
        assert int(summary["completed_profiles"]) == 3 and int(summary["failed_profiles"]) == 0
        values = [run[metric] for run in runs if run["history"] == history]
        for statistic, total in (("median", statistics.median(values)), ("min", min(values)),
                                 ("max", max(values))):
            assert float(summary[statistic + "_run_total"]) == total
            assert float(summary[statistic + "_run_per_message"]) == total / 16
    return runs


def ranges(groups):
    medians = [statistics.median(values) for values in groups]
    lows = [median - min(values) for median, values in zip(medians, groups)]
    highs = [max(values) - median for median, values in zip(medians, groups)]
    return medians, lows, highs


def history_figure():
    runs = validated_runs()
    agree = all(len({run[event] for run in runs if run["history"] == history}) == 1
                for history in HISTORIES for event in EVENTS)
    figure, axes = plt.subplots(1, 2, figsize=(12, 5.6), constrained_layout=True)
    instruction_groups = [[run["Ir"] / run["completed"] / 1e6 for run in runs
                           if run["history"] == history] for history in HISTORIES]
    medians, lows, highs = ranges(instruction_groups)
    bars = axes[0].bar(range(len(HISTORIES)), medians, width=0.56, color="#195c9c",
                       yerr=[lows, highs], capsize=4)
    for bar, median, high in zip(bars, medians, highs):
        axes[0].annotate(f"{median:.2f}", (bar.get_x() + bar.get_width() / 2, median + high),
                         xytext=(0, 5), textcoords="offset points", ha="center", fontsize=9)
    axes[0].set(title="Instructions per completed cycle", ylabel="Executed instructions (millions)")
    rate_groups = []
    for metric, label, color, shift in (("d1_data_misses", "D1 data misses / data references", "#a14f0a", -0.16),
                                        ("ll_data_misses", "LL data misses / data references", "#195c9c", 0.16)):
        # Compute each run's ratio before taking medians and observed ranges.
        groups = [[100 * run[metric] / run["data_references"] for run in runs
                   if run["history"] == history] for history in HISTORIES]
        rate_groups.extend(groups)
        medians, lows, highs = ranges(groups)
        positions = [index + shift for index in range(len(HISTORIES))]
        axes[1].errorbar(positions, medians, yerr=[lows, highs], fmt="o", color=color,
                         label=label, capsize=4, markersize=6)
        for position, median, high in zip(positions, medians, highs):
            axes[1].annotate(f"{median:.3f}%", (position, median + high),
                             xytext=(0, 6), textcoords="offset points", ha="center", fontsize=9)
    axes[1].set(title="Simulated data-cache miss rates", ylabel="Misses / modeled data references (%)")
    for axis in axes:
        axis.set_xticks(range(len(HISTORIES)), [f"H={history}" for history in HISTORIES])
        axis.set_xlabel("Previously sent and acknowledged messages")
        helpers.style(axis)
    positive_rates = [value for group in rate_groups for value in group if value > 0]
    if len(positive_rates) == sum(map(len, rate_groups)) and max(positive_rates) / min(positive_rates) >= 20:
        axes[1].set_yscale("log")
        axes[1].set_ylim(min(positive_rates) * 0.55, max(positive_rates) * 2.2)
        axes[1].set_ylabel("Misses / modeled data references (%; log scale)")
    axes[1].legend(frameon=False, loc="upper left", fontsize=9)
    figure.suptitle("Cachegrind · Linux x86_64 · 0.4.26 · optimized build · diagnostics OFF", fontsize=12)
    variation = ("All three runs agree at each H; observed ranges collapse." if agree else
                 "Whiskers: full observed three-run ranges.")
    figure.supxlabel(f"Bars/points: three-run medians. {variation} No confidence intervals.\n"
                      "16 new issue/send/receive/ack cycles per run. Cold simulated region after setup/preload.\n"
                      "Fixed model: 32 KiB I1/D1, 8 MiB LL, 64-byte lines. Instruction/cache counts, not hardware timing.", fontsize=9)
    helpers.save(figure, "cachegrind-history")
    plt.close(figure)


if __name__ == "__main__":
    plt.rcParams.update({"font.size": 10, "svg.hashsalt": "angaria-4.24"})
    history_figure()
    print("Wrote cachegrind-history SVG and PNG from nine validated retained profiles.")
