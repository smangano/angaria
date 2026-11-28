#!/usr/bin/env python3
"""Plot the already recorded secondary active-window metric; run no benchmark."""

import argparse
import csv
from pathlib import Path
import statistics
import sys

sys.dont_write_bytecode = True
HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parents[1]))
import recorded_connection_study as study


def generate(directory):
    # Validate the original archive, hashes and authoritative worker rows before
    # reading either metric. The primary figure and evidence remain untouched.
    manifest, reports = study.reconstruct(directory)
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt
    matplotlib.rcParams["svg.hashsalt"] = "angaria-recorded-connection-active"
    metrics = (("logical_start_to_delivery_ns", "Delivery latency (µs, log)", 1000),
               ("first_send_to_last_verified_per_second", "Active verified rate (pub/s, log)", 1))
    payloads = sorted({row["payload_bytes"] for row in reports})
    histories = sorted({row["history"] for row in reports})
    figure, axes = plt.subplots(2, len(payloads), figsize=(5.1 * len(payloads), 9.0), squeeze=False)
    table = []
    for column, payload in enumerate(payloads):
        for row_index, (metric, ylabel, scale) in enumerate(metrics):
            ax = axes[row_index, column]
            for profile_index, profile in enumerate(study.PROFILES):
                xs, centers, lows, highs = [], [], [], []
                for history_index, history in enumerate(histories):
                    group = [row for row in reports if (row["profile"], row["history"], row["payload_bytes"]) ==
                             (profile, history, payload)]
                    values = [study.ipc.rank(row["metrics"][metric], 1, 2) if metric in row["metrics"] else
                              row["rates"][metric] for row in group]
                    center = statistics.median(values)
                    table.append(dict(profile=profile, history=history, payload_bytes=payload, metric=metric,
                                      runs=len(values), median=center, minimum=min(values), maximum=max(values)))
                    values = [value / scale for value in values]
                    x = history_index + (profile_index - .5) * .11
                    jitter = [(index - (len(values) - 1) / 2) * .007 for index in range(len(values))]
                    ax.scatter([x + shift for shift in jitter], values, s=19, alpha=.5, color=study.COLORS[profile])
                    xs.append(x); centers.append(center / scale)
                    lows.append(center / scale - min(values)); highs.append(max(values) - center / scale)
                ax.errorbar(xs, centers, yerr=[lows, highs], marker="o", capsize=4, color=study.COLORS[profile],
                            linewidth=1.7, label=study.LABELS[profile])
            ax.set_xticks(range(len(histories)), [str(value) for value in histories])
            ax.set_xlabel("Initial retained messages (H)")
            if column == 0:
                ax.set_ylabel(ylabel, fontsize=10)
            ax.set_yscale("log"); ax.grid(axis="y", alpha=.2)
            if row_index == 0:
                ax.set_title(str(payload) + "-byte payload")
    handles, labels = axes[0, 0].get_legend_handles_labels()
    figure.legend(handles, labels, loc="upper center", bbox_to_anchor=(.5, .94), frameon=False, fontsize=10)
    figure.suptitle("Secondary active-window comparison — " + manifest["runner_metadata"].get("host_label", ""), fontsize=13)
    figure.subplots_adjust(top=.78, bottom=.23, hspace=.65, wspace=.38)
    messages = study.cohort_size(reports)
    capacity = manifest["runner_metadata"]["ring_slots"]
    socket_bytes = reports[0]["socket_buffer_bytes"]
    resolutions = sorted({worker["clock_resolution_ns"] for row in reports for worker in row["worker_reports"].values()})
    figure.text(.5, .025,
                "Existing secondary rate: first send → completed consumer verification and required Angaria producer lookup.\n"
                "Only the initial T0-to-first-send delay is excluded; the original common-T0 cohort rates remain retained.\n"
                "Dots: every attempt; lines: median; whiskers: full range. Latency: logical start → receive return; points: per-run nearest-rank p50.\n"
                f"Finite {messages}-message cohorts; {capacity} Angaria logical credits vs {socket_bytes:,}-byte nanomsg socket buffers. Different recording guarantees.\n"
                "Reported clock resolution: " + "/".join(str(value) for value in resolutions) +
                " ns. No sustained-capacity or p99 claim; no timing outliers discarded.",
                ha="center", fontsize=9, linespacing=1.6)
    for suffix in ("png", "svg"):
        path = directory / ("active-window-latency-throughput." + suffix)
        figure.savefig(path, dpi=170, metadata={"Software": "Angaria secondary active-window study"} if suffix == "png" else
                       {"Date": None, "Creator": "Angaria secondary active-window study"})
        if suffix == "svg":
            path.write_text("\n".join(line.rstrip() for line in path.read_text().splitlines()) + "\n")
    plt.close(figure)
    with (directory / "active-comparison.csv").open("w", newline="") as stream:
        writer = csv.DictWriter(stream, fieldnames=list(table[0]), lineterminator="\n")
        writer.writeheader(); writer.writerows(table)
    print(str(directory) + ": reconstructed original secondary metric for " + str(len(reports)) + " attempts")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("directories", nargs="*", type=Path)
    options = parser.parse_args()
    directories = options.directories or sorted(path for path in HERE.iterdir() if (path / "manifest.json").is_file())
    study.ipc.check(bool(directories), "No retained host dataset")
    for directory in directories:
        generate(directory.resolve())


if __name__ == "__main__":
    main()
