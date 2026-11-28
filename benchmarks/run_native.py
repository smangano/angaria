#!/usr/bin/env python3
"""Run bounded native benchmarks; retain per-run reports, never Session files.

CSV quantiles are medians of the separate runs' quantiles, not quantiles of a
pooled sample. Profile mode measures an instrumented private engine separately
from the public buffered API. No elapsed-time value is a pass/fail threshold.
"""
import argparse
import csv
from datetime import datetime, timezone
import hashlib
import json
from pathlib import Path
import platform
import re
import statistics
import subprocess
import sys
import tempfile


ROOT = Path(__file__).resolve().parents[1]
QUANTILES = ("min_ns", "p50_ns", "p95_ns", "p99_ns", "max_ns", "total_ns")
DIMENSIONS = ("mode", "api_kind", "messages", "warmup_messages", "producers", "payload_bytes")
PUBLIC_METRICS = {"issue", "send", "receive", "acknowledge"}
PROFILE_METRICS = {"issue", "receive", "acknowledge", "gate_wait", "gate_held",
                   "prepare", "file_growth", "materialize", "apply", "profile_send"}


def check(condition, message):
    if not condition:
        raise RuntimeError(message)


def invoke(command, timeout, succeeds=True):
    try:
        result = subprocess.run([str(part) for part in command], capture_output=True,
                                text=True, errors="replace", timeout=timeout)
    except subprocess.TimeoutExpired as error:
        raise RuntimeError(f"Timed out: {command!r}\nstdout: {error.stdout!r}\nstderr: {error.stderr!r}") from error
    check(result.returncode == (0 if succeeds else 1),
          f"Unexpected exit {result.returncode}: {command!r}\nstdout: {result.stdout}\nstderr: {result.stderr}")
    if not succeeds:
        check(not result.stdout.strip() and bool(result.stderr.strip()),
              f"Rejected command must report an error only on stderr: {command!r}")
    return result.stdout


def validate(report, case):
    n, w, p, payload, mode = case
    check(type(report) is dict, "Benchmark report must be a JSON object")
    check(type(report.get("framework_version")) is str and
          re.fullmatch(r"\d+\.\d+\.\d+", report["framework_version"]), "Invalid framework version")
    check(report["framework_version"] == (ROOT / "VERSION").read_text().strip(),
          "Benchmark version differs from this source checkout; rebuild the executable")
    expected = {"messages": n, "warmup_messages": w, "producers": p, "payload_bytes": payload,
                "channels": 4, "ring_capacity": 8, "history_before": w, "history_after": w + n,
                "completed": n, "checksum": n * (payload // 8)}
    for name, value in expected.items():
        check(type(report.get(name)) is int and report[name] == value, f"Invalid {name}: {report.get(name)!r}")
    api = "instrumented_private_engine" if mode == "profile" else "buffered_public"
    check(report.get("mode") == mode and report.get("api_kind") == api, "Wrong mode/API kind")
    for name in ("file_bytes_before", "file_bytes_after_publish", "file_bytes_after_drain", "append_bytes",
                 "publish_phase_ns", "drain_phase_ns", "cycle_phase_ns"):
        check(type(report.get(name)) is int and report[name] >= 0, f"Invalid {name}")
    before, published, drained = (report[name] for name in
                                  ("file_bytes_before", "file_bytes_after_publish", "file_bytes_after_drain"))
    check(before > 0 and published == drained and published - before == report["append_bytes"] > 0,
          "Append accounting or reserved drain progress changed")
    check(all(value % 64 == 0 for value in (before, published, drained, report["append_bytes"])),
          "File/append lengths must remain 64-byte aligned")
    check((report["publish_phase_ns"] == report["drain_phase_ns"] == 0 and report["cycle_phase_ns"] > 0)
          if mode == "serial" else (report["publish_phase_ns"] > 0 and report["drain_phase_ns"] > 0
                                    and report["cycle_phase_ns"] == 0), "Wrong phase timing shape")
    metrics = report.get("metrics")
    check(type(metrics) is list and all(type(metric) is dict for metric in metrics), "Invalid metrics array")
    names = [metric.get("name") for metric in metrics]
    expected_names = PROFILE_METRICS if mode == "profile" else PUBLIC_METRICS | ({"cycle"} if mode == "serial" else set())
    check(all(type(name) is str for name in names) and len(names) == len(set(names))
          and set(names) == expected_names, "Missing, repeated, or unexpected metrics")
    for metric in metrics:
        check(type(metric.get("count")) is int and metric["count"] == n,
              f"Wrong sample/phase count for {metric['name']}")
        values = [metric.get(name) for name in QUANTILES]
        check(all(type(value) is int and value >= 0 for value in values)
              and values == sorted(values), f"Invalid ordered quantiles for {metric['name']}")
        check(values[0] * n <= values[-1] <= values[-2] * n, f"Invalid sample total for {metric['name']}")


def sha256(path):
    with path.open("rb") as source:
        return hashlib.file_digest(source, "sha256").hexdigest() if hasattr(hashlib, "file_digest") else _hash_stream(source)


def _hash_stream(source):
    digest = hashlib.sha256()
    for block in iter(lambda: source.read(1024 * 1024), b""):
        digest.update(block)
    return digest.hexdigest()


def metadata(executable, options):
    """Common host/build identity; each runner adds its own data-plane inputs.

    The public IPC runner reuses this without the retired native-transport
    executable. Keeping a driver-specific source here would make current IPC
    provenance fail before starting any participants.
    """
    def optional(command, cwd=None):
        try:
            return subprocess.check_output(command, cwd=cwd, stderr=subprocess.DEVNULL,
                                           text=True, errors="replace", timeout=10).strip()
        except (OSError, subprocess.SubprocessError):
            return "unavailable"

    cache = next((parent / "CMakeCache.txt" for parent in list(executable.parents)[:4]
                  if (parent / "CMakeCache.txt").is_file()), None)
    settings = {}
    if cache:
        for line in cache.read_text(errors="replace").splitlines():
            match = re.match(r"(CMAKE_(?:BUILD_TYPE|CXX_COMPILER|CXX_FLAGS(?:_[A-Z]+)?|CONFIGURATION_TYPES)):[^=]+=(.*)", line)
            if match:
                settings[match[1]] = match[2]
    cpu = platform.processor() or "unavailable"
    if sys.platform == "darwin":
        cpu = optional(["sysctl", "-n", "machdep.cpu.brand_string"])
    elif Path("/proc/cpuinfo").is_file():
        try:
            cpu = next((line.split(":", 1)[1].strip() for line in Path("/proc/cpuinfo").read_text().splitlines()
                        if line.startswith("model name")), cpu)
        except OSError:
            pass
    status = optional(["git", "status", "--porcelain"], ROOT)
    tracked_status = optional(["git", "status", "--porcelain", "--untracked-files=no"], ROOT)
    manifest = {"started_utc": datetime.now(timezone.utc).isoformat(), "host_label": options.host_label,
                "platform": platform.platform(), "machine": platform.machine(), "cpu": cpu,
                "python": platform.python_version(), "git_head": optional(["git", "rev-parse", "HEAD"], ROOT),
                "git_dirty": bool(status) if status != "unavailable" else "unavailable",
                "git_tracked_dirty": bool(tracked_status) if tracked_status != "unavailable" else "unavailable",
                "executable": str(executable), "executable_sha256": sha256(executable),
                "compiler_version": (optional([settings["CMAKE_CXX_COMPILER"], "--version"]) or "unavailable").splitlines()[0]
                if settings.get("CMAKE_CXX_COMPILER") else "unavailable",
                "cmake_cache": str(cache) if cache else "unavailable", "smoke": options.smoke,
                "timeout_seconds": options.timeout, "repetitions": 1 if options.smoke else options.repetitions,
                "runner_arguments": sys.argv[1:],
                "summary_method": "median of per-run quantiles; samples are not pooled"}
    sources = {name: sha256(ROOT / name) for name in
               ("CMakeLists.txt", "VERSION", "benchmarks/run_native.py")}
    return manifest, sources, settings


def write_artifacts(directory, reports, manifest, sources, settings):
    with (directory / "reports.jsonl").open("w", encoding="utf-8") as output:
        for report in reports:
            output.write(json.dumps(report, sort_keys=True) + "\n")
    groups = {}
    for report in reports:
        groups.setdefault(tuple(report[name] for name in DIMENSIONS), []).append(report)
    columns = list(DIMENSIONS) + ["metric", "runs", "samples_per_run"] + ["median_run_" + name for name in QUANTILES]
    with (directory / "summary.csv").open("w", newline="", encoding="utf-8") as output:
        writer = csv.DictWriter(output, fieldnames=columns)
        writer.writeheader()
        for dimensions, runs in groups.items():
            for metric in runs[0]["metrics"]:
                row = dict(zip(DIMENSIONS, dimensions), metric=metric["name"], runs=len(runs), samples_per_run=metric["count"])
                samples = [{item["name"]: item for item in run["metrics"]}[metric["name"]] for run in runs]
                row.update({"median_run_" + name: statistics.median(sample[name] for sample in samples)
                            for name in QUANTILES})
                writer.writerow(row)
    with (directory / "manifest.toml").open("w", encoding="utf-8") as output:
        for section, values in ((None, manifest), ("source_sha256", sources), ("cmake", settings)):
            if section:
                output.write(f"\n[{section}]\n")
            for key, value in values.items():
                # TOML accepts Unicode scalars, not JSON's surrogate-pair escapes.
                output.write(f"{json.dumps(key, ensure_ascii=False)} = {json.dumps(value, ensure_ascii=False)}\n")


def smoke_rejections(executable, temporary, timeout, existing):
    valid = [8, 4, 1, 8, "serial"]
    invalid = [(0, 0), (0, 4097), (3, 0), (3, 7), (3, 4104), (2, 3), (2, 0),
               (4, "unknown"), (0, "+8"), (1, 257)]
    cases = [valid[:index] + [value] + valid[index + 1:] for index, value in invalid]
    cases += [[7, 4, 2, 8, "phased"], [8, 3, 2, 8, "profile"], [8, 4, 2, 8, "serial"]]
    for index, arguments in enumerate(cases):
        directory = temporary / f"invalid-{index}"
        invoke([executable, directory, *arguments], timeout, succeeds=False)
        check(not directory.exists() and not directory.is_symlink(), "Rejected CLI created a Session directory")
    def snapshot():
        return {str(path.relative_to(existing)): sha256(path) if path.is_file() else "directory"
                for path in existing.rglob("*")}
    before = snapshot()
    invoke([executable, existing, *valid], timeout, succeeds=False)
    check(snapshot() == before, "Existing-Session rejection changed its files")


def parse_arguments():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("executable", type=Path)
    parser.add_argument("--output", type=Path, help="new directory for JSONL, CSV, and TOML reports")
    parser.add_argument("--smoke", action="store_true", help="fixed correctness matrix, no timing thresholds")
    parser.add_argument("--messages", type=int, default=128)
    parser.add_argument("--warmup", type=int, default=16)
    parser.add_argument("--repetitions", type=int, default=3)
    parser.add_argument("--payload-bytes", default="8,256", help="comma-separated payload byte counts")
    parser.add_argument("--producers", default="1,2,4", help="comma-separated producer counts")
    parser.add_argument("--modes", default="serial,phased,profile", help="comma-separated execution modes")
    parser.add_argument("--timeout", type=int, default=120, help="seconds allowed for each subprocess")
    parser.add_argument("--host-label", default="", help="optional verified description of the measurement host")
    options = parser.parse_args()
    try:
        payloads = list(dict.fromkeys(int(value) for value in options.payload_bytes.split(",")))
        producers = list(dict.fromkeys(int(value) for value in options.producers.split(",")))
        modes = list(dict.fromkeys(options.modes.split(",")))
        check(options.output is not None or options.smoke, "Normal runs require --output NEWDIR")
        check(0 < options.messages <= 4096 and 0 <= options.warmup <= 256, "Messages must be 1..4096; warmup 0..256")
        check(options.repetitions > 0 and options.timeout > 0, "Repetitions and timeout must be positive")
        check(all(8 <= value <= 4096 and value % 8 == 0 for value in payloads), "Payloads must be multiples of 8 in 8..4096")
        check(all(value in (1, 2, 4) for value in producers), "Producers must be 1, 2, or 4")
        check(all(value in ("serial", "phased", "profile") for value in modes), "Unknown benchmark mode")
        cases = [(options.messages, options.warmup, p, payload, mode)
                 for payload in payloads for mode in modes for p in producers if mode != "serial" or p == 1]
        check(cases and all(n % p == 0 and w % p == 0 for n, w, p, _, _ in cases),
              "Each message/warmup count must divide evenly among producers; serial requires producer 1")
        if options.smoke:
            cases = [(8, 4, p, 8, mode) for mode in ("serial", "phased", "profile")
                     for p in (1, 2, 4) if mode != "serial" or p == 1] + [(8, 4, 1, 256, "serial")]
        executable = options.executable.resolve(strict=True)
        check(executable.is_file(), "Executable must be a regular file")
    except (ValueError, OSError, RuntimeError) as error:
        parser.error(str(error))
    return options, executable, cases


def main():
    options, executable, cases = parse_arguments()
    reports = []
    output = options.output.absolute() if options.output else None
    try:
        if output:
            output.mkdir()  # Exclusive reservation also rejects dangling symlinks.
        manifest, sources, settings = metadata(executable, options)
        manifest["status"] = "failed"
        try:
            with tempfile.TemporaryDirectory(prefix="angaria-native-benchmark-") as temporary_name:
                temporary = Path(temporary_name)
                for case in cases:
                    for repetition in range(1 if options.smoke else options.repetitions):
                        # Remove each run's native files before the next run.
                        with tempfile.TemporaryDirectory(prefix="run-", dir=temporary) as run_directory:
                            directory = Path(run_directory) / "session"
                            report = json.loads(invoke([executable, directory, *case], options.timeout))
                            validate(report, case)
                            reports.append(dict(report, repetition=repetition + 1))
                            if options.smoke and len(reports) == 1:
                                smoke_rejections(executable, temporary, options.timeout, directory)
            manifest["status"] = "complete"
        except Exception as error:
            manifest["failure"] = str(error)
            raise
        finally:
            manifest["finished_utc"] = datetime.now(timezone.utc).isoformat()
            manifest["completed_runs"] = len(reports)
            if output:
                write_artifacts(output, reports, manifest, sources, settings)
        print(f"Validated {len(reports)} benchmark runs" + (f"; reports retained at {output}" if output else " (smoke)."))
        return 0
    except (OSError, RuntimeError, ValueError, KeyError, TypeError) as error:
        print(f"run_native: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
