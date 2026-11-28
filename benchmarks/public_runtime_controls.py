#!/usr/bin/env python3
"""Compare public admission with the private connection core on one Linux host.

Five cyclic rotations balance every profile across all five order positions.
The 25 finite core64 attempts reuse the qualification process protocol, binary
sample validation, and lossless raw archives. No attempt or outlier is removed.
This focused control suite is separate from full end-to-end qualification.
"""
import argparse
from datetime import datetime, timezone
import csv
import json
from pathlib import Path
import platform
import shutil
import statistics
import subprocess
import sys
import tempfile
from types import SimpleNamespace

sys.dont_write_bytecode = True
import run_end_to_end as qualification
from recorded_connection_study import build_snapshot, cache_values

ipc = qualification.ipc
ROOT = Path(__file__).resolve().parents[1]
SUITE = "public-runtime-controls"
PROFILES = ("public-recorded", "recorded-connection", "public-acknowledged",
            "recorded-acknowledged-connection", "nanomsg")
INPUTS = ("benchmarks/public_runtime_controls.py",
          ".github/workflows/public-runtime-controls.yml",
          "test/test_public_runtime_controls.py")


def plan():
    # Match the full suite's core64 case exactly: 20,480 total publications,
    # including 4,096 warmup, so each quantile covers 16,384 measured samples.
    cases = {"saturation-64": qualification.plan(False, "public")[0]["saturation-64"]}
    attempts = []
    for rotation in range(len(PROFILES)):
        for position, profile in enumerate(PROFILES[rotation:] + PROFILES[:rotation], 1):
            attempts.append(dict(attempt=len(attempts) + 1, case="saturation-64",
                                 profile=profile, round=rotation + 1, position=position))
    return cases, attempts


def frozen_source(commit):
    snapshot = qualification.frozen_source(commit)
    for name in INPUTS:
        content = subprocess.check_output(["git", "show", commit + ":" + name], cwd=ROOT)
        ipc.check((ROOT / name).read_bytes() == content, "Control inputs differ from frozen source: " + name)
        snapshot["sha256"][name] = ipc.sha256(ROOT / name)
    return snapshot


def reconstruct(output):
    output = Path(output)
    manifest = qualification.load_json((output / "manifest.json").read_text())
    ipc.check(manifest["format"] == 1 and manifest["suite"] == SUITE and manifest["complete"] is True,
              "Incomplete or different control suite; failures remain retained")
    cases, attempts = plan()
    ipc.check(manifest["cases"] == cases and manifest["attempts"] == attempts, "Control order or case changed")
    artifacts = {"raw/attempt-%04d.tar.gz" % request["attempt"] for request in attempts}
    ipc.check(set(manifest["raw_sha256"]) == artifacts, "Missing or extra control archives")
    published = [qualification.load_json(line) for line in (output / "reports.jsonl").read_text().splitlines()]
    ipc.check(len(published) == len(attempts), "Missing or extra control reports")
    derived = []
    for request, report in zip(attempts, published):
        artifact = "raw/attempt-%04d.tar.gz" % request["attempt"]
        path = output / artifact
        ipc.check(report["artifact"] == artifact and not path.is_symlink() and
                  ipc.sha256(path) == manifest["raw_sha256"][artifact], "Control archive identity/hash mismatch")
        with tempfile.TemporaryDirectory(prefix="angaria-controls-verify-") as temporary:
            directory = Path(temporary)
            qualification.extract(path, directory)
            result = dict(request, artifact=artifact, **qualification.validate_evidence(
                directory, request["profile"], cases[request["case"]],
                qualification.load_json((directory / "evidence.json").read_text()),
                manifest["source"]["version"], manifest["dependency"]))
        ipc.check(json.dumps(result, sort_keys=True, allow_nan=False) ==
                  json.dumps(report, sort_keys=True, allow_nan=False),
                  "Control metrics differ from retained evidence: " + artifact)
        derived.append(result)
    return derived, manifest


def summarize(reports):
    _, attempts = plan()
    ipc.check(len(reports) == len(attempts) and all(
        all(report.get(key) == value for key, value in request.items()) and report.get("status") == "complete"
        for request, report in zip(attempts, reports)), "Summaries require every successful planned attempt")

    def distribution(values):
        return dict(median=statistics.median(values), minimum=min(values), maximum=max(values))

    result = {}
    for profile in PROFILES:
        selected = [report for report in reports if report["profile"] == profile]
        measures = dict(
            completed_per_second=[row["rates"]["measured_completed_per_second"] for row in selected],
            delivery_p50_ns=[row["metrics"]["logical_start_to_delivery_ns"]["p50_ns"] for row in selected],
            delivery_p99_ns=[row["metrics"]["logical_start_to_delivery_ns"]["p99_ns"] for row in selected],
            producer_path_p50_ns=[row["metrics"]["logical_producer_path_ns"]["p50_ns"] for row in selected],
            producer_path_p99_ns=[row["metrics"]["logical_producer_path_ns"]["p99_ns"] for row in selected],
            inclusive_cpu_ns_per_publication=[row["resources"]["totals"]["inclusive_cpu_ns_per_publication"]
                                              for row in selected],
            producer_backpressure_count=[row["counters"]["producer"]["measured_backpressure_count"]
                                         for row in selected])
        if "logical_start_to_acknowledgment_ns" in selected[0]["metrics"]:
            for quantile in ("p50", "p99"):
                measures["acknowledgment_" + quantile + "_ns"] = [
                    row["metrics"]["logical_start_to_acknowledgment_ns"][quantile + "_ns"] for row in selected]
        result[profile] = dict(runs=len(selected), metrics={key: distribution(values) for key, values in measures.items()})
    return dict(suite=SUITE, method="Median and full range of per-run results; samples are not pooled",
                profiles=result)


def write_summary(output, reports):
    summary = summarize(reports)
    qualification.write_json(output / "summary.json", summary)
    with (output / "comparison.csv").open("w", newline="") as stream:
        writer = csv.writer(stream)
        writer.writerow(("profile", "runs", "metric", "median", "minimum", "maximum"))
        for profile, record in summary["profiles"].items():
            for metric, values in record["metrics"].items():
                writer.writerow((profile, record["runs"], metric, values["median"], values["minimum"], values["maximum"]))


def collect(options):
    executable, output = options.executable.resolve(strict=True), options.output.resolve()
    ipc.check(platform.system() == "Linux" and platform.machine() == "x86_64", "Controls require native Linux x86_64")
    ipc.check(not output.exists(), "Use a fresh output directory; earlier attempts must be retained")
    output.mkdir(parents=True)
    (output / "raw").mkdir()
    cases, attempts = plan()
    manifest = dict(format=1, suite=SUITE, complete=False, cases=cases, attempts=attempts,
                    raw_sha256={}, failures=[], host_label=options.host_label,
                    methodology="Five cyclic rotations of five profiles, one identical finite core64 case; no discarded attempts")
    qualification.write_json(output / "manifest.json", manifest)
    try:
        source = frozen_source(options.source_commit)
        files, build = build_snapshot(executable, output)
        settings = cache_values(executable.parent / "CMakeCache.txt")
        for key in ("ANGARIA_BUILD_IPC_COMPARISON", "ANGARIA_BUILD_BENCHMARKS"):
            ipc.check(settings.get(key, "").upper() in ("ON", "TRUE", "YES", "1"), "Enable " + key)
        host, _, _ = ipc.comparison_metadata(executable, SimpleNamespace(
            host_label=options.host_label, smoke=False, timeout=options.timeout,
            repetitions=5, angaria_profile=PROFILES[0]))
        manifest.update(source=source, build=build, host=host, dependency=ipc.pinned_dependency())
        qualification.write_json(output / "manifest.json", manifest)
    except Exception as error:
        manifest["failures"].append(dict(phase="preflight", failure=f"{type(error).__name__}: {error}"))
        qualification.write_json(output / "manifest.json", manifest)
        raise
    with (output / "reports.jsonl").open("w") as stream:
        for request in attempts:
            directory = output / "attempt"
            directory.mkdir()
            artifact = "raw/attempt-%04d.tar.gz" % request["attempt"]
            print(f"{request['attempt']}/{len(attempts)} {request['profile']}", flush=True)
            try:
                result = qualification.execute(executable, request["profile"], cases[request["case"]], directory,
                                               source["version"], options.timeout, options.start_lead_ms)
            except Exception as error:
                result = dict(status="failed", failure=f"{type(error).__name__}: {error}")
                manifest["failures"].append(dict(request, **result))
                qualification.write_json(directory / "failure.json", result)
            qualification.archive(directory, output / artifact)
            manifest["raw_sha256"][artifact] = ipc.sha256(output / artifact)
            shutil.rmtree(directory)
            stream.write(json.dumps(dict(request, artifact=artifact, **result), sort_keys=True, allow_nan=False) + "\n")
            stream.flush()
            qualification.write_json(output / "manifest.json", manifest)
    ipc.check(frozen_source(options.source_commit) == source, "Source changed during controls")
    ipc.check({name: ipc.sha256(path) for name, path in files.items()} == build["sha256"], "Build changed during controls")
    manifest["completed_utc"] = datetime.now(timezone.utc).isoformat()
    manifest["complete"] = not manifest["failures"]
    qualification.write_json(output / "manifest.json", manifest)
    ipc.check(manifest["complete"], "Control attempts failed; all raw evidence retained")
    reports, _ = reconstruct(output)
    write_summary(output, reports)
    print(f"Reconstructed all {len(reports)} control attempts from raw evidence: {output}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest="action", required=True)
    collecting = sub.add_parser("collect")
    collecting.add_argument("executable", type=Path)
    collecting.add_argument("--output", type=Path, required=True)
    collecting.add_argument("--source-commit", required=True)
    collecting.add_argument("--host-label", required=True)
    collecting.add_argument("--timeout", type=int, default=90)
    collecting.add_argument("--start-lead-ms", type=int, default=100)
    sub.add_parser("verify").add_argument("output", type=Path)
    options = parser.parse_args()
    if options.action == "collect":
        ipc.check(1 <= options.timeout <= 600 and 1 <= options.start_lead_ms <= 5000, "Invalid timing bounds")
        collect(options)
    else:
        reports, _ = reconstruct(options.output)
        ipc.check(qualification.load_json((options.output / "summary.json").read_text()) == summarize(reports),
                  "Published summary differs from raw evidence")
        print(f"Verified {len(reports)} control attempts from retained raw evidence")


if __name__ == "__main__":
    try:
        main()
    except Exception as error:
        print(f"Public runtime controls failed: {error}", file=sys.stderr)
        sys.exit(1)
