#!/usr/bin/env python3
"""Retain and reconstruct the matched recorded-connection/nanomsg IPC study.

Uses the existing IPC coordinator and its authoritative row validation. Finite
cohorts measure logical start to delivery and verified completion, including
ordinary producer lookup. They do not establish sustained capacity or p99.
"""

import argparse
import csv
from datetime import datetime, timezone
import gzip
import hashlib
import json
import os
from pathlib import Path
import platform
import shutil
import statistics
import subprocess
import sys
import tarfile

sys.dont_write_bytecode = True
import run_ipc_comparison as ipc

ROOT = Path(__file__).resolve().parents[1]
PROFILES = ("recorded-connection", "nanomsg")
LABELS = {"recorded-connection": "Angaria: recorded, unacknowledged; ID + send + lookup",
          "nanomsg": "libnanomsg IPC: unrecorded, unacknowledged"}
COLORS = {"recorded-connection": "#168469", "nanomsg": "#cc702d"}
METRICS = (("logical_start_to_delivery_ns", "Logical start → delivery (µs)", 1000),
           ("bounded_cohort_verified_per_second", "Verified publications / second", 1))


def write_json(path, value):
    path.write_text(json.dumps(value, indent=2, sort_keys=True, allow_nan=False) + "\n")


def command_metadata(argv):
    try:
        result = subprocess.run(argv, capture_output=True, text=True, timeout=10)
        return dict(argv=[str(arg) for arg in argv], exit_code=result.returncode,
                    stdout=result.stdout.strip(), stderr=result.stderr.strip())
    except (OSError, subprocess.TimeoutExpired) as error:
        return dict(argv=[str(arg) for arg in argv], error=str(error))


def source_snapshot(commit):
    head = subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=ROOT, text=True).strip()
    pinned = subprocess.check_output(["git", "rev-parse", commit + "^{commit}"], cwd=ROOT, text=True).strip()
    ipc.check(head == pinned, "HEAD must equal the requested frozen source commit")
    names = subprocess.check_output(["git", "ls-files", "-z"], cwd=ROOT).decode().split("\0")
    def relevant(name):
        return bool(name) and (Path(name).suffix in (".cpp", ".h", ".hpp", ".cmake", ".in") or
                               name in ("CMakeLists.txt", "VERSION", "benchmarks/recorded_connection_study.py",
                                        "benchmarks/run_ipc_comparison.py", "benchmarks/run_native.py", "test/ipc_comparison.py"))
    names = [name for name in names if relevant(name)]
    unknown = subprocess.check_output(["git", "ls-files", "--others", "--exclude-standard", "-z"], cwd=ROOT).decode().split("\0")
    ipc.check(not any(relevant(name) for name in unknown), "Commit all implementation inputs before measurement")
    ipc.check(subprocess.run(["git", "diff", "--quiet", "HEAD", "--", *names], cwd=ROOT).returncode == 0,
              "Implementation inputs differ from the frozen source")
    return dict(commit=pinned, sha256={name: ipc.sha256(ROOT / name) for name in sorted(names)})


def cache_values(path):
    result = {}
    for line in path.read_text().splitlines():
        if line and not line.startswith(("#", "//")) and ":" in line and "=" in line:
            key, value = line.split("=", 1)
            result[key.split(":", 1)[0]] = value
    return result


def build_snapshot(binary, output):
    cache = binary.parent / "CMakeCache.txt"
    values = cache_values(cache)
    ipc.check(values.get("CMAKE_BUILD_TYPE") == "Release", "Use a Release build")
    ipc.check(values.get("ANGARIA_ENABLE_DIAGNOSTICS", "").upper() in ("OFF", "FALSE", "NO", "0"),
              "Ordinary timings require diagnostics OFF")
    ipc.check(values.get("ANGARIA_ENABLE_CACHEGRIND", "OFF").upper() in ("OFF", "FALSE", "NO", "0"),
              "Ordinary timings require Cachegrind OFF")
    files = {"CMakeCache.txt": cache, "angaria.a": binary.parent / "libangaria.a", "executable": binary}
    nanomsg = Path(values.get("nanomsg_BINARY_DIR", "")) / "libnanomsg.a"
    ipc.check(nanomsg.is_file(), "CMake must identify the actual statically linked nanomsg archive")
    files["nanomsg.a"] = nanomsg
    for target in ("angaria", "angaria_ipc_comparison"):
        for name in ("flags.make", "link.txt"):
            path = binary.parent / "CMakeFiles" / (target + ".dir") / name
            if path.is_file():
                files[target + "-" + name] = path
    provenance = output / "provenance"
    provenance.mkdir()
    for name, path in files.items():
        if name not in ("angaria.a", "nanomsg.a", "executable"):
            shutil.copyfile(path, provenance / name)
    return files, dict(sha256={name: ipc.sha256(path) for name, path in files.items()},
                       compiler=command_metadata([values["CMAKE_CXX_COMPILER"], "--version"]),
                       executable_architecture=command_metadata(["file", str(binary)]),
                       hardware_optional=command_metadata(["/usr/sbin/sysctl", "-n", "machdep.cpu.brand_string"])
                       if platform.system() == "Darwin" else command_metadata(["lscpu"]),
                       host=dict(platform.uname()._asdict()), coordinator_python=sys.version,
                       logical_cpus=os.cpu_count())


def runner_metadata(path):
    # The coordinator emits flat JSON-literal values in named TOML sections.
    # This reads that owned measurement format, not general user configuration.
    top, sources, cmake, cases = {}, {}, {}, []
    section = top
    for line in path.read_text().splitlines():
        if not line.strip():
            continue
        if line == "[[cases]]":
            section = {}
            cases.append(section)
        elif line.startswith("["):
            section = sources if line == "[source_sha256]" else cmake if line == "[cmake]" else None
        elif section is not None:
            key, value = line.split(" = ", 1)
            key = json.loads(key)
            ipc.check(key not in section, "Duplicate runner metadata key")
            section[key] = json.loads(value)
    return top, sources, cmake, cases


def pack(raw, output, commit, extra=None):
    metadata, sources, cmake, cases = runner_metadata(raw / "manifest.toml")
    ipc.check(metadata.get("git_head") == commit, "Runner source commit differs from the requested source")
    for name in ("runs.csv", "summary.csv", "manifest.toml"):
        shutil.copyfile(raw / name, output / name)
    with (raw / "reports.jsonl").open("rb") as source, (output / "reports.jsonl.gz").open("wb") as target:
        with gzip.GzipFile(fileobj=target, mode="wb", filename="", mtime=0) as compressed:
            shutil.copyfileobj(source, compressed)
    with tarfile.open(output / "raw-attempts.tar.gz", "w:gz") as archive:
        for path in sorted(raw.iterdir()):
            if path.is_dir():
                archive.add(path, arcname=path.name)
    with (raw / "reports.jsonl").open() as stream:
        first = next((json.loads(line) for line in stream if line.strip()), {})
    version = first.get("worker_reports", {}).get("producer", {}).get("framework_version",
                first.get("setup", {}).get("framework_version", "unavailable"))
    manifest = dict(schema_version=1, source_commit=commit, framework_version=version,
                    retention_script_sha256=ipc.sha256(Path(__file__)),
                    created_utc=datetime.now(timezone.utc).isoformat(), status="retained",
                    runner_metadata=metadata, source_sha256=sources, cmake=cmake, cases=cases,
                    interpretation="Finite fresh 128-message cohorts by default; no sustained-capacity, p99 or old-engine speedup claim",
                    guarantees=LABELS, raw_archive_scope="All attempt directories, including failed attempts; original stdout/stderr and reports preserved",
                    vm_fault_counts="Not collected by the IPC harness; no fault or physical-I/O inference from timing",
                    cpu_scope="Original worker/process and coordinator intervals retained; includes lead/control waits and library threads")
    if extra:
        manifest.update(extra)
    evidence = [path for path in output.rglob("*") if path.is_file() and
                not path.resolve().is_relative_to(raw.resolve()) and path.name not in ("validation.json", "manifest.json")]
    manifest["evidence_sha256"] = {str(path.relative_to(output)): ipc.sha256(path) for path in sorted(evidence)}
    write_json(output / "manifest.json", manifest)


def collect(options):
    binary, output = options.binary.resolve(strict=True), options.output.resolve()
    ipc.check(not output.exists(), "Choose a new output directory; existing evidence is never overwritten")
    before = source_snapshot(options.source_commit)
    output.mkdir(parents=True)
    files, build = build_snapshot(binary, output)
    argv = [sys.executable, "-B", str(ROOT / "benchmarks/run_ipc_comparison.py"), str(binary),
            "--angaria-profile", "recorded-connection", "--output", str(output / "attempts"),
            "--blocks", str(options.blocks), "--messages", str(options.messages), "--histories", options.histories,
            "--payload-bytes", options.payload_bytes, "--ring-slots", str(options.ring_slots),
            "--budget-ms", str(options.budget_ms), "--timeout", str(options.timeout),
            "--start-lead-ms", str(options.start_lead_ms), "--host-label", options.host_label]
    extra = dict(collection_command=argv, source_before=before, build=build,
                 execution_context="Current process context; local Unix-domain socket access is required; no CPU affinity requested")
    print("Collecting the matched pair; progress and every attempt are retained in runner.stdout.txt", flush=True)
    with (output / "runner.stdout.txt").open("wb") as stdout, (output / "runner.stderr.txt").open("wb") as stderr:
        result = subprocess.run(argv, cwd=ROOT, stdout=stdout, stderr=stderr)
    extra["runner_exit_code"] = result.returncode
    try:
        extra["source_after"] = source_snapshot(options.source_commit)
        extra["source_build_unchanged"] = extra["source_after"] == before and all(
            ipc.sha256(path) == build["sha256"][name] for name, path in files.items())
    except (OSError, RuntimeError, subprocess.SubprocessError) as error:
        extra.update(source_build_unchanged=False, freeze_error=str(error))
    pack(output / "attempts", output, before["commit"], extra)
    # Only remove redundant expanded files after the lossless archive validates.
    render(output, validate_only=True)
    shutil.rmtree(output / "attempts")
    if not options.no_plots:
        render(output)


def reconstruct(output):
    manifest = json.loads((output / "manifest.json").read_text())
    for name, expected in manifest["evidence_sha256"].items():
        ipc.check(ipc.sha256(output / name) == expected, "Retained evidence changed: " + name)
    if "source_build_unchanged" in manifest:
        ipc.check(manifest["source_build_unchanged"] and manifest.get("runner_exit_code") == 0,
                  "Collection failed or source/build changed; every attempted result remains retained")
    with gzip.open(output / "reports.jsonl.gz", "rt") as stream:
        reports = [json.loads(line) for line in stream if line.strip()]
    metadata = manifest["runner_metadata"]
    ipc.check(metadata.get("status") == "complete" and metadata.get("angaria_profile") == PROFILES[0],
              "Runner did not complete the requested recorded comparison")
    ipc.check(metadata.get("git_tracked_dirty") is False, "Runner did not measure a frozen tracked source tree")
    cases = manifest["cases"]
    ipc.check(len(reports) == metadata["planned_runs"] == len(cases) * metadata["blocks"] * 4,
              "Incomplete matched matrix; retained attempts cannot be silently excluded")
    expected = [(case, block, position, profile) for case in cases for block in range(1, metadata["blocks"] + 1)
                for position, profile in enumerate((PROFILES[0], PROFILES[1], PROFILES[1], PROFILES[0]) if block % 2 else
                                                    (PROFILES[1], PROFILES[0], PROFILES[0], PROFILES[1]), 1)]
    with tarfile.open(output / "raw-attempts.tar.gz", "r:gz") as archive:
        for index, (report, (case, block, position, profile)) in enumerate(zip(reports, expected), 1):
            ipc.check(report["attempt"] == index and report["profile"] == profile and report["block"] == block and
                      report["position"] == position and report["status"] == "complete", "Changed run order or incomplete run")
            for name, value in case.items():
                ipc.check(report[name] == value, "Changed matched workload dimension")
            for role, worker in report["worker_reports"].items():
                ipc.common_report(worker, role, profile, case, manifest["framework_version"])
                ipc.check(worker.get("instrumentation_enabled") is False and worker.get("collection") == "none",
                          "Instrumented timings cannot be pooled with ordinary measurements")
                member = archive.extractfile(report["artifact_directory"] + "/" + role + ".stdout")
                ipc.check(member is not None, "Missing original worker stdout")
                events = [json.loads(line) for line in member.read().decode().splitlines() if line.strip()]
                originals = [event for event in events if event.get("event") == "REPORT"]
                ipc.check(originals == [worker], "Aggregated worker rows differ from original stdout")
            ipc.validate_cached_summary(report)
            ipc.check(report["accepted"] == report["delivered"] == report["messages"] and
                      report["acknowledged"] == report["pending"] == 0, "Different delivery/completion guarantees")
    return manifest, reports


def summaries(reports):
    result = []
    cells = sorted({(row["profile"], row["history"], row["payload_bytes"]) for row in reports})
    for profile, history, payload in cells:
        group = [row for row in reports if (row["profile"], row["history"], row["payload_bytes"]) == (profile, history, payload)]
        for metric, _, _ in METRICS:
            values = [ipc.rank(row["metrics"][metric], 1, 2) if metric in row["metrics"] else row["rates"][metric] for row in group]
            result.append(dict(profile=profile, history=history, payload_bytes=payload, metric=metric,
                               runs=len(values), median=statistics.median(values), minimum=min(values), maximum=max(values),
                               attempt_values=json.dumps(values)))
    return result


def plot(output, manifest, reports, rows):
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt
    matplotlib.rcParams["svg.hashsalt"] = "angaria-recorded-connection"
    payloads = sorted({row["payload_bytes"] for row in reports})
    histories = sorted({row["history"] for row in reports})
    figure, axes = plt.subplots(2, len(payloads), figsize=(5.1 * len(payloads), 8.0), squeeze=False)
    for column, payload in enumerate(payloads):
        for row_index, (metric, ylabel, scale) in enumerate(METRICS):
            ax = axes[row_index, column]
            for profile_index, profile in enumerate(PROFILES):
                centers, lows, highs, xs = [], [], [], []
                for history_index, history in enumerate(histories):
                    summary = next(row for row in rows if (row["profile"], row["history"], row["payload_bytes"], row["metric"]) ==
                                   (profile, history, payload, metric))
                    values = [value / scale for value in json.loads(summary["attempt_values"])]
                    x = history_index + (profile_index - .5) * .11
                    jitter = [(index - (len(values) - 1) / 2) * .007 for index in range(len(values))]
                    ax.scatter([x + shift for shift in jitter], values, s=19, alpha=.5, color=COLORS[profile])
                    center = summary["median"] / scale
                    xs.append(x); centers.append(center)
                    lows.append(center - min(values)); highs.append(max(values) - center)
                ax.errorbar(xs, centers, yerr=[lows, highs], marker="o", capsize=4, color=COLORS[profile],
                            linewidth=1.7, label=LABELS[profile])
            ax.set_xticks(range(len(histories)), [str(value) for value in histories])
            ax.set_xlabel("Initial retained messages (H)")
            if column == 0:
                ax.set_ylabel(ylabel + " · log scale")
            ax.set_yscale("log"); ax.grid(axis="y", alpha=.2)
            if row_index == 0:
                ax.set_title(str(payload) + "-byte payload")
    handles, labels = axes[0, 0].get_legend_handles_labels()
    figure.legend(handles, labels, loc="upper center", bbox_to_anchor=(.5, .94), frameon=False, fontsize=10)
    figure.suptitle("Private recorded point-to-point vs classic nanomsg — " + manifest["runner_metadata"].get("host_label", ""), fontsize=13)
    figure.subplots_adjust(top=.80, bottom=.20, hspace=.48, wspace=.38)
    figure.text(.5, .035,
                "Dots: every attempt; line: median; whiskers: full run range. Latency points are per-run nearest-rank p50.\n"
                "Finite fresh " + str(reports[0]["messages"]) + "-message cohorts; throughput waits for verified delivery and Angaria's ordinary producer lookup.\n"
                "Different recording guarantees; no sustained-capacity or per-message p99 claim. No timing outliers are removed.\n"
                "Angaria uses nonblocking spin/yield; nanomsg uses nonblocking socket calls with poll. Startup and setup are outside data intervals.",
                ha="center", fontsize=9, linespacing=1.6)
    for suffix in ("png", "svg"):
        path = output / ("latency-throughput." + suffix)
        figure.savefig(path, dpi=170, metadata={"Software": "Angaria recorded connection study"} if suffix == "png" else
                       {"Date": None, "Creator": "Angaria recorded connection study"})
        if suffix == "svg":
            path.write_text("\n".join(line.rstrip() for line in path.read_text().splitlines()) + "\n")
    plt.close(figure)


def render(output, validate_only=False):
    manifest, reports = reconstruct(output)
    rows = summaries(reports)
    if not validate_only:
        with (output / "comparison.csv").open("w", newline="") as stream:
            writer = csv.DictWriter(stream, fieldnames=list(rows[0]), lineterminator="\n")
            writer.writeheader(); writer.writerows(rows)
        plot(output, manifest, reports, rows)
    print("Verified " + str(len(reports)) + " matched attempts; all raw worker reports and cached summaries agree.")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    collection = commands.add_parser("collect")
    collection.add_argument("binary", type=Path)
    collection.add_argument("--source-commit", required=True)
    collection.add_argument("--output", type=Path, required=True)
    collection.add_argument("--host-label", default=platform.system() + "; native executable architecture retained in manifest")
    collection.add_argument("--blocks", type=int, default=5)
    collection.add_argument("--messages", type=int, default=128)
    collection.add_argument("--histories", default="0,16,128")
    collection.add_argument("--payload-bytes", default="64,256,1024")
    collection.add_argument("--ring-slots", type=int, default=8)
    collection.add_argument("--budget-ms", type=int, default=10000)
    collection.add_argument("--timeout", type=int, default=30)
    collection.add_argument("--start-lead-ms", type=int, default=100)
    collection.add_argument("--no-plots", action="store_true")
    packing = commands.add_parser("pack", help="retain an existing runner output, e.g. downloaded Linux CI evidence")
    packing.add_argument("raw", type=Path)
    packing.add_argument("--source-commit", required=True)
    packing.add_argument("--output", type=Path, required=True)
    rendering = commands.add_parser("render")
    rendering.add_argument("output", type=Path)
    rendering.add_argument("--validate-only", action="store_true")
    options = parser.parse_args()
    if options.command == "collect":
        collect(options)
    elif options.command == "pack":
        ipc.check(not options.output.exists(), "Choose a new output directory")
        options.output.mkdir(parents=True)
        pack(options.raw.resolve(), options.output.resolve(), options.source_commit)
        render(options.output.resolve())
    else:
        render(options.output.resolve(), options.validate_only)


if __name__ == "__main__":
    try:
        main()
    except (OSError, RuntimeError, ValueError, subprocess.SubprocessError, tarfile.TarError) as error:
        print("Recorded connection study failed: " + str(error), file=sys.stderr)
        sys.exit(1)
