#!/usr/bin/env python3
"""Retain paired BASE/HEAD public messaging measurements on one machine.

Six rounds rotate the three profiles and alternate adjacent BASE/HEAD order.
Every revision/profile therefore occupies every order position once. Both
checkouts are built before collection; raw samples validate each binary's own
framework version. The ordinary full HEAD suite remains a separate artifact.
"""
import argparse
import csv
from datetime import datetime, timezone
import io
import json
from pathlib import Path
import shutil
import statistics
import subprocess
import sys
import tempfile

sys.dont_write_bytecode = True
import run_end_to_end as qualification
import end_to_end_report as report

ipc = qualification.ipc
ROOT = Path(__file__).resolve().parents[1]
SUITE = "paired-public-ownership"
REVISIONS = ("base", "head")
PROFILES = qualification.PUBLIC_PROFILES
INPUTS = ("benchmarks/ownership_performance.py", "test/test_ownership_performance.py",
          ".github/workflows/ownership-performance.yml")

# Run the frozen-source and build helpers from EACH checkout. Mutating imported
# module ROOT globals would accidentally authenticate BASE with HEAD's inputs.
PREFLIGHT = r'''
import json, pathlib, sys
from types import SimpleNamespace
root, executable, output = map(pathlib.Path, sys.argv[2:5])
sys.path.insert(0, str(root / "benchmarks"))
import run_end_to_end as q
from recorded_connection_study import build_snapshot, cache_values
source = q.frozen_source(sys.argv[5])
result = dict(source=source)
if sys.argv[1] == "preflight":
    files, build = build_snapshot(executable, output)
    settings = cache_values(executable.parent / "CMakeCache.txt")
    for key in ("ANGARIA_BUILD_IPC_COMPARISON", "ANGARIA_BUILD_BENCHMARKS"):
        q.ipc.check(settings.get(key, "").upper() in ("ON", "TRUE", "YES", "1"), "Enable " + key)
    host, _, _ = q.ipc.comparison_metadata(executable, SimpleNamespace(
        host_label=sys.argv[6], smoke=False, timeout=int(sys.argv[7]), repetitions=6,
        angaria_profile="public-recorded"))
    keys = ("CMAKE_BUILD_TYPE", "CMAKE_CXX_COMPILER", "CMAKE_CXX_FLAGS", "CMAKE_CXX_FLAGS_RELEASE",
            "CMAKE_C_COMPILER", "CMAKE_C_FLAGS", "CMAKE_C_FLAGS_RELEASE")
    result.update(build=build, host=host, dependency=q.ipc.pinned_dependency(),
                  files={name: str(path) for name, path in files.items()},
                  compiler_settings={key: settings.get(key, "") for key in keys})
print(json.dumps(result, sort_keys=True, allow_nan=False))
'''


def plan():
    cases = {"saturation-64": qualification.plan(False, "public")[0]["saturation-64"]}
    attempts = []
    for round_index in range(6):
        rotation = round_index % len(PROFILES)
        profiles = PROFILES[rotation:] + PROFILES[:rotation]
        revisions = REVISIONS if round_index % 2 == 0 else REVISIONS[::-1]
        for profile_position, profile in enumerate(profiles):
            for revision in revisions:
                attempts.append(dict(attempt=len(attempts) + 1, case="saturation-64", profile=profile,
                                     revision=revision, round=round_index + 1,
                                     pair=round_index * len(PROFILES) + profile_position + 1,
                                     position=(len(attempts) % 6) + 1))
    return cases, attempts


def runner_source(commit):
    source = qualification.frozen_source(commit)
    for name in INPUTS:
        frozen = subprocess.check_output(["git", "show", commit + ":" + name], cwd=ROOT)
        ipc.check((ROOT / name).read_bytes() == frozen, "Paired study input differs from frozen HEAD: " + name)
        source["sha256"][name] = ipc.sha256(ROOT / name)
    return source


def snapshot(root, executable, directory, commit, options, phase):
    command = [sys.executable, "-B", "-c", PREFLIGHT, phase, str(root), str(executable), str(directory),
               commit, options.host_label, str(options.timeout)]
    process = subprocess.run(command, cwd=root, capture_output=True, text=True, timeout=90)
    if phase == "preflight":
        (directory / "preflight.stdout.txt").write_text(process.stdout)
        (directory / "preflight.stderr.txt").write_text(process.stderr)
    ipc.check(process.returncode == 0, "Frozen checkout preflight failed: " + process.stderr)
    return qualification.load_json(process.stdout)


def reconstruct(output):
    output = Path(output)
    manifest = qualification.load_json((output / "manifest.json").read_text())
    ipc.check(manifest["format"] == 1 and manifest["suite"] == SUITE and manifest["complete"] is True,
              "Incomplete or different paired study; all failures remain retained")
    cases, attempts = plan()
    ipc.check(manifest["cases"] == cases and manifest["attempts"] == attempts, "Paired study case/order changed")
    expected = {"raw/attempt-%04d.tar.gz" % item["attempt"] for item in attempts}
    ipc.check(set(manifest["raw_sha256"]) == expected, "Missing or extra paired archives")
    published = [qualification.load_json(line) for line in (output / "reports.jsonl").read_text().splitlines()]
    ipc.check(len(published) == len(attempts), "Missing or extra paired reports")
    derived = []
    for request, retained in zip(attempts, published):
        artifact = "raw/attempt-%04d.tar.gz" % request["attempt"]
        archive = output / artifact
        ipc.check(retained["artifact"] == artifact and not archive.is_symlink() and
                  ipc.sha256(archive) == manifest["raw_sha256"][artifact], "Paired raw archive identity/hash mismatch")
        with tempfile.TemporaryDirectory(prefix="angaria-paired-verify-") as temporary:
            directory = Path(temporary)
            qualification.extract(archive, directory)
            result = dict(request, artifact=artifact, **qualification.validate_evidence(
                directory, request["profile"], cases[request["case"]],
                qualification.load_json((directory / "evidence.json").read_text()),
                manifest["revisions"][request["revision"]]["source"]["version"], manifest["dependency"]))
        ipc.check(json.dumps(result, sort_keys=True, allow_nan=False) ==
                  json.dumps(retained, sort_keys=True, allow_nan=False),
                  "Paired metrics differ from raw evidence: " + artifact)
        derived.append(result)
    return derived, manifest


def summaries(reports, manifest):
    cases, attempts = plan()
    ipc.check(len(reports) == len(attempts) and all(
        all(row.get(key) == value for key, value in request.items()) and row.get("status") == "complete"
        for request, row in zip(attempts, reports)), "Summary requires every successful planned BASE/HEAD pair")
    rows = []
    for revision in REVISIONS:
        selected = [row for row in reports if row["revision"] == revision]
        rows.extend(dict(row, revision=revision) for row in report.summaries(selected, dict(cases=cases)))
    paired = []
    metrics = ("completed_rate", "delivery_p50", "delivery_p99", "producer_p50", "cpu_inclusive", "ack_p50", "ack_p99")
    for profile in PROFILES:
        for metric in metrics:
            values = []
            for round_number in range(1, 7):
                pair = {row["revision"]: report.metric_values(row) for row in reports
                        if row["profile"] == profile and row["round"] == round_number}
                if metric not in pair["base"]:
                    ipc.check(metric not in pair["head"], "Metric exists for only one revision")
                    continue
                base, head = (pair[revision][metric][0] for revision in REVISIONS)
                values.append(dict(round=round_number, base=base, head=head, difference=head - base,
                                   head_over_base=head / base if base else None))
            if values:
                differences = [value["difference"] for value in values]
                ratios = [value["head_over_base"] for value in values]
                paired.append(dict(profile=profile, metric=metric, pairs=values,
                                   median_difference=statistics.median(differences),
                                   minimum_difference=min(differences), maximum_difference=max(differences),
                                   median_head_over_base=statistics.median(ratios) if None not in ratios else None))
    return dict(suite=SUITE, attempts=len(reports), rows=rows, paired=paired,
                interpretation="Per-run quantiles and same-round paired differences; no pooled samples or timing exclusions")


def comparison_text(summary):
    stream = io.StringIO(newline="")
    fields = ("revision", "case", "profile", "metric", "runs", "samples_per_attempt", "median", "minimum", "maximum", "attempt_values")
    writer = csv.DictWriter(stream, fieldnames=fields, lineterminator="\n")
    writer.writeheader()
    for row in summary["rows"]:
        writer.writerow(dict(row, attempt_values=json.dumps(row["attempt_values"], separators=(",", ":"))))
    return stream.getvalue()


def verify_summary(output, reports, manifest, required=False):
    summary = summaries(reports, manifest)
    for name, expected in (("summary.json", json.dumps(summary, sort_keys=True, indent=2, allow_nan=False) + "\n"),
                           ("comparison.csv", comparison_text(summary))):
        path = output / name
        ipc.check(not required or path.is_file(), "Missing paired summary: " + name)
        if path.exists():
            ipc.check(path.read_text() == expected, "Published paired summary differs from raw evidence: " + name)
    return summary


def render(output, reports, manifest, plots=True):
    summary = verify_summary(output, reports, manifest)
    qualification.write_json(output / "summary.json", summary)
    (output / "comparison.csv").write_text(comparison_text(summary))
    if not plots:
        return
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt
    matplotlib.rcParams["svg.hashsalt"] = SUITE
    panels = (("completed_rate", "Completed messages / second", 1000000, "Million messages / second"),
              ("delivery_p50", "Logical start to delivery p50", 1000, "Microseconds"),
              ("delivery_p99", "Logical start to delivery p99", 1000, "Microseconds"),
              ("cpu_inclusive", "Producer + consumer CPU through FINISH", 1000, "Microseconds / message"))
    indexed = {(row["revision"], row["profile"], row["metric"]): row for row in summary["rows"]}
    figure, axes = plt.subplots(2, 2, figsize=(12, 9))
    colors = dict(base="#777777", head="#168469")
    for axis, (metric, title, scale, unit) in zip(axes.flat, panels):
        for revision, shift in (("base", -.13), ("head", .13)):
            for position, profile in enumerate(PROFILES):
                row = indexed[revision, profile, metric]
                values = [value / scale for value in row["attempt_values"]]
                center = row["median"] / scale
                axis.scatter([position + shift + (i - 2.5) * .012 for i in range(6)], values,
                             color=colors[revision], alpha=.5, s=22)
                axis.errorbar(position + shift, center,
                              yerr=[[center - min(values)], [max(values) - center]],
                              color=colors[revision], marker="o", capsize=5,
                              label=revision.upper() if position == 0 else None)
        axis.set_title(title)
        axis.set_ylabel(unit)
        axis.set_xticks(range(3), ("Public unacknowledged", "Public acknowledged", "nanomsg"))
        axis.set_ylim(bottom=0)
        axis.grid(axis="y", alpha=.2)
    versions = " -> ".join(manifest["revisions"][revision]["source"]["version"] for revision in REVISIONS)
    figure.suptitle("Paired BASE / HEAD public messaging: " + versions + "\n" + manifest["host_label"], fontsize=13)
    handles, labels = axes[0, 0].get_legend_handles_labels()
    figure.legend(handles, labels, loc="upper center", bbox_to_anchor=(.5, .92), ncol=2, frameon=False)
    figure.text(.5, .025,
                "Dots: all six runs per revision/profile; center: median; whiskers: full range. Adjacent BASE/HEAD pairs alternate order.\n"
                "64-byte messages; 16,384 measured + 4,096 warmup; history H=8,192; Angaria credit K=8. No discarded attempts.\n"
                "Angaria retains replay; nanomsg is unrecorded/unacknowledged. Finite cohorts, not an endurance test.",
                ha="center", fontsize=9, linespacing=1.5)
    figure.subplots_adjust(top=.84, bottom=.17, hspace=.37, wspace=.27)
    for suffix in ("png", "svg"):
        figure.savefig(output / ("latency-throughput." + suffix), dpi=160,
                       metadata={"Software": SUITE} if suffix == "png" else {"Date": None, "Creator": SUITE})
    plt.close(figure)
    qualification.write_json(output / "figures.json", dict(figures=["latency-throughput.png"],
                                                            attempts=len(reports), matplotlib=matplotlib.__version__))


def collect(options):
    output = options.output.resolve()
    ipc.check(not output.exists(), "Choose a fresh output directory; previous attempts must be retained")
    output.mkdir(parents=True)
    (output / "raw").mkdir()
    cases, attempts = plan()
    manifest = dict(format=1, suite=SUITE, complete=False, cases=cases, attempts=attempts,
                    host_label=options.host_label, revisions={}, failures=[], raw_sha256={},
                    methodology="Six rotated rounds, adjacent BASE/HEAD pairs alternating AB/BA, no timing exclusions")
    qualification.write_json(output / "manifest.json", manifest)
    roots = dict(base=options.base_root.resolve(), head=ROOT)
    binaries = {revision: getattr(options, revision + "_executable").resolve(strict=True) for revision in REVISIONS}
    commits = {revision: getattr(options, revision + "_commit") for revision in REVISIONS}
    try:
        runner = runner_source(options.head_commit)
        manifest["runner_source"] = runner
        for revision in REVISIONS:
            directory = output / revision
            directory.mkdir()
            manifest["revisions"][revision] = snapshot(roots[revision], binaries[revision], directory,
                                                       commits[revision], options, "preflight")
        base, head = (manifest["revisions"][revision] for revision in REVISIONS)
        ipc.check(base["source"]["commit"] != head["source"]["commit"], "BASE and HEAD must be distinct commits")
        ipc.check(base["compiler_settings"] == head["compiler_settings"], "BASE/HEAD compiler settings differ")
        ipc.check(base["dependency"] == head["dependency"] == ipc.pinned_dependency(), "BASE/HEAD nanomsg pins differ")
        for key in ("cpu", "machine", "platform", "compiler_version"):
            ipc.check(base["host"][key] == head["host"][key], "BASE/HEAD host changed: " + key)
        manifest["dependency"] = head["dependency"]
        qualification.write_json(output / "manifest.json", manifest)
    except Exception as error:
        manifest["failures"].append(dict(phase="preflight", failure=f"{type(error).__name__}: {error}"))
        qualification.write_json(output / "manifest.json", manifest)
        raise
    with (output / "reports.jsonl").open("w") as stream:
        for request in attempts:
            directory = output / "attempt"
            directory.mkdir()
            revision = request["revision"]
            artifact = "raw/attempt-%04d.tar.gz" % request["attempt"]
            print(f"{request['attempt']}/{len(attempts)} {revision} {request['profile']}", flush=True)
            try:
                result = qualification.execute(binaries[revision], request["profile"], cases[request["case"]], directory,
                    manifest["revisions"][revision]["source"]["version"], options.timeout, options.start_lead_ms)
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
    ipc.check(runner_source(options.head_commit) == runner, "HEAD collection inputs changed during measurements")
    for revision in REVISIONS:
        retained = manifest["revisions"][revision]
        ipc.check(snapshot(roots[revision], binaries[revision], output / revision, commits[revision], options, "freeze")
                  ["source"] == retained["source"], "Frozen checkout changed during measurements: " + revision)
        ipc.check({name: ipc.sha256(Path(path)) for name, path in retained["files"].items()} == retained["build"]["sha256"],
                  "Build changed during measurements: " + revision)
    manifest["complete"] = not manifest["failures"]
    manifest["completed_utc"] = datetime.now(timezone.utc).isoformat()
    qualification.write_json(output / "manifest.json", manifest)
    ipc.check(manifest["complete"], "Paired study failed; every attempt is retained")
    reports, manifest = reconstruct(output)
    render(output, reports, manifest, plots=not options.no_plots)
    print(f"Reconstructed all {len(reports)} paired attempts: {output}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest="action", required=True)
    collecting = sub.add_parser("collect")
    collecting.add_argument("--base-root", type=Path, required=True)
    for revision in REVISIONS:
        collecting.add_argument("--" + revision + "-executable", type=Path, required=True)
        collecting.add_argument("--" + revision + "-commit", required=True)
    collecting.add_argument("--output", type=Path, required=True)
    collecting.add_argument("--host-label", required=True)
    collecting.add_argument("--timeout", type=int, default=90)
    collecting.add_argument("--start-lead-ms", type=int, default=100)
    collecting.add_argument("--no-plots", action="store_true")
    for action in ("verify", "render"):
        sub.add_parser(action).add_argument("output", type=Path)
    options = parser.parse_args()
    if options.action == "collect":
        ipc.check(1 <= options.timeout <= 600 and 1 <= options.start_lead_ms <= 5000, "Invalid timing bounds")
        collect(options)
    else:
        reports, manifest = reconstruct(options.output)
        if options.action == "render":
            render(options.output, reports, manifest)
        else:
            verify_summary(options.output, reports, manifest, required=True)
        print(f"Verified {len(reports)} paired attempts from raw evidence")


if __name__ == "__main__":
    try:
        main()
    except Exception as error:
        print(f"Paired ownership qualification failed: {error}", file=sys.stderr)
        sys.exit(1)
