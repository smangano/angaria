#!/usr/bin/env python3
"""Qualify study separation, real IPC controls, and diagnostic integrity.

Private benchmark validation complements the public primer. Deliberate report
mutations below are evidence-integrity checks, with no speed threshold.
"""
from collections import Counter
import copy
import csv
import json
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

sys.dont_write_bytecode = True
ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "benchmarks"))
import run_stall_study as study
from ipc_comparison import rejected, require


def check_order():
    require(study.balanced_order(1) == ("angaria", "recorded-spsc", "nanomsg", "nanomsg", "recorded-spsc", "angaria"),
            "First paired triple changed")
    orders = [study.balanced_order(block) for block in range(1, 6)]
    require(Counter(profile for order in orders for profile in order) ==
            {profile: 10 for profile in study.PROFILES}, "Unequal per-profile repetitions")
    # Three rotating blocks put each profile at each position exactly once.
    require(all(Counter(order[position] for order in orders[:3]) ==
                {profile: 1 for profile in study.PROFILES} for position in range(6)),
            "Rotating order failed to balance positions")


def run_study(ordinary, diagnostic, output, extra, history, payload, messages):
    command = [sys.executable, "-B", str(ROOT / "benchmarks/run_stall_study.py"), str(ordinary),
               "--diagnostic-executable", str(diagnostic), "--output", str(output),
               "--blocks", "1", "--control-repetitions", "1", "--budget-ms", "3000",
               "--timeout", "20", "--start-lead-ms", "20"] + extra
    result = subprocess.run(command, capture_output=True, text=True, timeout=80)
    require(result.returncode == 0, "Study failed; evidence at %s\n%s\n%s" % (output, result.stdout, result.stderr))
    manifest = json.loads((output / "study.json").read_text())
    require(manifest["status"] == "complete" and manifest["total_attempts"] == 11,
            "Study omitted a control or comparison attempt")
    expected = {"ordinary-concurrent", "ordinary-backlog", "diagnostics-concurrent", "diagnostics-backlog", "diagnostics-empty"}
    require({leaf["name"] for leaf in manifest["leaves"]} == expected, "Ordinary/diagnostic scenarios were combined")
    all_reports = {}
    for leaf in manifest["leaves"]:
        directory = output / leaf["name"]
        reports = [json.loads(line) for line in (directory / "reports.jsonl").read_text().splitlines()]
        require(len(reports) == leaf["attempts"], "Manifest lost attempts")
        all_reports[leaf["name"]] = reports
        instrumented = leaf["name"].startswith("diagnostics-")
        collection = "timings" if instrumented else "none"
        scenario = leaf["name"].split("-", 1)[1]
        metadata = (directory / "manifest.toml").read_text()
        require('"collection" = ' + json.dumps(collection) in metadata and
                '"scenario" = ' + json.dumps(scenario) in metadata, "Missing leaf observation scope")
        for report in reports:
            case = {key: report[key] for key in study.ipc.DIMENSIONS + study.EXTRA_DIMENSIONS}
            require((report["messages"], report["history"], report["payload_bytes"]) ==
                    (messages, history, payload if leaf["name"] == "ordinary-concurrent" else 64),
                    "Requested cohort dimensions changed")
            study.validate_attempt(report, case, instrumented, smoke=True)
            require(report["collection"] == collection and report["scenario"] == scenario,
                    "Incompatible populations share a leaf")
        with (directory / "summary.csv").open(newline="") as stream:
            rows = list(csv.DictReader(stream))
        require(rows and all(row["pooled_p99"] == row["pooled_p999"] in
                            ("insufficient_samples", "incomplete_population", "not_applicable") for row in rows),
                "Small controls invented production tail estimates")
        for name in ("operations", "counters", "phases"):
            require((directory / ("diagnostic-" + name + ".csv")).is_file(), "Diagnostic breakdown missing")
    current = all_reports["ordinary-concurrent"]
    require(Counter(row["profile"] for row in current) == {profile: 2 for profile in study.PROFILES},
            "Real comparisons lost a profile")
    require(all(row["acknowledged"] == (messages if row["profile"] == "angaria" else 0) for row in current),
            "Acknowledgment semantics were conflated")
    diagnostic_current = all_reports["diagnostics-concurrent"][0]
    operations = [operation for worker in diagnostic_current["worker_reports"].values()
                  for operation in worker["diagnostics"]["operations"]]
    require(sum(operation["phases"]["native_gate_wait"]["calls"] for operation in operations) > 0 and
            sum(operation["phases"]["local_gate_wait"]["calls"] for operation in operations) > 0,
            "Completed diagnostic operations did not capture the lock boundaries")
    return all_reports


def corrupt_checks(reports):
    ordinary = reports["ordinary-concurrent"][0]
    case = {key: ordinary[key] for key in study.ipc.DIMENSIONS + study.EXTRA_DIMENSIONS}
    for field, value in (("instrumentation_enabled", True), ("collection", "timings"), ("scenario", "backlog")):
        corrupt = copy.deepcopy(ordinary)
        corrupt["worker_reports"]["producer"][field] = value
        rejected(lambda: study.validate_attempt(corrupt, case, False))
    corrupt = copy.deepcopy(ordinary)
    corrupt["rates"]["bounded_cohort_verified_per_second"] *= 2
    rejected(lambda: study.validate_attempt(corrupt, case, False))
    diagnostic = reports["diagnostics-concurrent"][0]
    for kind in ("overflow", "phase", "counter"):
        corrupt = copy.deepcopy(diagnostic)
        operation = next(op for op in corrupt["worker_reports"]["producer"]["diagnostics"]["operations"] if op["calls"])
        if kind == "overflow":
            operation["overflow"] = True
        elif kind == "phase":
            phase = next(iter(operation["phases"].values()))
            phase["exclusive_ns"] = phase["inclusive_ns"] + 1
        else:
            name = next(iter(operation["counters"]))
            operation["counters"][name] = -1
        rejected(lambda: study.validate_diagnostics(corrupt, "timings", True))


def main():
    ordinary, diagnostic = (Path(argument).resolve(strict=True) for argument in sys.argv[1:3])
    output = Path(tempfile.mkdtemp(prefix="angaria-stall-test-"))
    try:
        check_order()
        first = run_study(ordinary, diagnostic, output / "smoke", ["--smoke"], 0, 64, 2)
        corrupt_checks(first)
        run_study(ordinary, diagnostic, output / "history",
                  ["--histories", "2", "--payload-bytes", "256", "--messages", "3", "--ring-slots", "8"], 2, 256, 3)
        # Refuse identical builds and inadequate ring credit before creating output.
        for index, arguments in enumerate((["--diagnostic-executable", str(ordinary)],
                                            ["--messages", "16", "--ring-slots", "8"])):
            destination = output / ("invalid-%d" % index)
            result = subprocess.run([sys.executable, "-B", str(ROOT / "benchmarks/run_stall_study.py"), str(ordinary),
                                     "--diagnostic-executable", str(diagnostic), "--output", str(destination)] + arguments,
                                    capture_output=True, timeout=5)
            require(result.returncode != 0 and not destination.exists(), "Invalid study created output")
    except Exception:
        print("Retained stall qualification evidence: " + str(output), file=sys.stderr)
        raise
    shutil.rmtree(output)
    print("Stall study: 22 real comparison/control attempts, scope/cache checks, and diagnostic boundaries passed")


if __name__ == "__main__":
    main()
