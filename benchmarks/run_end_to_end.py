#!/usr/bin/env python3
"""Collect/reconstruct finite warm-stream qualification, retaining every attempt.

One process pair per attempt, no clock correction or outlier removal. Binary
samples and bounded control streams are losslessly archived after validation;
the large reserved transport files are deliberately not benchmark evidence.
"""
import argparse
from contextlib import contextmanager
from datetime import datetime, timezone
import itertools
import json
from pathlib import Path
import shutil
import subprocess
import sys
import tarfile
import tempfile
import time
from types import SimpleNamespace

sys.dont_write_bytecode = True
import run_ipc_comparison as ipc
from end_to_end_validation import validate_attempt, validate_case
from recorded_connection_study import build_snapshot, cache_values, source_snapshot

ROOT = Path(__file__).resolve().parents[1]
PROFILES = ("recorded-connection", "recorded-acknowledged-connection", "nanomsg")
CONTROLS = (*PROFILES, "angaria", "recorded-spsc")
FLAGS = ("qualification", "warmup_messages", "processing_delay_ns", "pause_at", "pause_ns", "pacing_spin")
SCRIPT_INPUTS = ("benchmarks/run_end_to_end.py", "benchmarks/end_to_end_validation.py",
                 "benchmarks/end_to_end_report.py", "benchmarks/recorded_connection_study.py",
                 "benchmarks/producer_log_append_study.py", "benchmarks/run_ipc_comparison.py",
                 "benchmarks/run_native.py")


def write_json(path, value):
    path.write_text(json.dumps(value, sort_keys=True, indent=2, allow_nan=False) + "\n")


def load_json(text):
    def pairs(items):
        result = {}
        for key, value in items:
            ipc.check(key not in result, "Duplicate JSON key: " + key)
            result[key] = value
        return result
    return json.loads(text, object_pairs_hook=pairs,
                      parse_constant=lambda value: ipc.check(False, "Nonfinite JSON: " + value))


def plan(smoke=False):
    """Balanced orders are fixed before measurements; counts never pooled."""
    base = dict(messages=20480, warmup_messages=4096, history=8192, payload_bytes=64,
                interval_ns=0, socket_buffer_bytes=131072, budget_ms=60000, ring_slots=8,
                qualification=1, processing_delay_ns=0, pause_at=0, pause_ns=0,
                pacing_spin=1, scenario="concurrent", collection="none")
    definitions = [("saturation-64", {}), ("saturation-256", dict(payload_bytes=256)),
                   ("saturation-1024", dict(payload_bytes=1024)),
                   ("paced-50k", dict(interval_ns=20000)), ("paced-100k", dict(interval_ns=10000)),
                   ("history-0", dict(history=0)), ("history-65536", dict(history=65536)),
                   ("credit-1", dict(ring_slots=1)), ("credit-64", dict(ring_slots=64)),
                   ("slow-consumer", dict(processing_delay_ns=50000)),
                   ("paused-consumer", dict(pause_at=4096, pause_ns=20000000))]
    if smoke:
        base.update(messages=40, warmup_messages=8, history=16)
        definitions = [("saturation-64", {}), ("paced-50k", dict(interval_ns=20000)),
                       ("paused-consumer", dict(pause_at=8, pause_ns=2000000)),
                       ("slow-consumer", dict(processing_delay_ns=50000))]
    cases, orders = {}, []
    for name, changes in definitions:
        cases[name] = dict(base, **changes)
        permutations = [PROFILES] if smoke else list(itertools.permutations(PROFILES))
        orders.extend((name, list(order)) for order in permutations)
    if not smoke:
        cases["long-64"] = dict(base, messages=139264, warmup_messages=8192)
        orders.extend(("long-64", list(PROFILES[i:] + PROFILES[:i])) for i in range(3))
    for history in (0, 128):
        name = "short-control-H" + str(history)
        cases[name] = dict(base, messages=16, warmup_messages=0, history=history)
        rotations = range(1) if smoke else range(len(CONTROLS))
        orders.extend((name, list(CONTROLS[i:] + CONTROLS[:i])) for i in rotations)
    attempts = []
    for block, (name, order) in enumerate(orders, 1):
        for position, profile in enumerate(order, 1):
            attempts.append(dict(attempt=len(attempts) + 1, case=name, profile=profile,
                                 round=block, position=position))
    for case in cases.values():
        validate_case(case)
    return cases, attempts


def arguments(executable, profile, role, session, case):
    result = ipc.command(executable, profile, role, session, case)
    for key in FLAGS:
        result += ["--" + key.replace("_", "-"), str(case[key])]
    return result


def frozen_source(commit):
    snapshot = source_snapshot(commit)
    for name in SCRIPT_INPUTS:
        content = subprocess.check_output(["git", "show", commit + ":" + name], cwd=ROOT)
        ipc.check((ROOT / name).read_bytes() == content, "Qualification scripts differ from frozen source: " + name)
        snapshot["sha256"][name] = ipc.sha256(ROOT / name)
    snapshot["version"] = (ROOT / "VERSION").read_text().strip()
    return snapshot


@contextmanager
def retained_session(directory, stop_workers):
    with tempfile.TemporaryDirectory(prefix="ae2e-", dir="/tmp") as temporary:
        session = Path(temporary) / "s"
        try:
            yield session
        finally:
            # Stop processes before removing their live transport or examining
            # their sidecars; error paths use the same ordering as success.
            stop_workers()
            # Even incomplete/error runs retain any sidecar a worker managed to
            # produce. The reserved log itself is intentionally not archived.
            for role in ipc.ROLES:
                source = session / (role + ".samples.bin")
                if source.is_file():
                    shutil.copyfile(source, directory / source.name)


def execute(executable, profile, case, directory, version, timeout, start_lead_ms):
    deadline = time.monotonic() + timeout
    setup_workers, workers = ipc.Workers(directory, deadline, "setup-control.jsonl"), None
    def close_workers():
        nonlocal setup_workers, workers
        errors = []
        for group in (setup_workers, workers):
            if group is not None:
                errors.extend(group.close())
        setup_workers = workers = None
        ipc.check(not errors, "Worker cleanup errors: " + repr(errors))
    try:
        with retained_session(directory, close_workers) as session:
            setup_workers.start("setup", arguments(executable, profile, "setup", session, case))
            setup = setup_workers.events({"setup": "SETUP"}, profile)["setup"][0]
            ipc.common_identity(setup, "setup", profile, case, version)
            ipc.check(setup["history_count"] == case["history"], "History setup mismatch")
            ipc.check(setup["pid"] == setup_workers.workers["setup"]["process"].pid, "Setup PID changed")
            setup_workers.exited()
            setup_workers.close()
            setup_workers = None
            workers = ipc.Workers(directory, deadline)
            for role in ipc.ROLES:
                workers.start(role, arguments(executable, profile, role, session, case))
            workers.events({role: "CONNECTED" for role in ipc.ROLES}, profile)
            probes = []
            for nonce in range(1, 4):
                for role in ipc.ROLES:
                    before, _ = workers.send(role, f"CLOCK {nonce}")
                    event, received = workers.events({role: "CLOCK"}, profile)[role]
                    resolution = ipc.integer(event.get("clock_resolution_ns"), "clock resolution", 1)
                    ipc.check(event.get("nonce") == nonce and event.get("clock_id") == "CLOCK_MONOTONIC" and
                              event.get("pid") == workers.workers[role]["process"].pid, "Clock probe identity changed")
                    ipc.check(before - resolution <= event["clock_ns"] <= received + resolution,
                              "Clock epochs differ across participants")
                    probes.append(dict(event, before_ns=before, received_ns=received))
            for barrier, expected in (("PRELOAD", "DRAINED"), ("READY", "READY")):
                for role in ipc.ROLES:
                    workers.send(role, barrier)
                events = workers.events({role: expected for role in ipc.ROLES}, profile)
                if barrier == "PRELOAD":
                    ipc.check(all(e[0].get("history_count") == case["history"] for e in events.values()), "Preload not drained")
            t0 = ipc.clock_ns() + start_lead_ms * 1000000
            for role in ipc.ROLES:
                workers.send(role, f"START {t0}")
            workers.events({role: "DONE" for role in ipc.ROLES}, profile)
            for role in ipc.ROLES:
                workers.send(role, "FINISH")
                workers.workers[role]["process"].stdin.close()
            packets = workers.events({role: "REPORT" for role in ipc.ROLES}, profile)
            reports = {role: packet[0] for role, packet in packets.items()}
            for role, report in reports.items():
                ipc.check(report["pid"] == workers.workers[role]["process"].pid, "REPORT PID changed")
                sample = session / (role + ".samples.bin")
                if sample.exists():
                    shutil.copyfile(sample, directory / sample.name)
            workers.exited({role: 0 if report.get("status") == "complete" else 1 for role, report in reports.items()})
            evidence = dict(t0_ns=t0, setup=setup, clock_probes=probes, reports=reports,
                            dependency=ipc.pinned_dependency())
            write_json(directory / "evidence.json", evidence)
            return validate_evidence(directory, profile, case, evidence, version)
    finally:
        close_workers()


def validate_evidence(directory, profile, case, evidence, version, dependency=None):
    """Derive summaries from archived bytes, not published aggregates."""
    setup, reports = evidence["setup"], evidence["reports"]
    dependency = ipc.pinned_dependency() if dependency is None else dependency
    ipc.check(evidence["dependency"] == dependency, "Retained dependency pin changed")
    ipc.common_identity(setup, "setup", profile, case, version, dependency)
    ipc.check(setup["history_count"] == case["history"], "Setup history mismatch")
    t0 = evidence["t0_ns"]
    events = [load_json(line) for line in (directory / "control.jsonl").read_text().splitlines()]
    for role in ("setup", *ipc.ROLES):
        argv = load_json((directory / (role + "-command.json")).read_text())
        ipc.check(len(argv) % 2 == 1 and len(set(argv[1::2])) == len(argv[1::2]), "Duplicate/malformed worker arguments")
        switches = dict(zip(argv[1::2], argv[2::2]))
        ipc.check(switches["--profile"] == profile and switches["--role"] == role, "Command identity changed")
        for key in (*ipc.DIMENSIONS, "scenario", "collection", "ring_slots", *FLAGS):
            ipc.check(switches.get("--" + key.replace("_", "-")) == str(case[key]), "Command changed " + key)
        raw = [load_json(line) for line in (directory / (role + ".stdout")).read_text().splitlines()]
        if role == "setup":
            ipc.check(raw == [setup], "Setup stdout differs from retained evidence")
            continue
        archived = [event for event in raw if event.get("event") == "REPORT"]
        ipc.check(archived == [reports[role]], "Evidence REPORT differs from retained stdout")
        expected_events = ["CONNECTED", "CLOCK", "CLOCK", "CLOCK", "DRAINED", "READY", "DONE", "REPORT"]
        ipc.check([event["event"] for event in raw] == expected_events, "Worker barriers are missing or reordered")
        own = [row for row in events if row.get("role") == role]
        sends = [row for row in own if row.get("direction") == "send"]
        receives = [row for row in own if row.get("direction") == "receive"]
        ipc.check([row["event"] for row in receives] == expected_events, "Control log differs from stdout")
        ipc.check([row["command"] for row in sends] ==
                  ["CLOCK 1", "CLOCK 2", "CLOCK 3", "PRELOAD", "READY", f"START {t0}", "FINISH"],
                  "Control commands missing or reordered")
        ipc.check(all(row["before_ns"] <= row["after_ns"] for row in sends) and
                  all(a["received_ns"] <= b["received_ns"] for a, b in zip(receives, receives[1:])),
                  "Control timestamps decreased")
        for sent, previous, following in zip(sends, receives, receives[1:]):
            ipc.check(previous["received_ns"] <= sent["before_ns"] <= sent["after_ns"] <= following["received_ns"],
                      "Barrier command lies outside its event brackets")
        starts = [row for row in events if row.get("direction") == "send" and row.get("role") == role and
                  row.get("command", "").startswith("START ")]
        ipc.check(len(starts) == 1 and starts[0]["command"] == f"START {t0}" and starts[0]["after_ns"] < t0,
                  "START is missing, differs, or arrived after T0")
        finish = [row for row in events if row.get("direction") == "send" and row.get("role") == role and row.get("command") == "FINISH"]
        receipt = [row for row in events if row.get("direction") == "receive" and row.get("role") == role and row.get("event") == "REPORT"]
        ipc.check(len(finish) == len(receipt) == 1 and
                  finish[0]["before_ns"] <= reports[role]["resource_end_ns"] <= receipt[0]["received_ns"],
                  "Resource tail does not end at FINISH")
        for key in ipc.DEPENDENCY_FIELDS:
            ipc.check(reports[role][key] == setup[key], "Dependency identity differs across roles")
        probes = [probe for probe in evidence["clock_probes"] if probe["role"] == role]
        ipc.check([probe["nonce"] for probe in probes] == [1, 2, 3], "Clock probes incomplete")
        for probe in probes:
            ipc.check(probe["clock_id"] == "CLOCK_MONOTONIC" and probe["pid"] == reports[role]["pid"] and
                      probe["before_ns"] - probe["clock_resolution_ns"] <= probe["clock_ns"] <=
                      probe["received_ns"] + probe["clock_resolution_ns"], "Clock domain not bracketed")
    # Version is recorded with the frozen source; independent later analysis
    # must not accidentally substitute a newer checkout's version.
    ipc.check(all(report["framework_version"] == version for report in reports.values()), "Source version mismatch")
    return validate_attempt(directory, profile, case, t0, reports["producer"], reports["consumer"],
                            expected_version=version, dependency=dependency)


def archive(directory, path):
    with tarfile.open(path, "w:gz", compresslevel=6) as output:
        for item in sorted(directory.iterdir()):
            ipc.check(item.is_file() and not item.is_symlink(), "Unexpected raw artifact")
            output.add(item, arcname=item.name, recursive=False)


def extract(path, directory):
    with tarfile.open(path, "r:gz") as source:
        seen, total = set(), 0
        for item in source:
            ipc.check(item.isfile() and item.name == Path(item.name).name and item.name not in ("", ".", "..") and
                      item.name not in seen, "Unsafe or duplicate raw artifact")
            total += item.size
            ipc.check(total <= 100 * 1024 * 1024, "Raw attempt exceeds extraction bound")
            seen.add(item.name)
            with source.extractfile(item) as raw, (directory / item.name).open("wb") as target:
                shutil.copyfileobj(raw, target)


def reconstruct(output):
    output = Path(output)
    manifest = load_json((output / "manifest.json").read_text())
    ipc.check(manifest["format"] == 1 and manifest["complete"] is True, "Incomplete qualification; failures remain retained")
    cases, attempts = plan(manifest["smoke"])
    ipc.check(manifest["cases"] == cases and manifest["attempts"] == attempts, "Qualification matrix changed")
    published = [load_json(line) for line in (output / "reports.jsonl").read_text().splitlines()]
    ipc.check(len(published) == len(attempts), "Missing or extra published attempts")
    derived = []
    for request, report in zip(attempts, published):
        artifact = "raw/attempt-%04d.tar.gz" % request["attempt"]
        ipc.check(report["artifact"] == artifact, "Raw artifact path changed")
        path = output / artifact
        ipc.check(not path.is_symlink() and ipc.sha256(path) == manifest["raw_sha256"][artifact], "Raw archive hash mismatch")
        with tempfile.TemporaryDirectory(prefix="ae2e-verify-") as temporary:
            directory = Path(temporary)
            extract(path, directory)
            result = dict(request, artifact=artifact, **validate_evidence(directory, request["profile"], cases[request["case"]],
                          load_json((directory / "evidence.json").read_text()), manifest["source"]["version"], manifest["dependency"]))
        ipc.check(json.dumps(result, sort_keys=True, allow_nan=False) == json.dumps(report, sort_keys=True, allow_nan=False),
                  "Published metrics differ from retained evidence: " + artifact)
        derived.append(result)
    return derived, manifest


def collect(options):
    executable, output = options.executable.resolve(), options.output.resolve()
    ipc.check(not output.exists(), "Use a new output directory; prior attempts must be retained")
    output.mkdir(parents=True)
    (output / "raw").mkdir()
    source = frozen_source(options.source_commit)
    files, build = build_snapshot(executable, output)
    settings = cache_values(executable.parent / "CMakeCache.txt")
    ipc.check(settings.get("ANGARIA_BUILD_STALL_STUDY") == "ON", "Legacy experiment controls require STALL_STUDY=ON")
    metadata_options = SimpleNamespace(host_label=options.host_label, smoke=options.smoke,
                                      timeout=options.timeout, repetitions=6, angaria_profile=PROFILES[0])
    host, _, _ = ipc.comparison_metadata(executable, metadata_options)
    cases, attempts = plan(options.smoke)
    manifest = dict(format=1, complete=False, smoke=options.smoke, source=source, build=build, host=host,
                    host_label=options.host_label, dependency=ipc.pinned_dependency(),
                    cases=cases, attempts=attempts, raw_sha256={}, failures=[],
                    methodology="All permutations for core cases; cyclic rotations for long and legacy controls. No discarded attempts.")
    write_json(output / "manifest.json", manifest)
    with (output / "reports.jsonl").open("w") as stream:
        for request in attempts:
            directory = output / "attempt"
            directory.mkdir()
            artifact = "raw/attempt-%04d.tar.gz" % request["attempt"]
            print(f"{request['attempt']}/{len(attempts)} {request['case']} {request['profile']}", flush=True)
            try:
                result = execute(executable, request["profile"], cases[request["case"]], directory,
                                 source["version"], options.timeout, options.start_lead_ms)
            except Exception as error:
                result = dict(status="failed", failure=f"{type(error).__name__}: {error}")
                manifest["failures"].append(dict(request, **result))
                write_json(directory / "failure.json", result)
            archive(directory, output / artifact)
            manifest["raw_sha256"][artifact] = ipc.sha256(output / artifact)
            shutil.rmtree(directory)
            stream.write(json.dumps(dict(request, artifact=artifact, **result), sort_keys=True, allow_nan=False) + "\n")
            stream.flush()
            write_json(output / "manifest.json", manifest)
    ipc.check(frozen_source(options.source_commit) == source, "Source changed during measurements")
    ipc.check({name: ipc.sha256(path) for name, path in files.items()} == build["sha256"], "Build changed during measurements")
    manifest["completed_utc"] = datetime.now(timezone.utc).isoformat()
    manifest["complete"] = not manifest["failures"]
    write_json(output / "manifest.json", manifest)
    ipc.check(manifest["complete"], "Qualification failed; all attempts retained, no complete comparison")
    reports, manifest = reconstruct(output)
    if not options.no_plots:
        from end_to_end_report import render
        render(output, reports, manifest)
    print(f"Reconstructed {len(reports)} successful attempts from raw evidence: {output}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest="action", required=True)
    collecting = sub.add_parser("collect")
    collecting.add_argument("executable", type=Path)
    collecting.add_argument("--output", type=Path, required=True)
    collecting.add_argument("--source-commit", required=True)
    collecting.add_argument("--host-label", required=True)
    collecting.add_argument("--smoke", action="store_true")
    collecting.add_argument("--no-plots", action="store_true")
    collecting.add_argument("--timeout", type=int, default=90)
    collecting.add_argument("--start-lead-ms", type=int, default=100)
    for name in ("verify", "render"):
        sub.add_parser(name).add_argument("output", type=Path)
    options = parser.parse_args()
    if options.action == "collect":
        ipc.check(1 <= options.start_lead_ms <= 5000 and 1 <= options.timeout <= 600, "Invalid coordinator timing bounds")
        collect(options)
    else:
        reports, manifest = reconstruct(options.output)
        if options.action == "render":
            from end_to_end_report import render
            render(options.output, reports, manifest)
        print(f"Verified {len(reports)} attempts from retained raw evidence")


if __name__ == "__main__":
    try:
        main()
    except Exception as error:
        print(f"Qualification failed: {error}", file=sys.stderr)
        sys.exit(1)
