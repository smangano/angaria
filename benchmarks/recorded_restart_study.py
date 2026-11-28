#!/usr/bin/env python3
"""Retain and reconstruct focused same-process restart costs, without dropping attempts.

These are block means over private endpoints, not independent-process latency,
sustained capacity, latency quantiles or a comparison against nanomsg.
"""

import argparse
from datetime import datetime, timezone
import math
import os
from pathlib import Path
import platform
import shutil
import statistics
import subprocess
import sys
import time
import xml.etree.ElementTree as ET

from producer_log_append_study import (cache_entries, describe, digest, git, load_json,
                                      require, sha256, source_snapshot, write_json)

HISTORIES, SAMPLES, CYCLES = (0, 128, 4096), 5, 64
COSTS = ("baseline_id_send_lookup_receive_ns", "producer_open_ns", "consumer_open_ns",
         "replay_ns_per_record", "recovered_id_send_lookup_receive_ns",
         "following_stream_id_send_lookup_receive_ns")
PREFIXES = ("baseline", "recovered", "following_stream")
FAULTS = tuple(f"{prefix}_{kind}_faults" for prefix in PREFIXES for kind in ("minor", "major"))
TESTS = {("RecordedQueueAllocationCapture", "CountsSuccessfulAllocationRequests"),
         ("RecordedQueuePerformance", "EstablishedDeliveryEmptyPollingAndFullCreditStayAllocationFree"),
         ("RecordedQueuePerformance", "ReplacementReplayAndEstablishedMessagingStayBoundedAcrossRetainedHistory")}


def samples(record, name, *, zero=False, counts=False):
    values = record.get(name)
    require(type(values) is list and len(values) == SAMPLES, f"wrong {name} sample count")
    require(all(type(v) is int and v >= 0 if counts or zero else
                type(v) in (int, float) and math.isfinite(v) and v > 0 for v in values), f"invalid {name}")
    require(not zero or all(v == 0 for v in values), f"nonzero {name}")
    return values


def verify(raw, name, command):
    require(command.get("exit_code") == 0, f"{name}: executable did not exit successfully")
    xml = ET.parse(raw / f"{name}.xml").getroot()
    require(all(int(xml.get(k, -1)) == v for k, v in
                (("tests", 3), ("failures", 0), ("errors", 0), ("disabled", 0))), "unexpected XML totals")
    cases = list(xml.iter("testcase"))
    require(len(cases) == 3 and {(c.get("classname"), c.get("name")) for c in cases} == TESTS and
            all(c.get("status") == "run" and c.get("result") == "completed" and
                c.find("failure") is None and c.find("skipped") is None for c in cases), "incomplete XML cases")
    rows = [load_json(line) for line in (raw / f"{name}.stdout.txt").read_text().splitlines()
            if line.startswith('{"study":')]
    require(all(type(r) is dict for r in rows), "performance rows must be objects")
    replacement = [r for r in rows if r.get("study") == "recorded_queue_replacement_cost"]
    ordinary = [r for r in rows if r.get("study") == "recorded_queue_same_process_cost"]
    require(len(rows) == 4 and len(replacement) == 3 and len(ordinary) == 1, "missing/extra performance rows")
    histories = [r.get("history_before_baseline") for r in replacement]
    require(all(type(h) is int for h in histories) and set(histories) == set(HISTORIES), "duplicate/missing histories")
    for record in rows:
        require(record.get("diagnostics_enabled") is False, "timings require diagnostics OFF")
        require(all(type(record.get(k)) is int and record[k] == v for k, v in
                    (("payload_bytes", 8), ("ring_capacity", 2), ("cycles_per_block", CYCLES))), "wrong fixed workload")
        if record in replacement:
            require(type(record.get("retained_on_replacement")) is int and
                    record["retained_on_replacement"] == record["history_before_baseline"] + CYCLES and
                    type(record.get("following_stream_cycles")) is int and
                    record["following_stream_cycles"] == 4096, "wrong retained/following-stream workload")
            costs, allocations = COSTS, ("replay_allocations", *(f"{p}_allocations" for p in PREFIXES))
            rates = [(f"{p}_verified_publications_per_second", f"{p}_id_send_lookup_receive_ns") for p in PREFIXES]
            for field in FAULTS:
                samples(record, field, counts=True)
        else:
            require(record.get("starting_history_records") == [i * CYCLES for i in range(SAMPLES)], "wrong ordinary histories")
            costs = ("id_send_lookup_receive_ns_per_roundtrip", "empty_receive_ns_per_call", "full_credit_ns_per_attempt")
            allocations = ("roundtrip_allocations", "empty_allocations", "full_allocations")
            rates = [("verified_roundtrips_per_second", costs[0])]
        for field in costs:
            samples(record, field)
        for field in allocations:
            samples(record, field, zero=True)
        for rate, cost in rates:
            value = record.get(rate)
            require(type(value) in (int, float) and math.isfinite(value) and value > 0 and
                    math.isclose(value, 1e9 / statistics.median(record[cost]), rel_tol=5e-5), f"inconsistent {rate}")
    return replacement


def evidence(output):
    return {str(p.relative_to(output)): sha256(p) for folder in ("raw", "provenance")
            for p in sorted((output / folder).rglob("*")) if p.is_file()}


def render(output, validate_only=False):
    manifest = load_json((output / "manifest.json").read_text())
    require(manifest.get("status") == "verified" and manifest.get("source_binary_build_unchanged") is True,
            "study is not verified")
    commands = manifest["commands"]
    require(type(manifest["requested_attempts"]) is int and manifest["requested_attempts"] > 0 and
            len(commands) == manifest["requested_attempts"], "incomplete attempt set")
    frozen = digest({"commit": manifest["source_commit"], "sha256": manifest["source_sha256"]})
    require(manifest["source_digest_before"] == frozen == manifest["source_digest_after"], "source digest mismatch")
    require(evidence(output) == manifest["evidence_sha256"], "retained evidence changed")
    for name, expected in manifest["build_inputs_sha256"].items():
        require(sha256(output / "provenance" / name) == expected, f"build provenance mismatch: {name}")
    rows = []
    for index, command in enumerate(commands, 1):
        name = f"run-{index:02}"
        require(command.get("run") == name and command.get("valid") is True and command.get("errors") == [], "invalid attempt")
        require(load_json((output / "raw" / f"{name}.validation.json").read_text()) == command, "validation sidecar changed")
        rows.extend(verify(output / "raw", name, command))
    summary = []
    for history in HISTORIES:
        selected = [r for r in rows if r["history_before_baseline"] == history]
        entry = {"history_before_baseline": history, "retained_on_replacement": history + CYCLES,
                 "attempts": len(commands), "block_samples": len(selected) * SAMPLES}
        for metric in (*COSTS, *FAULTS):
            values = [v for r in selected for v in r[metric]]
            entry[metric] = {"median": statistics.median(values), "min": min(values), "max": max(values)}
        for prefix in PREFIXES:
            entry[f"{prefix}_verified_publications_per_second"] = 1e9 / entry[f"{prefix}_id_send_lookup_receive_ns"]["median"]
        summary.append(entry)
    if not validate_only:
        write_json(output / "summary.json", summary)
    print(f"Verified {len(commands)} attempts; {len(commands) * SAMPLES} block means per history, none discarded.")


def collect(options):
    root, binary, output = options.source_root.resolve(), options.binary.resolve(), options.output.resolve()
    require(options.attempts > 0 and options.timeout > 0, "attempt count and timeout must be positive")
    require(not output.exists(), "output already exists; retain it and choose another directory")
    before = source_snapshot(root)
    require(before["commit"] == git(root, "rev-parse", options.source_commit + "^{commit}").decode().strip(), "HEAD differs from pinned source")
    scripts = [Path(__file__).resolve(), Path(__file__).with_name("producer_log_append_study.py").resolve()]
    require(subprocess.run(["git", "diff", "--quiet", "HEAD", "--", *map(str, scripts)], cwd=root).returncode == 0 and
            all(git(root, "ls-files", "--error-unmatch", str(p)).strip() for p in scripts), "commit collectors before collection")
    build = (options.build_dir or binary.parent).resolve()
    entries = cache_entries(build / "CMakeCache.txt")
    require(entries.get("CMAKE_BUILD_TYPE", {}).get("value") == "Release", "requires Release")
    require(all(entries.get(k, {}).get("value", "").upper() in ("OFF", "FALSE", "NO", "0") for k in
                ("ANGARIA_ENABLE_DIAGNOSTICS", "ANGARIA_ENABLE_CACHEGRIND")), "requires diagnostics/Cachegrind explicitly OFF")
    inputs = [binary, build / "libangaria.a", *scripts, build / "CMakeCache.txt"]
    inputs += [p for p in (build / "build.ninja", build / "CMakeFiles/rules.ninja",
               *(build / "CMakeFiles" / (target + ".dir") / name for target in
                 ("angaria", "angaria_recorded_queue_performance") for name in ("flags.make", "link.txt"))) if p.is_file()]
    frozen = {str(p): sha256(p) for p in inputs}
    raw, provenance = output / "raw", output / "provenance"
    raw.mkdir(parents=True)
    provenance.mkdir()
    build_hashes = {}
    for path in inputs[4:]:
        name = str(path.relative_to(build))
        target = provenance / name
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(path, target)
        build_hashes[name] = sha256(path)
    manifest = {"schema_version": 1, "status": "collecting", "source_commit": before["commit"],
                "framework_version": (root / "VERSION").read_text().strip(), "source_sha256": before["sha256"],
                "source_digest_before": digest(before), "frozen_inputs_sha256": frozen,
                "build_inputs_sha256": build_hashes, "cmake_cache_entries": entries,
                "compiler": describe([entries["CMAKE_CXX_COMPILER"]["value"], "--version"]),
                "binary_architecture": describe(["file", str(binary)]), "host": dict(platform.uname()._asdict()),
                "hardware_optional": describe(["/usr/sbin/sysctl", "-n", "machdep.cpu.brand_string"])
                                     if platform.system() == "Darwin" else describe(["lscpu"]),
                "logical_cpu_count": os.cpu_count(), "coordinator_python": sys.version,
                "requested_attempts": options.attempts, "commands": [],
                "scope": "Same-process block means; no affinity, tails, nanomsg or sustained IPC capacity claim"}
    write_json(output / "manifest.json", manifest)
    for index in range(1, options.attempts + 1):
        name = f"run-{index:02}"
        argv = [str(binary), "--gtest_color=no", f"--gtest_output=xml:{raw / (name + '.xml')}"]
        command = {"run": name, "argv": argv, "started_utc": datetime.now(timezone.utc).isoformat(), "errors": []}
        start = time.monotonic()
        try:
            result = subprocess.run(argv, cwd=root, capture_output=True, timeout=options.timeout)
            stdout, stderr, command["exit_code"] = result.stdout, result.stderr, result.returncode
        except (OSError, subprocess.TimeoutExpired) as error:
            stdout, stderr, command["exit_code"] = getattr(error, "stdout", None) or b"", getattr(error, "stderr", None) or b"", None
            command["errors"].append(str(error))
        command["elapsed_seconds"] = time.monotonic() - start
        (raw / f"{name}.stdout.txt").write_bytes(stdout)
        (raw / f"{name}.stderr.txt").write_bytes(stderr)
        try:
            verify(raw, name, command)
        except (ValueError, TypeError, KeyError, OSError, ET.ParseError) as error:
            command["errors"].append(str(error))
        command["valid"] = not command["errors"]
        write_json(raw / f"{name}.validation.json", command)
        manifest["commands"].append(command)
        write_json(output / "manifest.json", manifest)
        print(f"{name}: {'verified' if command['valid'] else 'INVALID (retained)'}", flush=True)
    try:
        after = source_snapshot(root)
        manifest["source_digest_after"] = digest(after)
        require(after == before and all(sha256(Path(p)) == value for p, value in frozen.items()), "frozen source/build inputs changed")
        manifest["source_binary_build_unchanged"] = True
    except (ValueError, OSError, subprocess.CalledProcessError) as error:
        manifest.update(source_binary_build_unchanged=False, errors=[str(error)])
    manifest["evidence_sha256"] = evidence(output)
    manifest["status"] = "verified" if manifest["source_binary_build_unchanged"] and all(c["valid"] for c in manifest["commands"]) else "invalid"
    write_json(output / "manifest.json", manifest)
    render(output)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    collect_parser = commands.add_parser("collect")
    collect_parser.add_argument("binary", type=Path)
    collect_parser.add_argument("output", type=Path)
    collect_parser.add_argument("--source-commit", required=True)
    collect_parser.add_argument("--source-root", type=Path, default=Path(__file__).resolve().parents[1])
    collect_parser.add_argument("--build-dir", type=Path)
    collect_parser.add_argument("--attempts", type=int, default=3)
    collect_parser.add_argument("--timeout", type=float, default=60)
    render_parser = commands.add_parser("render")
    render_parser.add_argument("output", type=Path)
    render_parser.add_argument("--validate-only", action="store_true")
    args = parser.parse_args()
    collect(args) if args.command == "collect" else render(args.output.resolve(), args.validate_only)
