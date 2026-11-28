#!/usr/bin/env python3
"""Plot independently reconstructed end-to-end qualification attempts.

The collector owns raw evidence and its reconstruction. This module aggregates
only identical cases, keeps each run's quantiles separate and never removes an
attempt because of its timing. Its CLI reconstructs before writing any figure.
"""

import argparse
import csv
import io
import json
import math
from pathlib import Path
import statistics
import sys

sys.dont_write_bytecode = True
from producer_log_append_study import load_json, require, write_json


PROFILES = ("recorded-connection", "recorded-acknowledged-connection", "nanomsg",
            "angaria", "recorded-spsc")
LABELS = {
    "recorded-connection": "Angaria: recorded / unacknowledged",
    "recorded-acknowledged-connection": "Angaria: recorded / acknowledged",
    "nanomsg": "nanomsg IPC: unrecorded / unacknowledged",
    "angaria": "Legacy public Angaria: recorded / acknowledged",
    "recorded-spsc": "Legacy current-log SPSC experiment",
}
COLORS = dict(zip(PROFILES, ("#168469", "#356eab", "#cc702d", "#9a568f", "#777777")))

# Keys name reconstructed values, never a percentile pooled across processes or
# attempts. An absent ACK or insufficient tail sample stays absent in the plot.
PANELS = {
    "delivery_p50": ("Logical start → delivery p50", "Microseconds", 1000, True),
    "delivery_p99": ("Logical start → delivery p99", "Microseconds", 1000, True),
    "delivery_p999": ("Logical start → delivery p99.9", "Microseconds", 1000, True),
    "ack_p50": ("Logical start → consumer ACK p50", "Microseconds", 1000, True),
    "ack_p99": ("Logical start → consumer ACK p99", "Microseconds", 1000, True),
    "ack_p999": ("Logical start → consumer ACK p99.9", "Microseconds", 1000, True),
    "scheduled_p50": ("Scheduled release → delivery p50", "Microseconds", 1000, True),
    "scheduled_p99": ("Scheduled release → delivery p99", "Microseconds", 1000, True),
    "schedule_lag_p99": ("Producer schedule lateness p99", "Microseconds", 1000, True),
    "producer_p50": ("Producer path p50", "Microseconds", 1000, True),
    "completed_rate": ("Completed measured publications/s", "Publications / second", 1, True),
    "cohort_rate": ("Completed publications/s from common T0", "Publications / second", 1, True),
    "cpu_inclusive": ("Producer + consumer CPU through FINISH", "Microseconds / measured publication", 1000, True),
    "cpu_active": ("Producer + consumer CPU through own DONE", "Microseconds / measured publication", 1000, True),
    "minor_faults": ("Minor faults through FINISH", "Faults / 1,000 measured publications", .001, False),
    "major_faults": ("Major faults through FINISH", "Faults / 1,000 measured publications", .001, False),
    "backpressure": ("Producer backpressure observations", "Observations / measured publication", 1, False),
    "credit_waits": ("Producer credit-wait observations", "Observations / measured publication", 1, False),
    "send_attempts": ("Producer send attempts", "Attempts / measured publication", 1, False),
    "receive_timeouts": ("Consumer receive timeouts", "Timeouts / measured publication", 1, False),
}


def case_value(case, name, default=0):
    aliases = {"messages": ("messages", "measured_messages"),
               "warmup_messages": ("warmup_messages",),
               "history": ("history", "initial_history_records"),
               "payload_bytes": ("payload_bytes",), "ring_slots": ("ring_slots",),
               "interval_ns": ("interval_ns",), "processing_delay_ns": ("processing_delay_ns",),
               "pause_at": ("pause_at",), "pause_ns": ("pause_ns",)}
    return next((case[key] for key in aliases.get(name, (name,)) if key in case), default)


def case_name(case):
    if isinstance(case, str):
        require(bool(case), "Every case needs its retained name")
        return case
    require(isinstance(case.get("name"), str) and case["name"], "Every case needs its retained name")
    return case["name"]


def measured_messages(case):
    return case.get("measured_messages", case_value(case, "messages") - case_value(case, "warmup_messages"))


def normalized(reports, manifest):
    """Resolve retained case names without changing the collector's evidence."""
    if isinstance(manifest["cases"], dict):
        cases = [dict(case, name=name) for name, case in sorted(manifest["cases"].items())]
    else:
        cases = manifest["cases"]
    named = {case_name(case): case for case in cases}
    require(len(named) == len(cases), "Duplicate retained case name")
    resolved = []
    for report in reports:
        case = report["case"]
        if isinstance(case, str):
            require(case in named, "Attempt names an unplanned case")
            case = named[case]
        resolved.append(dict(report, case=case))
    return resolved, dict(manifest, cases=cases)


def metric_values(record):
    """Expose the validator's named distributions/resources without raw samples."""
    result = {}
    for prefix, distribution in (
            ("delivery", "logical_start_to_delivery_ns"),
            ("ack", "logical_start_to_acknowledgment_ns"),
            ("scheduled", "scheduled_to_delivery_ns"),
            ("schedule_lag", "producer_schedule_lag_ns"),
            ("producer", "logical_producer_path_ns")):
        values = record.get("metrics", {}).get(distribution, {})
        for statistic in ("p50", "p99", "p999"):
            value = values.get(statistic + "_ns")
            if value is not None:
                result[prefix + "_" + statistic] = (value, values["count"])
    rates = record.get("rates", {})
    for target, source in (("completed_rate", "measured_completed_per_second"),
                           ("cohort_rate", "bounded_cohort_completed_per_second")):
        if source in rates:
            result[target] = rates[source], record["measured_count"]
    count = record["measured_count"]
    resources = record.get("resources", {}).get("totals", {})
    for target, source in (("cpu_inclusive", "inclusive_cpu_ns_per_publication"),
                           ("cpu_active", "active_cpu_ns_per_publication")):
        if source in resources:
            result[target] = resources[source], count
    for target, source in (("minor_faults", "inclusive_minor_faults"),
                           ("major_faults", "inclusive_major_faults")):
        if source in resources:
            result[target] = resources[source] / count, count
    counters = record.get("counters", {})
    for target, role, source in (
            ("backpressure", "producer", "measured_backpressure_count"),
            ("credit_waits", "producer", "measured_credit_waits"),
            ("send_attempts", "producer", "measured_send_attempts"),
            ("receive_timeouts", "consumer", "measured_receive_timeouts")):
        if source in counters.get(role, {}):
            result[target] = counters[role][source] / count, count
    for name, (value, samples) in result.items():
        require(type(value) in (int, float) and math.isfinite(value) and value >= 0,
                "Invalid reconstructed value: " + name)
        require(type(samples) is int and samples > 0, "Invalid sample count: " + name)
    return result


def summaries(reports, manifest):
    """Retain every attempt value; do not pool messages or different workloads."""
    reports, manifest = normalized(reports, manifest)
    require(reports and all(row.get("status") == "complete" for row in reports),
            "Incomplete qualification has no headline comparison; retain its status and every attempt")
    cases = manifest["cases"]
    names = [case_name(case) for case in cases]
    require(len(names) == len(set(names)), "Duplicate retained case name")
    require({case_name(row["case"]) for row in reports} == set(names), "Case plan and attempts differ")
    rows = []
    for case in cases:
        name = case_name(case)
        matched = [row for row in reports if case_name(row["case"]) == name]
        require(all(row["case"] == case for row in matched), "Different dimensions share a case name")
        for profile in PROFILES:
            selected = [row for row in matched if row["profile"] == profile]
            if not selected:
                continue
            attempts = [metric_values(row) for row in selected]
            keys = set(attempts[0])
            require(all(set(attempt) == keys for attempt in attempts),
                    "Missing metric in one run of " + name + "/" + profile)
            for metric in sorted(keys & PANELS.keys()):
                values = [attempt[metric][0] for attempt in attempts]
                counts = {attempt[metric][1] for attempt in attempts}
                require(len(counts) == 1, "Different sample counts cannot share a summary")
                rows.append(dict(case=name, profile=profile, metric=metric, runs=len(values),
                                 samples_per_attempt=next(iter(counts)), median=statistics.median(values),
                                 minimum=min(values), maximum=max(values), attempt_values=values))
    require({row["profile"] for row in reports} <= set(PROFILES), "Unknown profile in qualified comparison")
    return rows


def short_label(case):
    payload, history, credit = (case_value(case, key) for key in ("payload_bytes", "history", "ring_slots"))
    if measured_messages(case) >= 100000:
        return str(payload) + " B long\nN=" + format(measured_messages(case), ",")
    if is_control(case):
        return "H=" + format(history, ",") + "\nN=" + str(measured_messages(case))
    if case_value(case, "processing_delay_ns"):
        return format(case_value(case, "processing_delay_ns") / 1000, "g") + " µs work\n" + str(payload) + " B"
    if case_value(case, "pause_ns"):
        return format(case_value(case, "pause_ns") / 1000000, "g") + " ms pause\n" + str(payload) + " B"
    if case_value(case, "interval_ns"):
        return format(1e9 / case_value(case, "interval_ns"), ",.0f") + "/s\n" + str(payload) + " B"
    return str(payload) + " B\nH=" + format(history, ",") + "  K=" + str(credit)


def plot_group(output, manifest, cases, rows, panels, filename, title):
    if not cases:
        return None
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt
    matplotlib.rcParams["svg.hashsalt"] = "angaria-end-to-end-qualification"
    indexed = {(row["case"], row["profile"], row["metric"]): row for row in rows}
    panels = [metric for metric in panels if any((case_name(case), profile, metric) in indexed
                                                 for case in cases for profile in PROFILES)]
    if not panels:
        return None
    columns = 2
    height = math.ceil(len(panels) / columns)
    many_cases = len(cases) > 3
    figure, axes = plt.subplots(height, columns, figsize=(max(12, len(cases) * 1.25 + 6),
                                                         3.5 * height + (3.8 if many_cases else 2.4)),
                                squeeze=False)
    visible_profiles = [profile for profile in PROFILES if any((case_name(case), profile, panels[0]) in indexed
                                                               for case in cases)]
    for ax, metric in zip(axes.flat, panels):
        panel_title, unit, scale, logarithmic = PANELS[metric]
        has_positive = False
        for profile_index, profile in enumerate(visible_profiles):
            xs, centers, lower, upper = [], [], [], []
            for case_index, case in enumerate(cases):
                summary = indexed.get((case_name(case), profile, metric))
                if summary is None:
                    continue
                values = [value / scale for value in summary["attempt_values"]]
                # Exact zero is visible on linear axes. A log-scale panel uses a
                # symmetric log scale only when that measured value occurs.
                has_positive |= any(value > 0 for value in values)
                x = case_index + (profile_index - (len(visible_profiles) - 1) / 2) * .16
                shifts = [(index - (len(values) - 1) / 2) * .009 for index in range(len(values))]
                ax.scatter([x + shift for shift in shifts], values, s=20, alpha=.5,
                           color=COLORS[profile], zorder=3)
                center = summary["median"] / scale
                xs.append(x); centers.append(center)
                lower.append(center - min(values)); upper.append(max(values) - center)
            if xs:
                ax.errorbar(xs, centers, yerr=[lower, upper], marker="o", markersize=5, capsize=4,
                            color=COLORS[profile], linewidth=1.5, label=LABELS[profile], zorder=4)
        ax.set_title(panel_title, fontsize=11)
        ax.set_ylabel(unit, fontsize=9.5)
        ax.set_xticks(range(len(cases)))
        ax.set_xticklabels([short_label(case) for case in cases], fontsize=8 if many_cases else 8.5,
                           rotation=40 if many_cases else 0, ha="right" if many_cases else "center")
        if logarithmic and has_positive:
            values = [row["minimum"] / scale for row in rows if row["metric"] == metric and
                      row["case"] in {case_name(case) for case in cases}]
            if min(values) <= 0:
                ax.set_yscale("symlog", linthresh=.001)
                ax.set_ylim(bottom=0)
            else:
                ax.set_yscale("log")
        else:
            ax.set_ylim(bottom=0)
        ax.grid(axis="y", which="both", alpha=.18)
        ax.set_xlim(-.5, len(cases) - .5)
    for ax in list(axes.flat)[len(panels):]:
        ax.set_visible(False)
    handles, labels = [], []
    for ax in axes.flat:
        for handle, label in zip(*ax.get_legend_handles_labels()):
            if label not in labels:
                handles.append(handle); labels.append(label)
    figure.legend(handles, labels, loc="upper center", bbox_to_anchor=(.5, .945), frameon=False,
                  ncol=1 if len(labels) > 3 else 2, fontsize=9)
    figure.suptitle(title + "\n" + manifest.get("host_label", "retained host"), fontsize=13, y=.99)
    count_pairs = sorted({(measured_messages(case), case_value(case, "warmup_messages")) for case in cases})
    counts = "; ".join("N=" + format(count, ",") + ", warmup=" + format(warm, ",") for count, warm in count_pairs)
    footer = ("Dots: every run; center: median; whiskers: full run range. Latency dots are each run's nearest-rank quantile.\n" +
              counts + ". Cases and quantiles are never pooled. H: prior history/traffic; K: Angaria ring slots.\n" +
              "ACK only for acknowledged modes. Angaria retains replay; nanomsg is unrecorded/unacknowledged. Finite logs; no endless-stream claim.\n" +
              "Completed rate includes final verification, producer send/lookup and selected consumer ACK. No timing outliers removed.")
    figure.text(.5, .025, footer, ha="center", va="bottom", fontsize=8.5, linespacing=1.5)
    figure.subplots_adjust(top=.77 if len(labels) > 3 else .80, bottom=.24 if many_cases else .20,
                           hspace=.62 if many_cases else .43, wspace=.29)
    for suffix in ("png", "svg"):
        figure.savefig(output / (filename + "." + suffix), dpi=160,
                       metadata={"Software": "Angaria end-to-end qualification"} if suffix == "png" else
                       {"Date": None, "Creator": "Angaria end-to-end qualification"})
    plt.close(figure)
    return filename + ".png"


def is_normal(case):
    return (0 < measured_messages(case) < 100000 and not is_control(case) and
            not any(case_value(case, key) for key in ("interval_ns", "processing_delay_ns", "pause_ns")))


def is_control(case):
    return case_value(case, "warmup_messages") == 0 and measured_messages(case) <= 128


def summary_document(reports, rows):
    return dict(schema_version=1, attempts=len(reports),
                interpretation="Per-run quantiles; no pooled tails or timing exclusions", rows=rows)


def comparison_text(rows):
    stream = io.StringIO(newline="")
    fields = ("case", "profile", "metric", "runs", "samples_per_attempt", "median", "minimum", "maximum", "attempt_values")
    writer = csv.DictWriter(stream, fieldnames=fields, lineterminator="\n")
    writer.writeheader()
    for row in rows:
        writer.writerow(dict(row, attempt_values=json.dumps(row["attempt_values"], separators=(",", ":"))))
    return stream.getvalue()


def verify_summaries(output, reports, manifest):
    """Reject a retained aggregate that disagrees with reconstructed attempts."""
    rows = summaries(reports, manifest)
    path = Path(output) / "qualification-summary.json"
    if path.exists():
        retained = load_json(path.read_text(encoding="utf-8"))
        require(json.dumps(retained, sort_keys=True) == json.dumps(summary_document(reports, rows), sort_keys=True),
                "Published qualification summary differs from reconstructed attempts")
    path = Path(output) / "comparison.csv"
    if path.exists():
        require(path.read_text(encoding="utf-8") == comparison_text(rows),
                "Published comparison CSV differs from reconstructed attempts")
    return rows


def render(output, reports, manifest):
    """Write summaries/figures only from the collector's verified reconstruction."""
    output = Path(output)
    reports, manifest = normalized(reports, manifest)
    if "host_label" not in manifest:
        manifest = dict(manifest, host_label=manifest.get("host", {}).get("host_label", "retained host"))
    rows = verify_summaries(output, reports, manifest)
    write_json(output / "qualification-summary.json", summary_document(reports, rows))
    (output / "comparison.csv").write_text(comparison_text(rows), encoding="utf-8")
    cases = manifest["cases"]
    reference = next((case for case in cases if case_name(case) == "saturation-64"), None)
    default_history = case_value(reference, "history") if reference else 8192
    default_credit = case_value(reference, "ring_slots") if reference else 8
    core = sorted([case for case in cases if is_normal(case) and case_value(case, "history") == default_history and
                   case_value(case, "ring_slots") == default_credit], key=lambda case: case_value(case, "payload_bytes"))
    # Select by retained dimensions rather than hardcoded case names so a reduced
    # smoke plan is still honestly labeled by its actual N/H/K and payload.
    histories = sorted([case for case in cases if is_normal(case) and case_value(case, "payload_bytes") == 64 and
                        case_value(case, "ring_slots") == default_credit], key=lambda case: case_value(case, "history"))
    paced = sorted([case for case in cases if case_value(case, "interval_ns")],
                   key=lambda case: -case_value(case, "interval_ns"))
    flow = [case for case in cases if 0 < measured_messages(case) < 100000 and not is_control(case) and
            case_value(case, "payload_bytes") == 64 and case_value(case, "history") == default_history and
            not case_value(case, "interval_ns")]
    flow.sort(key=lambda case: (case_value(case, "processing_delay_ns") > 0,
                               case_value(case, "pause_ns") > 0, case_value(case, "ring_slots")))
    controls = sorted([case for case in cases if is_control(case)], key=lambda case: case_value(case, "history"))
    long = [case for case in cases if measured_messages(case) >= 100000]
    figures = []
    plans = [
        (core, ("delivery_p50", "delivery_p99", "ack_p99", "completed_rate"), "latency-throughput", "Warmed finite streams: delivery and completed work"),
        (histories, ("delivery_p50", "delivery_p99", "completed_rate", "cpu_inclusive"), "history-sensitivity", "Retained-history sensitivity, 64-byte messages"),
        (paced, ("scheduled_p50", "scheduled_p99", "schedule_lag_p99", "completed_rate"), "offered-load", "Fixed offered load: scheduled latency and completed work"),
        (flow, ("delivery_p99", "completed_rate", "backpressure", "credit_waits"), "backpressure", "Credit, slow processing and a paused consumer"),
        ([case for case in cases if case not in controls], ("cpu_inclusive", "cpu_active", "minor_faults", "major_faults"), "cpu-faults", "Process CPU and VM faults after live warmup"),
        (long, ("delivery_p99", "delivery_p999", "ack_p999", "completed_rate"), "long-stream", "Long finite stream: per-run tail qualification"),
        (controls, ("delivery_p50", "cohort_rate", "completed_rate", "cpu_inclusive"), "legacy-controls", "Short controls: current modes and retained legacy adapters"),
    ]
    for selected, panels, filename, title in plans:
        result = plot_group(output, manifest, selected, rows, panels, filename, title)
        if result:
            figures.append(result)
    write_json(output / "figures.json", dict(figures=figures, attempts=len(reports)))
    return rows


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("output", type=Path)
    parser.add_argument("--validate-only", action="store_true")
    options = parser.parse_args()
    # Import only for standalone reconstruction: the collector can import render
    # after its own validation without a module-initialization cycle.
    from run_end_to_end import reconstruct
    reports, manifest = reconstruct(options.output.resolve())
    if options.validate_only:
        verify_summaries(options.output.resolve(), reports, manifest)
    else:
        render(options.output.resolve(), reports, manifest)
    print("Verified " + str(len(reports)) + " end-to-end attempts; no timing-based exclusions.")


if __name__ == "__main__":
    try:
        main()
    except (OSError, RuntimeError, ValueError) as error:
        print("End-to-end report failed: " + str(error), file=sys.stderr)
        sys.exit(1)
