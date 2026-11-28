#!/usr/bin/env python3
"""Collect or reconstruct the private producer-owned append/replay cost study.

Collection requires a frozen implementation commit and a diagnostics-OFF Release
build. Every attempted process, its original output, XML, parsed reports and
validation errors are retained. An invalid attempt invalidates the study rather
than disappearing from its plots. Wall times are component block means, not
transport latency, delivered-message throughput, tail quantiles or speedups.
"""

import argparse
import csv
from datetime import datetime, timezone
import hashlib
import json
import math
import os
from pathlib import Path
import platform
import shutil
import statistics
import subprocess
import sys
import tempfile
import time
import xml.etree.ElementTree as ET

ATTEMPTS, SAMPLES, CYCLES = 10, 5, 64
HISTORIES = (0, 128, 1024)
PAYLOADS = (64, 256, 1024)
PREFIX = "CODEC_APPEND_PERFORMANCE "
CASES = {(history, payload) for history in HISTORIES for payload in PAYLOADS}
ZERO_COUNTERS = ("native_read_calls", "native_map_calls", "native_unmap_calls", "file_stat_calls",
                 "file_grow_calls", "gate_attempts", "gate_acquisitions")
METRICS = ("prepared_append_ns", "prepared_appends_per_second", "capture_ns", "replay_ns_per_record",
           "replay_records_per_second", "writer_recovery_open_ns", "reader_open_with_counter_capture_ns")


def require(condition, message):
    if not condition:
        raise ValueError(message)


def unique_object(pairs):
    result = {}
    for key, value in pairs:
        require(key not in result, f"duplicate JSON key: {key}")
        result[key] = value
    return result


def reject_nonfinite(value):
    raise ValueError(f"non-finite JSON number: {value}")


def finite_json_float(value):
    result = float(value)
    require(math.isfinite(result), f"non-finite JSON number: {value}")
    return result


def load_json(text):
    return json.loads(text, object_pairs_hook=unique_object, parse_constant=reject_nonfinite,
                      parse_float=finite_json_float)


def write_json(path, value):
    path.write_text(json.dumps(value, indent=2, sort_keys=True, allow_nan=False) + "\n", encoding="utf-8")


def sha256(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def digest(value):
    return hashlib.sha256(json.dumps(value, sort_keys=True, separators=(",", ":")).encode()).hexdigest()


def describe(argv, cwd=None):
    try:
        result = subprocess.run(argv, cwd=cwd, capture_output=True, text=True, timeout=30)
        return {"argv": [str(arg) for arg in argv], "exit_code": result.returncode,
                "stdout": result.stdout.strip(), "stderr": result.stderr.strip()}
    except (OSError, subprocess.TimeoutExpired) as error:
        return {"argv": [str(arg) for arg in argv], "exit_code": None, "error": str(error)}


def git(root, *arguments):
    return subprocess.check_output(["git", *arguments], cwd=root)


def implementation_input(name):
    path = Path(name)
    return (path.suffix.lower() in (".cpp", ".cc", ".cxx", ".h", ".hh", ".hpp", ".hxx", ".ipp", ".tpp", ".cmake", ".in") or
            path.name == "CMakeLists.txt" or name == "VERSION")


def source_snapshot(root):
    names = [name for name in git(root, "ls-files", "-z").decode().split("\0")
             if name and implementation_input(name)]
    untracked = [name for name in git(root, "ls-files", "--others", "--exclude-standard", "-z").decode().split("\0")
                 if name and implementation_input(name)]
    require(not untracked, f"untracked implementation inputs must be frozen first: {untracked}")
    changed = subprocess.run(["git", "diff", "--quiet", "HEAD", "--", *names], cwd=root).returncode
    require(changed == 0, "implementation inputs differ from HEAD; commit the frozen source before collection")
    return {"commit": git(root, "rev-parse", "HEAD").decode().strip(),
            "sha256": {name: sha256(root / name) for name in sorted(names)}}


def cache_entries(cache):
    entries = {}
    for line in cache.read_text(encoding="utf-8").splitlines():
        if line and not line.startswith(("#", "//")) and ":" in line and "=" in line:
            key, value = line.split("=", 1)
            name, kind = key.split(":", 1)
            entries[name] = {"type": kind, "value": value}
    return entries


def positive(value, name):
    require(type(value) in (int, float) and math.isfinite(value) and value > 0,
            f"{name}: expected a finite positive number")
    return value


def verify_record(record):
    case = record.get("initial_history_records"), record.get("payload_bytes")
    require(all(type(value) is int for value in case) and case in CASES, "unexpected history/payload case")
    history, _ = case
    for name, expected in (("samples", SAMPLES), ("cycles_per_sample", CYCLES),
                           ("reserved_file_bytes", 8 * 1024 * 1024), ("publication_capacity", 2048),
                           ("capture_new_records_per_sample", CYCLES), ("diagnostic_append_records", 32),
                           ("diagnostic_capture_replay_records", history + SAMPLES * CYCLES + 32)):
        require(type(record.get(name)) is int and record[name] == expected, f"{case}: wrong {name}")
    require(record.get("instrumentation_enabled") is False, f"{case}: expected diagnostics-OFF timing")
    require(record.get("append_block_start_records") == [history + CYCLES * index for index in range(SAMPLES)],
            f"{case}: wrong actual append history")
    require(record.get("replay_record_counts") == [history + CYCLES * (index + 1) for index in range(SAMPLES)],
            f"{case}: wrong replay denominators")
    for name in ("append_allocations_per_block", "capture_allocations_per_block", "replay_allocations_per_block"):
        values = record.get(name)
        require(type(values) is list and len(values) == SAMPLES and
                all(type(value) is int and value == 0 for value in values), f"{case}: nonzero/invalid {name}")
    for name in ("diagnostic_append_counters", "diagnostic_capture_replay_counters"):
        counters = record.get(name)
        require(type(counters) is dict and set(ZERO_COUNTERS) <= counters.keys(), f"{case}: missing {name}")
        require(all(type(value) is int and value == 0 for value in counters.values()),
                f"{case}: diagnostics-OFF report contains invalid/nonzero counters")
    medians = {}
    for name in ("prepared_append_ns_samples", "capture_ns_samples", "replay_ns_per_record_samples"):
        values = record.get(name)
        require(type(values) is list and len(values) == SAMPLES, f"{case}: wrong {name} count")
        medians[name] = statistics.median([positive(value, f"{case}/{name}") for value in values])
    append_ns = medians["prepared_append_ns_samples"]
    replay_ns = medians["replay_ns_per_record_samples"]
    for name, expected in (("prepared_append_median_ns", append_ns), ("replay_median_ns_per_record", replay_ns)):
        require(abs(positive(record.get(name), name) - expected) <= 0.0015, f"{case}: stale {name}")
    for name, expected in (("prepared_appends_per_second", 1e9 / append_ns),
                           ("replay_records_per_second", 1e9 / replay_ns)):
        require(math.isclose(positive(record.get(name), name), expected, rel_tol=5e-5, abs_tol=0.0015),
                f"{case}: stale {name} beyond printed-sample rounding")
    return {"initial_history_records": history, "payload_bytes": case[1],
            "prepared_append_ns": append_ns, "prepared_appends_per_second": 1e9 / append_ns,
            "capture_ns": medians["capture_ns_samples"], "replay_ns_per_record": replay_ns,
            "replay_records_per_second": 1e9 / replay_ns,
            "writer_recovery_open_ns": positive(record.get("writer_recovery_open_ns"), "writer recovery open"),
            "reader_open_with_counter_capture_ns": positive(record.get("reader_open_with_counter_capture_ns"),
                                                            "reader open including counter capture")}


def verify_records(records):
    require(len(records) == len(CASES), f"expected nine performance records, found {len(records)}")
    keys = [(record.get("initial_history_records"), record.get("payload_bytes")) for record in records]
    require(len(set(keys)) == len(CASES) and set(keys) == CASES, "duplicate or missing history/payload case")
    return [verify_record(record) for record in records]


def verify_xml(path):
    root = ET.parse(path).getroot()
    for name, expected in (("tests", 10), ("failures", 0), ("errors", 0), ("disabled", 0)):
        require(int(root.attrib.get(name, -1)) == expected, f"XML {name}: expected {expected}")
    cases = list(root.iter("testcase"))
    require(len(cases) == 10, "XML omitted test results")
    require(all(case.get("status") == "run" and case.get("result") == "completed" and
                case.find("failure") is None and case.find("skipped") is None for case in cases),
            "XML includes an incomplete, failed or skipped test")


def evidence_hashes(output):
    return {str(path.relative_to(output)): sha256(path)
            for directory in (output / "raw", output / "provenance")
            for path in sorted(directory.iterdir()) if path.is_file()}


def collect(options):
    root = options.source_root.resolve()
    binary = options.binary.resolve()
    output = options.output.resolve()
    build = (options.build_dir or binary.parent).resolve()
    require(binary.is_file(), f"binary not found: {binary}")
    require(not output.exists() or not any(output.iterdir()), "output is not empty; preserve it and choose another directory")
    pinned = git(root, "rev-parse", options.source_commit + "^{commit}").decode().strip()
    before = source_snapshot(root)
    require(before["commit"] == pinned, "HEAD does not match the requested frozen source commit")
    cache = build / "CMakeCache.txt"
    entries = cache_entries(cache)
    require(entries.get("CMAKE_BUILD_TYPE", {}).get("value") == "Release", "study requires a Release build")
    require(entries.get("ANGARIA_ENABLE_DIAGNOSTICS", {}).get("value", "").upper() in ("OFF", "FALSE", "NO", "0"),
            "study requires an explicitly diagnostics-OFF CMake cache")
    compiler = entries.get("CMAKE_CXX_COMPILER", {}).get("value")
    require(compiler, "CMake cache does not identify the compiler")
    raw, provenance = output / "raw", output / "provenance"
    raw.mkdir(parents=True)
    provenance.mkdir()
    recorded_build_inputs = {"CMakeCache.txt": cache}
    for name in ("flags.make", "link.txt"):
        path = build / "CMakeFiles" / "angaria_producer_log_append_performance.dir" / name
        if path.is_file():
            recorded_build_inputs[name] = path
    for name, path in recorded_build_inputs.items():
        shutil.copyfile(path, provenance / name)
    build_hashes = {name: sha256(path) for name, path in recorded_build_inputs.items()}
    library = build / "libangaria.a"
    binary_hash = sha256(binary)
    collector_hash = sha256(Path(__file__))
    library_hash = sha256(library) if library.is_file() else None
    manifest = {
        "schema_version": 1, "status": "collecting", "source_commit": pinned,
        "framework_version": (root / "VERSION").read_text().strip(), "source_sha256": before["sha256"],
        "source_tree_digest_before": digest(before), "binary_sha256": binary_hash, "library_sha256": library_hash,
        "collector_sha256": collector_hash, "build_inputs_sha256": build_hashes,
        "cmake_cache_entries": {name: value for name, value in entries.items()
                                if name.startswith(("CMAKE_CXX_", "CMAKE_EXE_LINKER_", "CMAKE_OSX_")) or
                                name in ("CMAKE_BUILD_TYPE", "ANGARIA_ENABLE_DIAGNOSTICS", "BUILD_TESTING")},
        "compiler": describe([compiler, "--version"]), "binary_architecture": describe(["file", str(binary)]),
        "hardware_optional": describe(["/usr/sbin/sysctl", "-n", "machdep.cpu.brand_string"])
                             if platform.system() == "Darwin" else describe(["lscpu"]),
        "host": dict(platform.uname()._asdict()), "logical_cpu_count": os.cpu_count(),
        "coordinator_python": sys.version, "coordinator_architecture": platform.machine(),
        "requested_attempts": ATTEMPTS, "histories": list(HISTORIES), "payloads": list(PAYLOADS),
        "blocks_per_case": SAMPLES, "appends_per_block": CYCLES,
        "scope": "Prepared local ID+append, incremental capture, borrowed full-cut replay, and fresh-handle recovery/open",
        "baseline": "First measured baseline for this private prepared-storage component; no old-runtime or transport speedup claim",
        "clock": "std::chrono::steady_clock; per-block means and per-attempt medians, no per-message quantiles",
        "timing_contract": "ID issuance plus append; schema/topology/payload already prepared; caller supplies a constant timestamp",
        "history_contract": "First append block starts at H; subsequent blocks start H+64*n. Replay cuts hold H+64*(n+1) records",
        "loop_work": "Result checks, append-result storage, and replay ordinal/payload-byte checksum work are included",
        "replay_verification": "Timed iteration consumes one payload byte per record; complete headers/destinations/payloads are verified afterward",
        "warmup": "16 appends and replay on a separate log; no undisclosed warmup records in the measured log",
        "allocation_hooks": "Successful C++ allocation counter enabled during every timing block; successful paths assert zero",
        "diagnostics": "OFF for timings; required zero counters are qualified separately by diagnostics-ON CI, not by these zeroed reports",
        "recovery_contract": "Fresh writer/reader handles over a retained prefix with warm OS cache; not disk-cold or power-loss recovery",
        "execution_context": "Sequential attempts on the current host with ordinary execution; no CPU affinity requested",
        "commands": [], "evidence_sha256": {},
    }
    write_json(output / "manifest.json", manifest)
    for attempt in range(1, ATTEMPTS + 1):
        name = f"run-{attempt:02}"
        xml = raw / (name + ".xml")
        argv = [str(binary), "--gtest_color=no", f"--gtest_output=xml:{xml}"]
        command = {"run": name, "argv": argv, "started_utc": datetime.now(timezone.utc).isoformat()}
        errors = []
        start = time.monotonic()
        try:
            result = subprocess.run(argv, cwd=root, capture_output=True, timeout=options.timeout)
            stdout, stderr = result.stdout, result.stderr
            command["exit_code"] = result.returncode
            if result.returncode != 0:
                errors.append(f"executable exit status {result.returncode}")
        except subprocess.TimeoutExpired as error:
            stdout, stderr = error.stdout or b"", error.stderr or b""
            command["exit_code"] = None
            errors.append(f"executable timed out after {options.timeout} seconds")
        except OSError as error:
            stdout, stderr = b"", b""
            command["exit_code"] = None
            errors.append(f"execution failed: {error}")
        command["elapsed_seconds"] = time.monotonic() - start
        (raw / (name + ".stdout.txt")).write_bytes(stdout)
        (raw / (name + ".stderr.txt")).write_bytes(stderr)
        records = []
        try:
            for line in stdout.decode("utf-8").splitlines():
                if line.startswith(PREFIX):
                    try:
                        record = load_json(line[len(PREFIX):])
                        require(type(record) is dict, "performance report must be a JSON object")
                        records.append(record)
                    except (ValueError, TypeError) as error:
                        errors.append(f"malformed report: {error}")
        except UnicodeError as error:
            errors.append(f"stdout decoding failed: {error}")
        (raw / (name + ".jsonl")).write_text("".join(json.dumps(record, sort_keys=True, allow_nan=False) + "\n"
                                                   for record in records), encoding="utf-8")
        try:
            verify_records(records)
        except (ValueError, TypeError, KeyError) as error:
            errors.append(f"performance report validation failed: {error}")
        try:
            verify_xml(xml)
        except (ValueError, OSError, ET.ParseError) as error:
            errors.append(f"XML validation failed: {error}")
        command.update({"parsed_cases": len(records), "valid": not errors, "errors": errors})
        write_json(raw / (name + ".validation.json"), command)
        manifest["commands"].append(command)
        manifest["evidence_sha256"] = evidence_hashes(output)
        write_json(output / "manifest.json", manifest)
        print(f"{name}: {'verified' if not errors else 'INVALID (retained)'}", flush=True)
    errors = []
    try:
        after = source_snapshot(root)
        manifest["source_tree_digest_after"] = digest(after)
        require(after == before, "implementation source changed during the study")
        require(sha256(binary) == binary_hash, "binary changed during the study")
        require(sha256(Path(__file__)) == collector_hash, "collector changed during the study")
        require(not library_hash or sha256(library) == library_hash, "library changed during the study")
        require({name: sha256(path) for name, path in recorded_build_inputs.items()} == build_hashes,
                "build configuration changed during the study")
        manifest["source_binary_build_unchanged"] = True
    except (ValueError, OSError, subprocess.CalledProcessError) as error:
        manifest["source_binary_build_unchanged"] = False
        errors.append(str(error))
    manifest["status"] = "verified" if not errors and all(item["valid"] for item in manifest["commands"]) else "invalid"
    manifest["errors"] = errors
    write_json(output / "manifest.json", manifest)
    require(manifest["status"] == "verified", "study invalid; all attempted evidence and errors were retained")
    render(output, validate_only=options.no_plots)


def reconstruct(output):
    manifest = load_json((output / "manifest.json").read_text())
    require(manifest.get("status") == "verified" and manifest.get("source_binary_build_unchanged") is True,
            "manifest does not describe a verified, unchanged-source study")
    require(manifest.get("requested_attempts") == ATTEMPTS and len(manifest.get("commands", [])) == ATTEMPTS,
            "manifest has an incomplete attempt set")
    require(evidence_hashes(output) == manifest.get("evidence_sha256"), "retained evidence file set or contents changed")
    source_digest = digest({"commit": manifest["source_commit"], "sha256": manifest["source_sha256"]})
    require(manifest.get("source_tree_digest_before") == source_digest and
            manifest.get("source_tree_digest_after") == source_digest, "source provenance digest mismatch")
    rows = []
    for attempt, command in enumerate(manifest["commands"], 1):
        name = f"run-{attempt:02}"
        require(command.get("run") == name and command.get("valid") is True and command.get("exit_code") == 0,
                f"{name}: invalid or reordered attempt")
        path = output / "raw" / (name + ".jsonl")
        records = [load_json(line) for line in path.read_text().splitlines() if line.strip()]
        original_records = [load_json(line[len(PREFIX):]) for line in
                            (output / "raw" / (name + ".stdout.txt")).read_text().splitlines()
                            if line.startswith(PREFIX)]
        require(records == original_records, f"{name}: parsed records differ from original stdout")
        require(load_json((output / "raw" / (name + ".validation.json")).read_text()) == command,
                f"{name}: validation sidecar differs from manifest")
        verify_xml(output / "raw" / (name + ".xml"))
        for row in verify_records(records):
            rows.append({"attempt": attempt, **row})
    require(len(rows) == ATTEMPTS * len(CASES), "incomplete reconstructed case set")
    summary = []
    for history in HISTORIES:
        for payload in PAYLOADS:
            selected = [row for row in rows if row["initial_history_records"] == history and row["payload_bytes"] == payload]
            entry = {"initial_history_records": history, "payload_bytes": payload, "attempts": ATTEMPTS,
                     "blocks_per_attempt": SAMPLES, "appends_per_block": CYCLES}
            for metric in METRICS:
                values = [row[metric] for row in selected]
                entry.update({"median_" + metric: statistics.median(values),
                              "min_" + metric: min(values), "max_" + metric: max(values)})
            summary.append(entry)
    return rows, summary


def write_csv(path, rows):
    with path.open("w", newline="", encoding="utf-8") as stream:
        writer = csv.DictWriter(stream, fieldnames=list(rows[0]), lineterminator="\n")
        writer.writeheader()
        writer.writerows({key: f"{value:.6f}" if isinstance(value, float) else value for key, value in row.items()}
                         for row in rows)


def plot(output, attempts, summary):
    os.environ.setdefault("MPLCONFIGDIR", str(Path(tempfile.gettempdir()) / "angaria-matplotlib"))
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt
    plt.rcParams.update({"font.size": 10, "svg.hashsalt": "angaria-producer-owned-append"})
    colors = ("#2166ac", "#b35806", "#32834d")

    def panel(axis, metric, scale, title, ylabel):
        for payload_index, (payload, color) in enumerate(zip(PAYLOADS, colors)):
            xs, medians, lows, highs = [], [], [], []
            for history_index, history in enumerate(HISTORIES):
                x = history_index + (payload_index - 1) * 0.19
                item = next(row for row in summary if row["initial_history_records"] == history and row["payload_bytes"] == payload)
                values = [row[metric] / scale for row in attempts
                          if row["initial_history_records"] == history and row["payload_bytes"] == payload]
                xs.append(x)
                medians.append(item["median_" + metric] / scale)
                lows.append((item["median_" + metric] - item["min_" + metric]) / scale)
                highs.append((item["max_" + metric] - item["median_" + metric]) / scale)
                jitters = [(index - (ATTEMPTS - 1) / 2) * 0.007 for index in range(ATTEMPTS)]
                axis.scatter([x + jitter for jitter in jitters], values, s=14, alpha=0.55, color=color, zorder=3)
            axis.errorbar(xs, medians, yerr=[lows, highs], fmt="o-", linewidth=1.3, markersize=4,
                          color=color, capsize=3, label=f"{payload} B payload", zorder=4)
        axis.set_xticks(range(len(HISTORIES)), [str(value) for value in HISTORIES])
        axis.set(title=title, ylabel=ylabel, xlabel="Initial retained records (H)", ylim=(0, None))
        axis.grid(axis="y", alpha=0.25)
        axis.spines[["top", "right"]].set_visible(False)

    figure, axes = plt.subplots(2, 2, figsize=(13.5, 10))
    figure.subplots_adjust(left=0.09, right=0.98, top=0.84, bottom=0.22, hspace=0.58, wspace=0.31)
    panel(axes[0, 0], "prepared_append_ns", 1e3, "Prepared ID issue + append", "Microseconds / append (lower is better)")
    panel(axes[0, 1], "prepared_appends_per_second", 1e6, "Prepared append rate", "Million appends / second (higher is better)")
    panel(axes[1, 0], "replay_ns_per_record", 1e3, "Borrowed replay iteration", "Microseconds / record (lower is better)")
    panel(axes[1, 1], "replay_records_per_second", 1e6, "Borrowed replay rate", "Million records / second (higher is better)")
    figure.suptitle("Producer-owned log: prepared append and replay", fontsize=16, y=0.98)
    figure.legend(*axes[0, 0].get_legend_handles_labels(), loc="upper center", bbox_to_anchor=(0.5, 0.94), ncol=3, frameon=False)
    figure.text(0.5, 0.035,
                "Dots: all 10 attempt values. Lines: median; whiskers: min–max. Each attempt value is the median of five block means.\n"
                "Each block appends 64 records; append blocks start at H, H+64, …, H+256. Full replay cuts contain H+64, …, H+320 records.\n"
                "Caller-prepared schema/topology/payload; caller-supplied constant timestamp; allocation hooks enabled and zero requests asserted.\n"
                "Local storage components only: admission, ring notification, scheduling and application acknowledgments are outside this measurement.\n"
                "These are not transport latency, delivered-message throughput, nanomsg comparisons or p99 estimates.",
                ha="center", va="bottom", fontsize=8.6, linespacing=1.6)
    for suffix in ("png", "svg"):
        figure.savefig(output / ("append-replay-latency-throughput." + suffix), dpi=170,
                       metadata={"Software": "Angaria producer-owned append study"} if suffix == "png" else
                                {"Date": None, "Creator": "Angaria producer-owned append study"})
    plt.close(figure)

    figure, axes = plt.subplots(1, 3, figsize=(16, 6.4))
    figure.subplots_adjust(left=0.06, right=0.985, top=0.76, bottom=0.29, wspace=0.32)
    panel(axes[0], "writer_recovery_open_ns", 1e3, "Writer reopen + prefix recovery", "Microseconds / open (lower is better)")
    panel(axes[1], "reader_open_with_counter_capture_ns", 1e3, "Reader open + counter-capture clocks", "Microseconds / open (lower is better)")
    panel(axes[2], "capture_ns", 1e3, "Capture / validate 64 newly committed records", "Microseconds / capture (lower is better)")
    figure.suptitle("Producer-owned log: open/recovery and incremental capture", fontsize=16, y=0.97)
    figure.legend(*axes[0].get_legend_handles_labels(), loc="upper center", bbox_to_anchor=(0.5, 0.91), ncol=3, frameon=False)
    figure.text(0.5, 0.04,
                "Dots: all 10 attempts; lines: median; whiskers: min–max. Open costs are one fresh-handle observation per attempt.\n"
                "The OS cache is warm from fixture creation/seeding. This is not disk-cold, power-loss recovery or a durability measurement.\n"
                "Capture values are medians of five captures, each validating only 64 new records; full replay is timed separately.\n"
                "Reader open includes the diagnostic Capture object's wall/CPU clocks with library instrumentation disabled.",
                ha="center", va="bottom", fontsize=9, linespacing=1.6)
    for suffix in ("png", "svg"):
        figure.savefig(output / ("recovery-capture." + suffix), dpi=170,
                       metadata={"Software": "Angaria producer-owned append study"} if suffix == "png" else
                                {"Date": None, "Creator": "Angaria producer-owned append study"})
    plt.close(figure)


def render(output, validate_only=False):
    attempts, summary = reconstruct(output)
    if not validate_only:
        plot(output, attempts, summary)
        write_csv(output / "attempts.csv", attempts)
        write_csv(output / "summary.csv", summary)
    print(f"Validated {ATTEMPTS} attempts / {len(attempts)} cases / {len(attempts) * SAMPLES} append block means; "
          "all raw capture/replay blocks retained.")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    collection = commands.add_parser("collect", help="run ten sequential, fully retained attempts from frozen source")
    collection.add_argument("binary", type=Path)
    collection.add_argument("--source-commit", required=True, help="exact source commit; HEAD and implementation files must match")
    collection.add_argument("--source-root", type=Path, default=Path(__file__).resolve().parents[1])
    collection.add_argument("--build-dir", type=Path, help="defaults to the executable's parent")
    collection.add_argument("--output", type=Path, required=True, help="new or empty evidence directory")
    collection.add_argument("--timeout", type=float, default=60.0, help="seconds per attempted executable invocation")
    collection.add_argument("--no-plots", action="store_true", help="collect/validate without importing matplotlib")
    reconstruction = commands.add_parser("render", help="validate retained evidence and reconstruct CSVs/figures without execution")
    reconstruction.add_argument("output", type=Path)
    reconstruction.add_argument("--validate-only", action="store_true")
    options = parser.parse_args()
    if options.command == "collect":
        require(math.isfinite(options.timeout) and 0 < options.timeout <= 60, "timeout must be within (0,60] seconds")
        collect(options)
    else:
        render(options.output.resolve(), options.validate_only)


if __name__ == "__main__":
    try:
        main()
    except (ValueError, OSError, subprocess.CalledProcessError, ET.ParseError) as error:
        print(f"Study failed: {error}", file=sys.stderr)
        sys.exit(1)
