#!/usr/bin/env python3
"""Qualify the optional IPC tool's evidence and cleanup, without speed limits.

The real cohorts exercise both independent processes. Deliberately corrupted
reports below test the coordinator's acceptance rules, not public Angaria APIs.
"""
import copy
import csv
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import time

sys.dont_write_bytecode = True
ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "benchmarks"))
import run_ipc_comparison as ipc


def require(condition, message):
    if not condition:
        raise RuntimeError(message)


def rejected(operation):
    try:
        operation()
    except RuntimeError:
        return
    raise RuntimeError("Corrupted evidence was accepted")


def independent_hash(row, size):
    # Build the golden wire independently: little-endian fields, then body.
    data = bytearray(b"\x01\x00\x01\x00\x00\x00\x00\x00")
    for field in ("sequence", "scheduled_ns", "logical_start_ns"):
        data.extend(row[field].to_bytes(8, "little"))
    data.extend((row["sequence"] + index) % 256 for index in range(32, size))
    value = 14695981039346656037
    for byte in data:
        value = (value ^ byte) * 1099511628211 % (1 << 64)
    return "%016x" % value


def run_cohort(executable, output, arguments, messages, history, payload, interval):
    result = subprocess.run([sys.executable, "-B", str(ROOT / "benchmarks/run_ipc_comparison.py"),
                             str(executable), "--output", str(output), "--budget-ms", "3000",
                             "--timeout", "10", "--start-lead-ms", "20"] + arguments,
                            capture_output=True, text=True, timeout=45)
    require(result.returncode == 0, "Cohort failed; retained at %s\n%s\n%s" % (output, result.stdout, result.stderr))
    reports = [json.loads(line) for line in (output / "reports.jsonl").read_text().splitlines()]
    require([row["profile"] for row in reports] == ["angaria", "nanomsg", "nanomsg", "angaria"], "ABBA order changed")
    require(all(row["status"] == "complete" for row in reports), "Incomplete smoke population")
    manifest = (output / "manifest.toml").read_text()
    require('"completed_runs" = 4' in manifest and '"failed_runs" = 0' in manifest and '"repetitions" = 2' in manifest,
            "Manifest population differs")
    require('"executable_sha256" = ' + json.dumps(hashlib.sha256(executable.read_bytes()).hexdigest()) in manifest,
            "Missing exact executable hash")
    # Check actual pinned metadata rather than trusting only a syntactically valid hash.
    for path in ("CMakeLists.txt", "VERSION", "benchmarks/ipc_comparison.cpp",
                 "benchmarks/ipc_comparison_support.h", "benchmarks/run_ipc_comparison.py", "test/ipc_comparison.py"):
        digest = hashlib.sha256((ROOT / path).read_bytes()).hexdigest()
        require(json.dumps(path) + " = " + json.dumps(digest) in manifest, "Missing exact source hash: " + path)
    pins = {"nanomsg_version": "1.3.0", "nanomsg_commit": "73e89d2ca28d11cbd39bb15e8d2e1edeb7eec68a",
            "nanomsg_archive_sha256": "acf65c0ef312f431aa3c4cb114326781c999ec0c977067f3a1f0c81b5ec8710f"}
    for report in reports:
        require((report["messages"], report["history"], report["payload_bytes"], report["interval_ns"]) ==
                (messages, history, payload, interval), "Requested dimensions changed")
        require((report["accepted"], report["delivered"], report["pending"]) == (messages, messages, 0), "Matched counts changed")
        require(report["acknowledged"] == (messages if report["profile"] == "angaria" else 0), "Profile acknowledgment boundary changed")
        producer, consumer = (report["worker_reports"][role] for role in ipc.ROLES)
        pids = {producer["pid"], consumer["pid"], report["setup"]["pid"]}
        require(len(pids) == 3 and os.getpid() not in pids, "Roles did not run in independent processes")
        require(report["clock_domain_validated"] and len(report["clock_probes"]) == 6, "Missing shared-clock crosscheck")
        for probe in report["clock_probes"]:
            require(probe["before_ns"] <= probe["worker_ns"] <= probe["received_ns"], "Clock probe outside coordinator bracket")
        for worker in (report["setup"], producer, consumer):
            require(all(worker[key] == value for key, value in pins.items()), "Wrong selected libnanomsg source")
            require(worker["framework_version"] == (ROOT / "VERSION").read_text().strip(), "Wrong framework version")
        t0 = report["control_timing"]["t0_ns"]
        for worker in (producer, consumer):
            require(worker["clock_pair_samples"] == 256 and worker["clock_pair_median_ns"] >= worker["clock_pair_min_ns"] >= 0,
                    "Missing independent clock overhead observation")
            require(worker["cpu_start_time_ns"] <= worker["data_start_ns"] <= worker["resource_end_ns"], "CPU interval changed")
            for index, row in enumerate(worker["rows"]):
                require(row["sequence"] == history + index + 1 and row["scheduled_ns"] == t0 + index * interval, "Identity/schedule changed")
                require(row["payload_hash"] == independent_hash(row, payload), "Wrong exact wire payload")
        resources = report["resources"]
        require(resources["complete_system_phase_cpu_ns"] == resources["coordinator_phase_cpu_ns"] +
                sum(resources["worker_process_phase_cpu_ns"].values()), "Participant CPU accounting omitted a role")
        directory = output / report["artifact_directory"]
        for role in ("setup", "producer", "consumer"):
            require((directory / (role + ".stdout")).is_file() and (directory / (role + ".stderr")).is_file(), "Raw stream missing")
        require(report["rates"]["bounded_cohort_verified_per_second"] == messages * 1e9 / report["cohort_verified_ns"], "Wrong verified denominator")
    with (output / "runs.csv").open(newline="") as stream:
        require(len(list(csv.DictReader(stream))) == 4, "CSV dropped completed attempts")
    with (output / "summary.csv").open(newline="") as stream:
        summary = list(csv.DictReader(stream))
    require(summary and all(row["runs"] == "2" for row in summary), "Summary merged unlike cells")
    require(all(row["pooled_p99"] == row["pooled_p999"] == ("insufficient_samples" if row["unit"] == "ns" else "not_applicable")
                for row in summary), "Short cohorts invented a tail estimate")
    require(not any(row["profile"] == "nanomsg" and row["metric"] == "acknowledge_api_ns" for row in summary), "PUSH gained a fabricated acknowledgment")
    return reports


def tamper_checks(reports):
    for original in reports[:2]:
        profile, t0 = original["profile"], original["control_timing"]["t0_ns"]
        case = {key: original[key] for key in ipc.DIMENSIONS}
        producer, consumer = (original["worker_reports"][role] for role in ipc.ROLES)
        def damaged(role, field, value):
            pair = copy.deepcopy((producer, consumer))
            pair[role]["rows"][0][field] = value
            rejected(lambda: ipc.validate_rows(*pair, profile, case, t0))
        damaged(0, "payload_hash", "0000000000000000")
        damaged(1, "sequence", case["history"] + case["messages"] + 1)
        damaged(0, "scheduled_ns", producer["rows"][0]["scheduled_ns"] + 1)
        damaged(1, "receive_ns", consumer["rows"][0]["logical_start_ns"] - 1)
        damaged(1, "ack_start_ns", consumer["rows"][0]["verified_ns"] - 1 if profile == "angaria" else 1)
        for key, value in (("framework_version", "wrong"), ("nanomsg_version", "1.2.3"), ("pid", 0),
                           ("nanomsg_runtime_abi_current", producer["nanomsg_header_abi_current"] + 1),
                           ("clock_pair_median_ns", float("nan"))):
            report = dict(producer, **{key: value})
            rejected(lambda: ipc.common_report(report, "producer", profile, case, (ROOT / "VERSION").read_text().strip()))
        # A verified prefix followed by an in-flight row remains incomplete evidence.
        pair = copy.deepcopy((producer, consumer))
        pair[0]["status"] = pair[1]["status"] = "budget_exceeded"
        pair[1]["rows"][-1]["verified_ns"] = 0
        pair[1]["verified"] -= 1
        pair[1]["completed"] -= 1
        if profile == "angaria":
            pair[1]["acknowledged"] -= 1
        prefix = ipc.validate_rows(*pair, profile, case, t0)
        require(prefix["status"] == "budget_exceeded" and prefix["delivered"] == case["messages"] - 1 and prefix["pending"] == 1,
                "Incomplete row was counted as delivered")


def cleanup_check(directory):
    directory.mkdir()
    workers = ipc.Workers(directory, time.monotonic() + 3)
    script = ('import json,sys,time; print(json.dumps(dict(event="CONNECTED",role="producer",profile="angaria")),flush=True); '
              'sys.stderr.write("retained-error\\n"); sys.stderr.flush(); sys.stdout.write("partial-tail"); sys.stdout.flush(); time.sleep(30)')
    try:
        workers.start("producer", [sys.executable, "-u", "-c", script])
        process = workers.workers["producer"]["process"]
        workers.events({"producer": "CONNECTED"}, "angaria")
        workers.deadline = time.monotonic() + 0.1
        rejected(lambda: workers.events({"producer": "DONE"}, "angaria"))
    finally:
        workers.close()
    require(process.poll() is not None, "Timed-out participant survived cleanup")
    try:
        os.waitpid(process.pid, os.WNOHANG)
    except ChildProcessError:
        pass
    else:
        raise RuntimeError("Participant was not reaped")
    require(b"partial-tail" in (directory / "producer.stdout").read_bytes() and
            b"retained-error" in (directory / "producer.stderr").read_bytes(), "Timeout discarded raw evidence")


def main():
    executable = Path(sys.argv[1]).resolve(strict=True)
    output = Path(tempfile.mkdtemp(prefix="angaria-ipc-test-"))
    try:
        reports = run_cohort(executable, output / "smoke", ["--smoke"], 2, 0, 64, 0)
        run_cohort(executable, output / "history", ["--blocks", "1", "--messages", "3", "--histories", "2",
                                                  "--payload-bytes", "1024", "--interval-ns", "1000000"], 3, 2, 1024, 1000000)
        tamper_checks(reports)
        cleanup_check(output / "timeout")
        for index, arguments in enumerate((["--blocks", "0"], ["--messages", "129"],
                                            ["--messages", "2", "--interval-ns", "1000000", "--budget-ms", "1"])):
            destination = output / ("invalid-%d" % index)
            result = subprocess.run([sys.executable, "-B", str(ROOT / "benchmarks/run_ipc_comparison.py"), str(executable),
                                     "--output", str(destination)] + arguments, capture_output=True, timeout=5)
            require(result.returncode != 0 and not destination.exists(), "Invalid matrix created results")
        marker = output / "smoke" / "keep.txt"
        marker.write_text("preserved")
        result = subprocess.run([sys.executable, "-B", str(ROOT / "benchmarks/run_ipc_comparison.py"), str(executable),
                                 "--output", str(output / "smoke"), "--smoke"], capture_output=True, timeout=5)
        require(result.returncode != 0 and marker.read_text() == "preserved", "Existing evidence was overwritten")
    except Exception:
        print("Retained qualification evidence: " + str(output), file=sys.stderr)
        raise
    shutil.rmtree(output)
    print("IPC comparison: eight real cohorts, metadata/wire/tail validation, corruption rejection, and timeout cleanup passed")


if __name__ == "__main__":
    main()
