#!/usr/bin/env python3
"""Keep ordinary IPC comparisons and focused stall diagnostics separate.

The ordinary matrix balances current acknowledged delivery, the experimental
recorded SPSC path, and libnanomsg. Backlog and empty-wait controls have separate
artifact directories, denominators, and summaries. No machine speed is a test.
"""
import argparse
import csv
from datetime import datetime, timezone
import json
from pathlib import Path
import sys

sys.dont_write_bytecode = True
import run_ipc_comparison as ipc

PROFILES = ("angaria", "recorded-spsc", "nanomsg")
OPERATIONS = ("issue", "send", "receive", "acknowledge")
EXTRA_DIMENSIONS = ("scenario", "collection", "ring_slots")
REQUIRED_PHASES = {"local_gate_wait", "native_gate_wait", "gate_acquire", "prefix_scan", "topology"}
REQUIRED_COUNTERS = {"gate_attempts", "gate_acquisitions", "empty_polls", "prefix_scans", "topology_builds", "native_gate_hold_ns"}


def balanced_order(block):
    """Two observations/profile/block; rotate positions across blocks."""
    offset = (block - 1) % len(PROFILES)
    forward = PROFILES[offset:] + PROFILES[:offset]
    return forward + forward[::-1]


def validate_diagnostics(report, collection, enabled):
    """Validate declared observation scope without inventing missing samples."""
    for role, worker in report["worker_reports"].items():
        ipc.check(worker.get("instrumentation_enabled") is enabled, "Wrong instrumentation build")
        ipc.check(worker.get("collection") == collection, "Worker collection differs from requested scope")
        diagnostics = worker.get("diagnostics")
        ipc.check(type(diagnostics) is dict and type(diagnostics.get("operations")) is list,
                  "Missing bounded diagnostic operation reports")
        operations = diagnostics["operations"]
        names = [operation.get("name") for operation in operations]
        ipc.check(len(names) == len(set(names)) and all(name in OPERATIONS for name in names),
                  "Unexpected/repeated diagnostic operation")
        if enabled:
            ipc.check(set(names) == set(OPERATIONS), "Instrumented report omitted an operation category")
        for operation in operations:
            ipc.integer(operation.get("calls"), "diagnostic calls")
            ipc.integer(operation.get("wall_ns"), "diagnostic root wall")
            ipc.integer(operation.get("cpu_ns"), "diagnostic root CPU")
            ipc.check(operation.get("valid") is True and operation.get("overflow") is False,
                      "Invalid/overflowed diagnostic capture")
            ipc.check(type(operation.get("phases")) is dict and type(operation.get("counters")) is dict,
                      "Missing phase/counter breakdown")
            if enabled:
                ipc.check(REQUIRED_PHASES <= operation["phases"].keys() and
                          REQUIRED_COUNTERS <= operation["counters"].keys(), "Required stall diagnostics are missing")
            if operation["calls"]:
                ipc.check(operation["name"] in (("issue", "send") if role == "producer" else ("receive", "acknowledge")),
                          "Operation attributed to the wrong worker")
            for name, value in operation["counters"].items():
                ipc.check(type(name) is str and bool(name), "Invalid counter name")
                ipc.integer(value, "diagnostic " + name)
                if collection == "none":
                    ipc.check(value == 0, "Uninstrumented run retained diagnostic counters")
            for name, phase in operation["phases"].items():
                ipc.check(type(name) is str and bool(name) and type(phase) is dict, "Invalid phase entry")
                for field in ("calls", "inclusive_ns", "exclusive_ns"):
                    ipc.integer(phase.get(field), "diagnostic phase " + field)
                ipc.check(phase["exclusive_ns"] <= phase["inclusive_ns"], "Phase exclusive time exceeds inclusive")
                if collection != "timings":
                    ipc.check(phase["inclusive_ns"] == phase["exclusive_ns"] == 0,
                              "Counts/ordinary run retained phase clocks")


def validate_attempt(report, case, instrumented, smoke=False):
    ipc.check(report.get("status") != "failed", report.get("failure") or "Study participant failed")
    for role, worker in report["worker_reports"].items():
        for name in ("scenario", "collection"):
            ipc.check(worker.get(name) == case[name], "Worker differs from requested " + name)
        expected_slots = 0 if report["profile"] == "nanomsg" else case["ring_slots"]
        ipc.check(worker.get("ring_slots") == expected_slots, "Worker ring geometry differs from its profile")
    ipc.validate_cached_summary(report)
    validate_diagnostics(report, case["collection"], instrumented)
    if case["scenario"] == "empty":
        ipc.check(report["status"] == "budget_exceeded" and report["accepted"] == report["delivered"] ==
                  report["acknowledged"] == report["pending"] == 0, "Empty-wait control delivered unexpected work")
        ipc.check(report["rates"]["bounded_cohort_verified_per_second"] == 0,
                  "Empty-wait control must retain a zero-work rate")
    elif smoke:
        ipc.check(report["status"] == "complete", "Qualification cohort did not complete")
    if report["profile"] == "recorded-spsc":
        ipc.check(report["acknowledged"] == 0, "Experimental SPSC gained processing acknowledgments")


def write_diagnostics(directory, reports):
    common = ("attempt", "profile", "history", "payload_bytes", "scenario", "collection", "role", "pid", "operation")
    streams = []
    try:
        writers = {}
        for name, fields in (("operations", ("calls", "wall_ns", "cpu_ns")),
                             ("counters", ("counter", "value")),
                             ("phases", ("phase", "calls", "inclusive_ns", "exclusive_ns"))):
            stream = (directory / ("diagnostic-" + name + ".csv")).open("w", newline="", encoding="utf-8")
            streams.append(stream)
            writer = csv.DictWriter(stream, fieldnames=common + fields, lineterminator="\n")
            writer.writeheader()
            writers[name] = writer
        for report in reports:
            if report["status"] == "failed":
                continue
            for role, worker in report["worker_reports"].items():
                for operation in worker["diagnostics"]["operations"]:
                    row = {name: report[name] for name in common if name not in ("role", "pid", "operation")}
                    row.update(role=role, pid=worker["pid"], operation=operation["name"])
                    writers["operations"].writerow(dict(row, **{name: operation[name] for name in ("calls", "wall_ns", "cpu_ns")}))
                    for name, value in operation["counters"].items():
                        writers["counters"].writerow(dict(row, counter=name, value=value))
                    for name, phase in operation["phases"].items():
                        writers["phases"].writerow(dict(row, phase=name, **phase))
    finally:
        for stream in streams:
            stream.close()


def source_hashes(sources):
    # Include the native library and prototype inputs, excluding unrelated
    # duplicate cloud-sync files. Package pins remain in comparison metadata.
    for directory in ("src", "include/angaria", "benchmarks"):
        for path in sorted((ipc.ROOT / directory).rglob("*")):
            if path.is_file() and path.suffix in (".h", ".cpp") and " 2." not in path.name:
                sources[str(path.relative_to(ipc.ROOT))] = ipc.sha256(path)
    for name in ("benchmarks/run_stall_study.py", "test/stall_study.py"):
        path = ipc.ROOT / name
        if path.is_file():
            sources[name] = ipc.sha256(path)
    return sources


def run_leaf(output, name, executable, cases, orders, options, instrumented):
    directory = output / name
    directory.mkdir()
    manifest, sources, settings = ipc.comparison_metadata(executable, options)
    source_hashes(sources)
    manifest.update(suite="stall-study-1-to-1", status="running", instrumentation_enabled=instrumented,
                    collection=cases[0]["collection"], scenario=cases[0]["scenario"],
                    ring_slots=options.ring_slots, planned_runs=len(cases) * sum(map(len, orders)),
                    repetitions=sum(order.count(PROFILES[0]) for order in orders),
                    order_method="Rotating forward/reverse triples for ordinary comparisons; explicit order retained for controls",
                    throughput_scope="Finite cohorts; scenarios/collections remain separate; empty controls use actual stop duration",
                    summary_method="Median/min/max of per-run nearest-rank p50; no timing-based outlier removal",
                    semantic_profiles=["Current Angaria recorded/acknowledged", "Experimental current-log recorded SPSC/unacknowledged", "libnanomsg IPC unrecorded/unacknowledged"],
                    p99_min_samples=10000, p999_min_samples=1000000)
    reports = []
    version = (ipc.ROOT / "VERSION").read_text().strip()
    def persist():
        manifest.update(recorded_runs=len(reports), completed_runs=sum(r["status"] == "complete" for r in reports),
                        budget_exceeded_runs=sum(r["status"] == "budget_exceeded" for r in reports),
                        failed_runs=sum(r["status"] == "failed" for r in reports))
        ipc.write_artifacts(directory, reports, manifest, sources, settings, cases)
        write_diagnostics(directory, reports)
    persist()
    try:
        for case in cases:
            for block, order in enumerate(orders, 1):
                for position, profile in enumerate(order, 1):
                    index = len(reports) + 1
                    attempt_name = f"attempt-{index:04d}-{profile}-h{case['history']}-b{case['payload_bytes']}"
                    attempt_dir = directory / attempt_name
                    attempt_dir.mkdir()
                    report = ipc.attempt(executable, profile, case, attempt_dir, options, version)
                    report.update(attempt=index, block=block, position=position, order="/".join(order),
                                  artifact_directory=attempt_name)
                    reports.append(report)
                    try:
                        validate_attempt(report, case, instrumented, options.smoke)
                    except Exception as error:
                        report.update(status="failed", failure=f"{type(error).__name__}: {error}")
                        (attempt_dir / "report.json").write_text(json.dumps(report, indent=2, sort_keys=True) + "\n")
                        raise
                    finally:
                        persist()
                    print(f"{name} {index}/{manifest['planned_runs']} {profile} H={case['history']} B={case['payload_bytes']}: {report['status']}", flush=True)
        manifest["status"] = "complete"
    except Exception as error:
        manifest.update(status="failed", failure=f"{type(error).__name__}: {error}")
        raise
    finally:
        manifest["finished_utc"] = datetime.now(timezone.utc).isoformat()
        persist()
    return dict(name=name, status=manifest["status"], attempts=len(reports),
                completed=manifest["completed_runs"], budget_stops=manifest["budget_exceeded_runs"],
                executable_sha256=manifest["executable_sha256"])


def parse_arguments():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("executable", type=Path, help="ordinary diagnostics-OFF comparison executable")
    parser.add_argument("--diagnostic-executable", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True, help="new study directory")
    parser.add_argument("--blocks", type=int, default=5)
    parser.add_argument("--control-repetitions", type=int, default=3)
    parser.add_argument("--diagnostic-collection", choices=("timings", "counts"), default="timings",
                        help="separate enabled observation mode; never pooled with ordinary measurements")
    parser.add_argument("--histories", default="0,128")
    parser.add_argument("--payload-bytes", default="64,256")
    parser.add_argument("--messages", type=int, default=16)
    parser.add_argument("--ring-slots", type=int, default=32)
    parser.add_argument("--budget-ms", type=int, default=10000)
    parser.add_argument("--empty-budget-ms", type=int, default=100)
    parser.add_argument("--timeout", type=int, default=30)
    parser.add_argument("--start-lead-ms", type=int, default=100)
    parser.add_argument("--socket-buffer-bytes", type=int, default=131072)
    parser.add_argument("--host-label", default="")
    parser.add_argument("--smoke", action="store_true")
    options = parser.parse_args()
    try:
        histories = list(dict.fromkeys(int(value) for value in options.histories.split(",")))
        payloads = list(dict.fromkeys(int(value) for value in options.payload_bytes.split(",")))
        ipc.check(histories and all(0 <= value <= 128 for value in histories), "History must be 0..128")
        ipc.check(payloads and all(value in (64, 256, 1024) for value in payloads), "Invalid payload size")
        ipc.check(options.blocks > 0 and options.control_repetitions > 0 and 1 <= options.messages <= 128,
                  "Positive block/repetition counts and 1..128 messages required")
        ipc.check(options.messages <= options.ring_slots <= 128 and options.ring_slots & (options.ring_slots - 1) == 0,
                  "Power-of-two ring must fit the entire backlog control cohort")
        ipc.check(1 <= options.empty_budget_ms <= options.budget_ms <= 600000 and options.timeout > 0 and options.start_lead_ms > 0,
                  "Invalid data/control budget")
        ipc.check(1024 <= options.socket_buffer_bytes <= (1 << 31) - 1, "Invalid socket buffer")
        ordinary = options.executable.resolve(strict=True)
        diagnostic = options.diagnostic_executable.resolve(strict=True)
        ipc.check(ordinary.is_file() and diagnostic.is_file() and ordinary != diagnostic,
                  "Distinct ordinary and diagnostic executables are required")
        if options.smoke:
            histories, payloads = [0], [64]
            options.blocks = options.control_repetitions = 1
            options.messages, options.ring_slots = 2, 8
        options.repetitions = options.blocks * 2
        options.histories_values, options.payload_values = histories, payloads
        options.interval_ns = 0
    except (OSError, ValueError, RuntimeError) as error:
        parser.error(str(error))
    return options, ordinary, diagnostic


def main():
    options, ordinary, diagnostic = parse_arguments()
    output = options.output.absolute()
    output.mkdir(parents=True) # Existing evidence, including a symlink, is never overwritten.
    manifest = dict(status="running", started_utc=datetime.now(timezone.utc).isoformat(),
                    framework_version=(ipc.ROOT / "VERSION").read_text().strip(), leaves=[])
    def persist():
        (output / "study.json").write_text(json.dumps(manifest, indent=2, sort_keys=True) + "\n")
    def cases(scenario, collection, payloads):
        return [dict(messages=options.messages, history=history, payload_bytes=payload,
                     interval_ns=0, socket_buffer_bytes=options.socket_buffer_bytes,
                     budget_ms=options.empty_budget_ms if scenario == "empty" else options.budget_ms,
                     scenario=scenario, collection=collection, ring_slots=options.ring_slots)
                for history in options.histories_values for payload in payloads]
    persist()
    failed = False
    try:
        plans = [("ordinary-concurrent", ordinary, cases("concurrent", "none", options.payload_values),
                  [balanced_order(block) for block in range(1, options.blocks + 1)], False),
                 ("ordinary-backlog", ordinary, cases("backlog", "none", [64]),
                  [("angaria", "recorded-spsc") if repeat % 2 == 0 else ("recorded-spsc", "angaria")
                   for repeat in range(options.control_repetitions)], False)]
        plans.extend(("diagnostics-" + scenario, diagnostic, cases(scenario, options.diagnostic_collection, [64]),
                      [("angaria",)] * options.control_repetitions, True)
                     for scenario in ("concurrent", "backlog", "empty"))
        for name, executable, leaf_cases, orders, instrumented in plans:
            manifest["active_leaf"] = name
            persist()
            leaf = run_leaf(output, name, executable, leaf_cases, orders, options, instrumented)
            manifest["leaves"].append(leaf)
            persist()
        manifest.update(status="complete", total_attempts=sum(leaf["attempts"] for leaf in manifest["leaves"]))
    except Exception as error:
        failed = True
        manifest.update(status="failed", failure=f"{type(error).__name__}: {error}")
        print(manifest["failure"], file=sys.stderr)
    finally:
        manifest["finished_utc"] = datetime.now(timezone.utc).isoformat()
        persist()
    return 1 if failed else 0


if __name__ == "__main__":
    raise SystemExit(main())
