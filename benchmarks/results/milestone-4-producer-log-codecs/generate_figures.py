#!/usr/bin/env python3
"""Reconstruct codec summaries/figures from ten retained performance-test runs."""

import argparse
import csv
import json
import math
import os
from pathlib import Path
import statistics
import tempfile

PAYLOADS = (64, 256, 1024)
PROFILES = (("legacy_owning", "Format 2: owning", "#a65a1a", -0.18, 3),
            ("owned_borrowed", "Format 3: caller buffer / borrowed", "#166ea0", 0.18, 0))
RUNS, SAMPLES, CYCLES = 10, 9, 2048


def require(condition, message):
    if not condition:
        raise ValueError(message)


def number(value, name):
    require(type(value) in (int, float) and math.isfinite(value), f"{name}: expected finite number")
    return value


def unique_object(pairs):
    result = {}
    for key, value in pairs:
        require(key not in result, f"duplicate JSON key: {key}")
        result[key] = value
    return result


def reconstruct(root):
    files = sorted((root / "raw").glob("run-*.jsonl"))
    require(len(files) == RUNS, f"expected {RUNS} raw/run-*.jsonl files, found {len(files)}")
    values = {(payload, profile): [] for payload in PAYLOADS for profile, *_ in PROFILES}
    for file in files:
        records = [json.loads(line, object_pairs_hook=unique_object)
                   for line in file.read_text(encoding="utf-8").splitlines() if line.strip()]
        require(len(records) == len(PAYLOADS), f"{file}: expected three payload records")
        seen = set()
        for record in records:
            payload = record["payload_bytes"]
            require(type(payload) is int and payload in PAYLOADS and payload not in seen,
                    f"{file}: invalid or duplicate payload")
            seen.add(payload)
            require(type(record["samples"]) is int and record["samples"] == SAMPLES,
                    f"{file}: wrong sample count")
            require(type(record["cycles_per_sample"]) is int and record["cycles_per_sample"] == CYCLES,
                    f"{file}: wrong cycles/sample")
            expected_sizes = {"owned_record_bytes": ((160 + payload + 63) // 64) * 64,
                              "legacy_body_bytes": 176 + payload,
                              "legacy_full_record_bytes": ((368 + payload + 63) // 64) * 64}
            for key, expected in expected_sizes.items():
                require(type(record[key]) is int and record[key] == expected, f"{file}: wrong {key}")
            for profile, _, _, _, allocations in PROFILES:
                item = record[profile]
                samples = item["cycle_ns_samples"]
                require(type(samples) is list and len(samples) == SAMPLES, f"{file}/{profile}: wrong samples")
                require(all(number(value, "sample") > 0 for value in samples), f"{file}/{profile}: nonpositive sample")
                middle = statistics.median(samples)
                cached = number(item["median_cycle_ns"], "cached median")
                require(abs(cached - middle) <= 0.0015, f"{file}/{profile}: stale median")
                rate = number(item["cycles_per_second"], "cached rate")
                require(rate > 0 and math.isclose(rate, 1e9 / middle, rel_tol=2e-5),
                        f"{file}/{profile}: stale rate beyond printed-sample rounding")
                require(number(item["allocations_per_cycle"], "allocations") == allocations,
                        f"{file}/{profile}: allocation contract changed")
                values[payload, profile].append(middle)
        require(seen == set(PAYLOADS), f"{file}: incomplete payload set")
    rows = []
    for payload in PAYLOADS:
        for profile, _, _, _, allocations in PROFILES:
            medians = values[payload, profile]
            rates = [1e9 / value for value in medians]
            rows.append(dict(payload_bytes=payload, profile=profile, runs=RUNS,
                             samples_per_run=SAMPLES, cycles_per_sample=CYCLES,
                             median_cycle_ns=statistics.median(medians),
                             min_run_median_cycle_ns=min(medians), max_run_median_cycle_ns=max(medians),
                             median_cycles_per_second=statistics.median(rates),
                             min_run_cycles_per_second=min(rates), max_run_cycles_per_second=max(rates),
                             allocations_per_cycle=allocations))
    return rows


def write_summary(root, rows):
    with (root / "summary.csv").open("w", newline="", encoding="utf-8") as stream:
        writer = csv.DictWriter(stream, fieldnames=list(rows[0]), lineterminator="\n")
        writer.writeheader()
        writer.writerows({key: f"{value:.6f}" if isinstance(value, float) else value
                         for key, value in row.items()} for row in rows)


def draw(root, rows):
    os.environ.setdefault("MPLCONFIGDIR", str(Path(tempfile.gettempdir()) / "angaria-matplotlib"))
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt
    plt.rcParams.update({"font.size": 10, "svg.hashsalt": "angaria-producer-log-codecs"})
    figure, axes = plt.subplots(1, 2, figsize=(12.5, 6.3))
    figure.subplots_adjust(left=0.075, right=0.98, top=0.80, bottom=0.28, wspace=0.25)
    for profile, label, color, shift, _ in PROFILES:
        selected = [next(row for row in rows if row["payload_bytes"] == payload and row["profile"] == profile)
                    for payload in PAYLOADS]
        for axis, center_key, low_key, high_key, scale in (
                (axes[0], "median_cycle_ns", "min_run_median_cycle_ns", "max_run_median_cycle_ns", 1),
                (axes[1], "median_cycles_per_second", "min_run_cycles_per_second", "max_run_cycles_per_second", 1e6)):
            centers = [row[center_key] / scale for row in selected]
            errors = [[(row[center_key] - row[low_key]) / scale for row in selected],
                      [(row[high_key] - row[center_key]) / scale for row in selected]]
            axis.bar([index + shift for index in range(len(PAYLOADS))], centers, width=0.34,
                     yerr=errors, capsize=4, label=label, color=color, edgecolor="white", linewidth=0.5)
    for axis in axes:
        axis.set_xticks(range(len(PAYLOADS)), [f"{payload} B" for payload in PAYLOADS])
        axis.set_xlabel("Application payload")
        axis.set_ylim(bottom=0)
        axis.grid(axis="y", alpha=0.25)
        axis.set_axisbelow(True)
        axis.spines[["top", "right"]].set_visible(False)
    axes[0].set(title="Encode + decode cycle time", ylabel="Nanoseconds / codec cycle (lower is better)")
    axes[1].set(title="Encode + decode codec rate", ylabel="Million codec cycles / second (higher is better)")
    figure.suptitle("Producer-log codecs: local encode + decode", fontsize=15, y=0.98)
    figure.legend(*axes[0].get_legend_handles_labels(), loc="upper center", ncol=2,
                  bbox_to_anchor=(0.5, 0.925), frameon=False)
    figure.text(0.5, 0.055,
                "Bars: medians across 10 run medians; each run has nine 2,048-cycle block means. Whiskers: min/max run medians, not p99.\n"
                "Caller-buffer encode / borrowed decode and format-2 owning encode/decode have different layouts and ownership contracts.\n"
                "Codec costs only: NOT transport latency, delivered-message throughput or a nanomsg comparison.\n"
                "Reconstructed from samples rounded to 0.001 ns; calculated rates inherit that quantization.",
                ha="center", va="bottom", fontsize=8.5, linespacing=1.5)
    figure.savefig(root / "latency-throughput.png", dpi=170, metadata={"Software": "Angaria codec study"})
    figure.savefig(root / "latency-throughput.svg", metadata={"Date": None, "Creator": "Angaria codec study"})
    plt.close(figure)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, default=Path(__file__).resolve().parent)
    parser.add_argument("--validate-only", action="store_true")
    options = parser.parse_args()
    rows = reconstruct(options.root)
    if not options.validate_only:
        draw(options.root, rows)
        write_summary(options.root, rows)
    print(f"Validated {RUNS} runs / {RUNS * len(PAYLOADS)} records / "
          f"{RUNS * len(PAYLOADS) * len(PROFILES) * SAMPLES} block means.")
    if not options.validate_only:
        print("Wrote summary.csv and latency-throughput.png/.svg from reconstructed samples.")


if __name__ == "__main__":
    main()
