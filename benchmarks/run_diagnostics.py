#!/usr/bin/env python3
"""Run controlled transport diagnostics and retain every operation sample.

Summaries take medians of per-run statistics. They never pool latency samples
from different retained histories, workloads, collection modes, or outcomes.
There are no performance pass/fail thresholds.
"""
import argparse
import csv
from datetime import datetime, timezone
import gzip
import json
import os
from pathlib import Path
import platform
import plistlib
import selectors
import statistics
import subprocess
import sys
import tempfile
import time

sys.dont_write_bytecode = True
from run_native import ROOT, check, metadata, sha256


DIMENSIONS = ("workload", "collection", "messages", "history", "history_scope",
              "payload_bytes", "fields", "capacity_scale", "idle_pollers", "work_budget_ms")
PROGRESS = ("outcome", "accepted", "completed", "pending", "unacknowledged_at_stop")
SUMMARY_DIMENSIONS = ("instrumentation_enabled", "active_claims", "producer_incarnation", "consumer_incarnation") + DIMENSIONS + PROGRESS
OPERATIONS = {"issue", "send", "receive", "acknowledge", "status", "identical_retry",
              "empty_receive", "idle_empty_receive"}
RESULTS = {"issue": {"ok"}, "send": {"ok"}, "acknowledge": {"ok"},
           "receive": {"delivery", "timeout"}, "status": {"accepted"},
           "identical_retry": {"accepted"}, "empty_receive": {"timeout"}, "idle_empty_receive": {"timeout"}}
COUNTERS = {"prefix_scans", "prefix_records", "index_scans", "indexed_records", "topology_builds",
            "delivery_validations", "validated_publications", "publication_lookups", "lookup_publications",
            "task_slots", "allocations", "allocated_bytes", "empty_polls", "native_read_calls",
            "native_read_bytes", "native_map_calls", "native_mapped_bytes", "native_unmap_calls",
            "file_stat_calls", "file_grow_calls", "gate_attempts", "gate_busy", "gate_acquisitions",
            "recovery_checks", "recovery_replays", "native_gate_holds", "native_gate_hold_ns",
            "native_gate_blocking_attempts", "native_gate_nonblocking_attempts"}
PHASES = {"journal_open", "recovery", "snapshot", "prefix_scan", "record_index", "task_check",
          "task_catalog", "topology", "delivery_validation", "publication_lookup", "schema_lookup",
          "codec_build", "payload_validation", "publication_plan", "journal_preflight", "journal_body",
          "file_growth", "materialize", "commit_apply", "source_reconcile", "receive_select",
          "native_read", "native_map", "native_stat", "native_unmap", "gate_acquire", "local_gate_wait", "native_gate_wait", "other"}


def case(**overrides):
    values = dict(workload="serial", collection="none", messages=16, history=0,
                  history_scope="same", payload_bytes=8, fields=1,
                  capacity_scale=1, idle_pollers=0, work_budget_ms=5000)
    values.update(overrides)
    return values


def command(executable, directory, values):
    arguments = [str(executable), str(directory)]
    for name in DIMENSIONS:
        arguments += ["--" + name.replace("_", "-"), str(values[name])]
    return arguments


def invoke(arguments, timeout, succeeds=True):
    try:
        result = subprocess.run(arguments, stdin=subprocess.DEVNULL, capture_output=True,
                                text=True, errors="replace", timeout=timeout)
    except subprocess.TimeoutExpired as error:
        raise RuntimeError(f"Timed out: {arguments!r}\nstdout={error.stdout!r}\nstderr={error.stderr!r}") from error
    check(result.returncode == (0 if succeeds else 1),
          f"Unexpected exit {result.returncode}: {arguments!r}\nstdout={result.stdout}\nstderr={result.stderr}")
    if not succeeds:
        check(not result.stdout.strip() and bool(result.stderr.strip()),
              "Rejected arguments must report an error on stderr without a success report")
    return result.stdout


def optional(arguments):
    try:
        return subprocess.check_output(arguments, stderr=subprocess.DEVNULL, text=True,
                                       errors="replace", timeout=10).strip()
    except (OSError, subprocess.SubprocessError):
        return "unavailable"


def invoke_process(executable, directory, values, timeout):
    """Coordinate independent roles, draining both output pipes throughout."""
    deadline = time.monotonic() + timeout
    setup = json.loads(invoke(command(executable, directory, values) + ["--role", "setup"], timeout))
    check(setup.get("role") == "setup", "Missing process setup report")
    for name in DIMENSIONS:
        check(type(setup.get(name)) is type(values[name]) and setup[name] == values[name], "Process setup dimensions changed")
    check(setup.get("history_before") == setup.get("history_after") == values["history"], "Setup history changed")
    before = number(setup.get("file_bytes_before"), "setup file bytes")
    for name in ("producer_task", "consumer_task"):
        check(0 < number(setup.get(name), name) <= 65535, "Invalid setup Task")
    check(setup["producer_task"] != setup["consumer_task"], "Process roles share a Task identity")
    roles, processes = ("producer", "consumer"), {}
    buffers = {(role, pipe): bytearray() for role in roles for pipe in ("stdout", "stderr")}
    ready, done = set(), set()
    released = finished = False
    release_ns = done_ns = after_done = 0
    try:
        with selectors.DefaultSelector() as selector:
            for role in roles:
                process = subprocess.Popen(command(executable, directory, values) + ["--role", role],
                                           stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE, bufsize=0)
                processes[role] = process
                for pipe in ("stdout", "stderr"):
                    selector.register(getattr(process, pipe), selectors.EVENT_READ, (role, pipe))
            while selector.get_map():
                remaining = deadline - time.monotonic()
                check(remaining > 0, f"Process diagnostic timed out; stderr={[(r, bytes(buffers[r, 'stderr'])) for r in roles]!r}")
                events = selector.select(min(remaining, 0.1))
                for key, _ in events:
                    role, pipe = key.data
                    chunk = os.read(key.fileobj.fileno(), 65536)
                    if not chunk:
                        selector.unregister(key.fileobj)
                        continue
                    buffer = buffers[role, pipe]
                    check(len(buffer) + len(chunk) <= (32 * 1024 * 1024 if pipe == "stdout" else 1024 * 1024),
                          "Process diagnostic exceeded its bounded output storage")
                    buffer.extend(chunk)
                    if pipe == "stdout":
                        while role not in done and b"\n" in buffer:
                            pending = ready if role not in ready else done
                            expected = b"READY" if pending is ready else b"DONE"
                            line, _, rest = buffer.partition(b"\n")
                            check(line.rstrip(b"\r") == expected, f"Unexpected {role} handshake: {line!r}")
                            check(pending is ready or released, "Worker completed before its release")
                            pending.add(role)
                            buffer[:] = rest
                for role, process in processes.items():
                    status = process.poll()
                    if status is not None and status != 0:
                        # A failed worker may have unread diagnostics after its
                        # final selected event. Drain only available bytes and
                        # retain the same bound used during normal collection.
                        os.set_blocking(process.stderr.fileno(), False)
                        buffer = buffers[role, "stderr"]
                        while True:
                            try:
                                chunk = os.read(process.stderr.fileno(), min(65536, 1024 * 1024 - len(buffer) + 1))
                            except BlockingIOError:
                                break
                            if not chunk:
                                break
                            check(len(buffer) + len(chunk) <= 1024 * 1024,
                                  "Failed worker exceeded bounded stderr storage")
                            buffer.extend(chunk)
                    check(status is None or status == 0,
                          f"{role} exited {status}; stderr={bytes(buffers[role, 'stderr'])!r}")
                    check(status is None or (released and finished and role in done),
                          f"{role} exited before the required barriers")
                if len(ready) == 2 and not released:
                    release_ns = time.monotonic_ns()
                    for role, process in processes.items():
                        try:
                            process.stdin.write(b"\n")
                        except OSError as error:
                            raise RuntimeError(f"Cannot release {role}; stderr={bytes(buffers[role, 'stderr'])!r}") from error
                    released = True
                if len(done) == 2 and not finished:
                    done_ns = time.monotonic_ns()
                    after_done = (directory / "session.ang").stat().st_size
                    for role, process in processes.items():
                        try:
                            process.stdin.write(b"\n")
                            process.stdin.close()
                        except OSError as error:
                            raise RuntimeError(f"Cannot release {role} controls; stderr={bytes(buffers[role, 'stderr'])!r}") from error
                    finished = True
            check(finished, "Worker streams closed before their work completed")
            workers = {}
            for role, process in processes.items():
                try:
                    check(process.wait(timeout=max(0.01, deadline - time.monotonic())) == 0,
                          f"{role} failed after closing its output streams")
                except subprocess.TimeoutExpired as error:
                    raise RuntimeError(f"{role} did not exit before the process diagnostic deadline") from error
                worker = json.loads(buffers[role, "stdout"])
                check(worker.get("role") == role and worker.get("clock_scope") == "actor_relative", "Wrong worker report")
                for name in DIMENSIONS + ("framework_version", "instrumentation_enabled", "session_high", "session_low"):
                    check(worker.get(name) == setup.get(name), f"Worker {name} differs from setup")
                check(worker.get("worker_task") == setup.get(role + "_task") and worker.get("worker_incarnation") == 2,
                      "Worker identity/incarnation differs from its independent replacement role")
                workers[role] = worker
            after = (directory / "session.ang").stat().st_size
            check(after == after_done, "Post-work process controls changed the append length")
            producer, consumer = workers["producer"], workers["consumer"]
            accepted = number(producer.get("sent"), "producer accepted publications")
            completed = number(consumer.get("completed"), "consumer acknowledgments")
            unacknowledged = number(consumer.get("unacknowledged_at_stop"), "unfinished delivery")
            check(accepted <= values["messages"] and completed + unacknowledged <= accepted and unacknowledged <= 1 and
                  producer.get("completed") == producer.get("checksum") == 0,
                  "Producer completion accounting changed")
            for role, worker in workers.items():
                count = accepted if role == "producer" else completed
                check(worker.get("outcome") == ("complete" if count == values["messages"] else "budget_exceeded"),
                      "Worker outcome disagrees with its recorded progress")
                expected_controls = dict(status=role == "producer" and accepted > 0,
                                         identical_retry=role == "producer" and accepted > 0,
                                         empty_receive=role == "consumer" and completed == values["messages"])
                check(worker.get("controls") == expected_controls and
                      all(type(flag) is bool for flag in worker["controls"].values()), "Worker control flags changed")
            outcome = "complete" if completed == accepted == values["messages"] else "budget_exceeded"
            controls = dict(status=accepted > 0, identical_retry=accepted > 0, empty_receive=completed == values["messages"])
            return dict(values, framework_version=setup["framework_version"],
                        instrumentation_enabled=setup["instrumentation_enabled"], channels=4, ring_capacity=8,
                        history_before=values["history"], history_after=values["history"] + accepted,
                        history_bounds_exact=False, clock_scope="actor_relative", active_claims=2,
                        producer_incarnation=2, consumer_incarnation=2, file_bytes_before=before,
                        file_bytes_after_publish=after_done, file_bytes_after_drain=after, append_bytes=after - before,
                        publish_phase_ns=0, drain_phase_ns=0, cycle_phase_ns=0,
                        process_phase_ns=done_ns - release_ns, process_exit_ns=time.monotonic_ns() - release_ns,
                        outcome=outcome, accepted=accepted, completed=completed, pending=accepted - completed,
                        unacknowledged_at_stop=unacknowledged, controls=controls, checksum=consumer["checksum"],
                        worker_processes=[dict(role=role, pid=processes[role].pid, task=workers[role]["worker_task"],
                                               incarnation=workers[role]["worker_incarnation"],
                                               work_phase_ns=workers[role]["work_phase_ns"]) for role in roles],
                        worker_diagnostics={role: buffers[role, "stderr"].decode("utf-8", errors="replace") for role in roles},
                        samples=producer["samples"] + consumer["samples"], idle_pollers_summary=[])
    finally:
        for process in processes.values():
            if process.poll() is None:
                process.kill()
        for process in processes.values():
            process.wait()
            for pipe in ("stdin", "stdout", "stderr"):
                stream = getattr(process, pipe)
                if stream and not stream.closed:
                    stream.close()


def execute_case(executable, directory, values, timeout):
    if values["workload"] == "process":
        return invoke_process(executable, directory, values, timeout)
    return json.loads(invoke(command(executable, directory, values), timeout))


def experiment_metadata(executable, options):
    manifest, sources, settings = metadata(executable, options)
    for directory in ("src", "include", "benchmarks"):
        for path in (ROOT / directory).rglob("*"):
            if (path.is_file() and path.suffix in (".h", ".cpp", ".py", ".in")
                    and " 2." not in path.name and "results" not in path.parts):
                sources[str(path.relative_to(ROOT))] = sha256(path)
    temporary = tempfile.gettempdir()
    manifest.update(binary_format=optional(["file", "-b", str(executable)]),
                    session_temporary_root=temporary,
                    session_filesystem_description=optional(["df", "-P", temporary]),
                    session_filesystem_type="unavailable", suite=options.suite,
                    summary_method="median of per-run statistics within case/operation/result; samples are not pooled")
    if sys.platform.startswith("linux"):
        manifest["session_filesystem_type"] = optional(["stat", "-f", "-c", "%T", temporary])
    elif sys.platform == "darwin":
        try:
            output = subprocess.check_output(["diskutil", "info", "-plist", temporary],
                                             stderr=subprocess.DEVNULL, timeout=10)
            manifest["session_filesystem_type"] = plistlib.loads(output).get("FilesystemType", "unavailable")
        except (OSError, subprocess.SubprocessError, ValueError, plistlib.InvalidFileException):
            pass
    try:
        values = os.statvfs(temporary)
        manifest["session_filesystem_block_size"] = values.f_bsize
        manifest["session_filesystem_fragment_size"] = values.f_frsize
    except OSError:
        pass
    cache = Path(manifest["cmake_cache"])
    if cache.is_file():
        for line in cache.read_text(errors="replace").splitlines():
            if line.startswith("ANGARIA_ENABLE_DIAGNOSTICS:"):
                settings["ANGARIA_ENABLE_DIAGNOSTICS"] = line.split("=", 1)[1]
    return manifest, sources, settings


def matrix(options, enabled):
    collections = ("none", "counts", "timings") if enabled else ("none",)
    if options.smoke:
        return ([case(messages=2, history=4, collection=mode) for mode in collections]
                + [case(messages=2, history=4, history_scope="other", workload="backlog",
                        capacity_scale=4, payload_bytes=32, fields=4,
                        collection="timings" if enabled else "none")]
                + [case(messages=2, workload="stream", idle_pollers=count,
                        collection="counts" if enabled else "none") for count in (0, 1)]
                + [case(messages=2, history=4, workload="process")]
                + [case(messages=2, workload="stream", idle_pollers=1, work_budget_ms=1,
                        collection="timings" if enabled else "none")])
    if options.suite == "ci":
        collections = ("none", "timings") if enabled else ("none",)
    messages = 8 if options.suite == "ci" else 16
    cases = [case(messages=messages, history=history, collection=mode)
             for history in (0, 32, 128) for mode in collections]
    controls = [dict(history_scope="other"), dict(capacity_scale=16),
                dict(payload_bytes=256, fields=1), dict(payload_bytes=256, fields=32),
                dict(workload="backlog"), dict(workload="stream"),
                dict(workload="stream", idle_pollers=3)]
    if options.suite == "ci":
        controls = [values for values in controls if "payload_bytes" not in values]
    for values in controls:
        for mode in (("none", "timings") if enabled else ("none",)):
            cases.append(case(messages=messages, history=128, collection=mode, **values))
    for mode in (("none", "timings") if enabled else ("none",)):
        cases.append(case(messages=messages, history=32 if options.suite == "ci" else 128,
                          workload="process", collection=mode))
    return cases


def number(value, label):
    check(type(value) is int and value >= 0, f"Invalid nonnegative integer {label}: {value!r}")
    return value


def rank(values, percent):
    ordered = sorted(values)
    return ordered[(percent * len(ordered) + 99) // 100 - 1]


def validate_observation(sample, collection, partial_cpu=False, empty=False):
    check(sample.get("valid") is True and sample.get("overflow") is False, "Invalid/overflowed capture")
    for name in ("wall_ns", "cpu_ns"):
        number(sample.get(name), name)
    check(type(sample.get("cpu_available")) is bool and
          (partial_cpu or sample["cpu_available"] or sample["cpu_ns"] == 0), "Invalid CPU clock availability")
    counters, phases = sample.get("counters"), sample.get("phases")
    check(type(counters) is dict and set(counters) == COUNTERS, "Unexpected counter schema")
    for name, count in counters.items():
        number(count, name)
    check(counters["native_gate_holds"] == counters["gate_acquisitions"],
          "Native gate acquisition and completed-hold counts disagree")
    if collection == "counts":
        check(counters["native_gate_hold_ns"] == 0, "Counts-only collection timed native gate ownership")
    elif collection == "timings":
        check(counters["native_gate_hold_ns"] <= sample["wall_ns"], "Native gate ownership exceeds public call time")
    check(type(phases) is list, "Phases must be an array")
    names = set()
    for phase in phases:
        check(type(phase) is dict and phase.get("name") in PHASES and phase["name"] not in names,
              "Invalid/repeated phase")
        names.add(phase["name"])
        check(number(phase.get("calls"), "phase calls") > 0, "A reported phase must have calls")
        number(phase.get("exclusive_ns"), "exclusive duration")
        check(number(phase.get("inclusive_ns"), "inclusive duration") >= phase["exclusive_ns"],
              "Exclusive phase time exceeds inclusive time")
    if empty:
        check(not phases and not any(counters.values()) and sample["wall_ns"] == sample["cpu_ns"] == 0,
              "A zero-call aggregate contains observations")
    elif collection == "none":
        check(not phases and not any(counters.values()), "Disabled collection recorded work")
    elif collection == "counts":
        check("other" in names and all(p["inclusive_ns"] == p["exclusive_ns"] == 0 for p in phases),
              "Counts-only collection recorded phase clocks")
    else:
        check("other" in names and sum(p["exclusive_ns"] for p in phases) == sample["wall_ns"],
              "Exclusive phases and unattributed time do not account for the public call")


def validate(report, values, enabled=None):
    check(type(report) is dict, "Diagnostic report must be a JSON object")
    check(report.get("framework_version") == (ROOT / "VERSION").read_text().strip(),
          "Framework version differs from this checkout; rebuild the executable")
    for name, expected in values.items():
        check(type(report.get(name)) is type(expected) and report[name] == expected, f"Invalid {name}")
    check(type(report.get("instrumentation_enabled")) is bool, "Missing build instrumentation flag")
    if enabled is not None:
        check(report["instrumentation_enabled"] == enabled, "Instrumentation flag changed within the matrix")
    check(report["instrumentation_enabled"] or values["collection"] == "none", "Disabled build collected observations")
    n, history = values["messages"], values["history"]
    accepted = number(report.get("accepted"), "accepted publications")
    completed = number(report.get("completed"), "completed acknowledgments")
    pending = number(report.get("pending"), "pending publications")
    unfinished = number(report.get("unacknowledged_at_stop"), "unfinished delivery")
    check(completed + unfinished <= accepted <= n and unfinished <= 1 and pending == accepted - completed,
          "Publication/acknowledgment progress is inconsistent")
    outcome = "complete" if completed == accepted == n else "budget_exceeded"
    check(report.get("outcome") == outcome and
          (outcome == "complete" or values["workload"] in ("stream", "process")), "Invalid work-budget outcome")
    for name, expected in dict(channels=4, ring_capacity=8, history_before=history,
                               history_after=history + accepted,
                               checksum=completed * values["payload_bytes"] * 65).items():
        check(type(report.get(name)) is int and report[name] == expected, f"Invalid {name}")
    process_case = values["workload"] == "process"
    replacement = values["workload"] in ("stream", "process")
    check(report.get("clock_scope") == ("actor_relative" if process_case else "process_relative"),
          "Wrong sample clock domain")
    for name, expected in dict(producer_incarnation=2 if replacement else 1,
                               consumer_incarnation=2 if replacement else 1,
                               active_claims=2 + values["idle_pollers"] if replacement else 9).items():
        check(type(report.get(name)) is int and report[name] == expected, f"Wrong {name} ownership cohort")
    check(type(report.get("history_bounds_exact")) is bool and
          report["history_bounds_exact"] == (values["workload"] not in ("stream", "process")), "Wrong history precision flag")
    for name in ("file_bytes_before", "file_bytes_after_publish", "file_bytes_after_drain", "append_bytes",
                 "publish_phase_ns", "drain_phase_ns", "cycle_phase_ns"):
        number(report.get(name), name)
    before, after = report["file_bytes_before"], report["file_bytes_after_publish"]
    check(before > 0 and after == report["file_bytes_after_drain"] and after - before == report["append_bytes"] and
          (report["append_bytes"] > 0 if accepted else report["append_bytes"] == 0),
          "Append/drain file accounting changed")
    check(all(count % 64 == 0 for count in (before, after, report["append_bytes"])), "Unaligned file accounting")
    if process_case:
        check(report.get("clock_scope") == "actor_relative" and number(report.get("process_phase_ns"), "process duration") > 0,
              "Invalid process phase timing")
        check(number(report.get("process_exit_ns"), "process completion") >= report["process_phase_ns"] and
              report["cycle_phase_ns"] == report["publish_phase_ns"] == report["drain_phase_ns"] == 0,
              "Process and thread phase accounting were mixed")
        workers = report.get("worker_processes")
        check(type(workers) is list and len(workers) == 2 and {w.get("role") for w in workers} == {"producer", "consumer"},
              "Missing independent-process roles")
        check(all(type(w.get("pid")) is int and w["pid"] > 0 and w.get("incarnation") == 2 for w in workers) and
              workers[0]["pid"] != workers[1]["pid"], "Invalid independent worker identities")
    elif values["workload"] == "backlog":
        check(report["cycle_phase_ns"] == 0 and report["publish_phase_ns"] > 0 and report["drain_phase_ns"] > 0,
              "Invalid backlog phase timing")
    else:
        check(report["cycle_phase_ns"] > 0 and report["publish_phase_ns"] == report["drain_phase_ns"] == 0,
              "Invalid cycle phase timing")
    samples = report.get("samples")
    check(type(samples) is list and len(samples) <= 4 * n + 19 + values["idle_pollers"] * (16 * n + 64),
          "Unexpected or unbounded sample count")
    operations = {name: [] for name in OPERATIONS}
    last_start = {}
    for sample in samples:
        check(type(sample) is dict and sample.get("operation") in OPERATIONS, "Invalid sample operation")
        check(sample.get("result") in RESULTS[sample["operation"]], "Missing/invalid operation outcome")
        for name in ("index", "start_ns", "history_before", "history_after", "history_upper_bound", "wall_ns", "cpu_ns"):
            number(sample.get(name), name)
        clock = sample.get("actor") if process_case else "shared"
        check(sample["start_ns"] >= last_start.get(clock, 0), "Samples are not ordered within their clock domain")
        last_start[clock] = sample["start_ns"]
        check(history <= sample["history_before"] <= sample["history_after"] <= sample["history_upper_bound"] <= history + n and
              sample["history_after"] <= history + accepted,
              "Invalid operation history bounds")
        if report["history_bounds_exact"]:
            check(sample["history_after"] == sample["history_upper_bound"], "Inexact history in a nonstreaming case")
        check(type(sample.get("actor")) is int and 0 <= sample["actor"] <= 1 + values["idle_pollers"],
              "Invalid worker identifier")
        validate_observation(sample, values["collection"])
        if sample["operation"] in ("status", "identical_retry", "empty_receive", "idle_empty_receive"):
            check(sample["counters"]["file_grow_calls"] == 0, "Observational operation grew the file")
        operations[sample["operation"]].append(sample)
    issued = len(operations["issue"])
    check(accepted <= issued <= min(n, accepted + 1), "Invalid issued-but-unsent publication accounting")
    check(len(operations["send"]) == accepted and len(operations["acknowledge"]) == completed,
          "Send/acknowledgment samples disagree with progress")
    offset = history if values["history_scope"] == "same" and not replacement else 0
    for name, count in (("issue", issued), ("send", accepted), ("acknowledge", completed)):
        check(sorted(s["index"] for s in operations[name]) == list(range(offset + 1, offset + count + 1)),
              f"Invalid {name} sequence samples")
    controls = dict(status=accepted > 0, identical_retry=accepted > 0,
                    empty_receive=completed == (n if process_case else accepted))
    check(type(report.get("controls")) is dict and report["controls"] == controls and
          all(type(flag) is bool for flag in report["controls"].values()), "Control presence/skipping is inconsistent")
    for name, performed in controls.items():
        check(len(operations[name]) == int(performed), f"Invalid {name} control count")
        if performed and name != "empty_receive":
            check(operations[name][0]["index"] == offset + accepted, "Control lookup changed publication identity")
    receives = operations["receive"]
    check(sum(s["result"] == "delivery" for s in receives) == completed + unfinished and
          all(s["result"] in ("delivery", "timeout") for s in receives), "Receive outcomes/counts changed")
    check(sorted(s["index"] for s in receives if s["result"] == "delivery") == list(range(offset + 1, offset + completed + unfinished + 1)),
          "Delivery sample sequence changed")
    check(completed + unfinished <= len(receives) <= completed + unfinished + (16 if replacement else 0),
          "Unexpected receive retries")
    idle = operations["idle_empty_receive"]
    check(len(idle) <= values["idle_pollers"] * (16 * n + 64) and
          all(s["result"] == "timeout" for s in idle), "Invalid idle polling samples")
    summaries = report.get("idle_pollers_summary")
    check(type(summaries) is list and len(summaries) == values["idle_pollers"], "Missing idle-worker aggregates")
    check(sorted(summary.get("actor", -1) for summary in summaries) == list(range(2, 2 + values["idle_pollers"])),
          "Invalid idle-worker aggregate identities")
    for summary in summaries:
        for name in ("polls", "captured_samples", "unrecorded_samples"):
            number(summary.get(name), name)
        captured = [sample for sample in idle if sample["actor"] == summary["actor"]]
        check(summary["captured_samples"] == len(captured) and
              summary["polls"] == len(captured) + summary["unrecorded_samples"], "Invalid idle sample accounting")
        check(type(summary.get("raw_truncated")) is bool and
              summary["raw_truncated"] == (summary["unrecorded_samples"] > 0), "Wrong raw-prefix precision flag")
        aggregate = summary.get("aggregate")
        check(type(aggregate) is dict and type(summary.get("cpu_all_available")) is bool and
              summary["cpu_all_available"] == aggregate.get("cpu_available"), "Invalid idle CPU coverage")
        validate_observation(aggregate, values["collection"], partial_cpu=True, empty=summary["polls"] == 0)
        check(aggregate["wall_ns"] >= sum(s["wall_ns"] for s in captured), "Idle aggregate omits captured work")
        for name in COUNTERS:
            observed = sum(s["counters"][name] for s in captured)
            check(aggregate["counters"][name] >= observed,
                  "Idle counter aggregate omits captured work")
            if not summary["raw_truncated"]:
                check(aggregate["counters"][name] == observed, "Complete idle counter aggregate disagrees with samples")
        if not summary["raw_truncated"]:
            check(aggregate["wall_ns"] == sum(s["wall_ns"] for s in captured), "Complete idle aggregate disagrees with samples")
            check(aggregate["cpu_ns"] == sum(s["cpu_ns"] for s in captured) and
                  aggregate["cpu_available"] == (bool(captured) and all(s["cpu_available"] for s in captured)),
                  "Complete idle CPU aggregate disagrees with samples")
    return report["instrumentation_enabled"]


def summarize(reports):
    groups = {}
    for report in reports:
        dimensions = tuple(report[name] for name in SUMMARY_DIMENSIONS)
        per_run = {}
        for sample in report["samples"]:
            per_run.setdefault((sample["operation"], sample["result"]), []).append(sample)
        for operation, samples in per_run.items():
            idle = report["idle_pollers_summary"] if operation[0] == "idle_empty_receive" else []
            groups.setdefault(dimensions + operation, []).append((samples, idle))
    summaries, phases = [], []
    for key, runs in groups.items():
        dimensions = dict(zip(SUMMARY_DIMENSIONS + ("operation", "result"), key))
        raw = [samples for samples, _ in runs]
        shared = dict(dimensions, runs_reporting_operation=len(runs),
                      min_samples_per_run=min(map(len, raw)), max_samples_per_run=max(map(len, raw)))
        statistics_per_run = []
        truncated = any(summary["raw_truncated"] for _, idle in runs for summary in idle)
        for samples, idle in runs:
            wall = [s["wall_ns"] for s in samples]
            cpu = [s["cpu_ns"] for s in samples if s["cpu_available"]]
            stats = {"samples": len(samples), "total_wall_ns": sum(wall), "mean_wall_ns": statistics.mean(wall),
                     "p50_wall_ns": rank(wall, 50), "p95_wall_ns": rank(wall, 95), "p99_wall_ns": rank(wall, 99),
                     "cpu_samples": len(cpu), "mean_cpu_ns": statistics.mean(cpu) if cpu else 0}
            stats.update({"mean_" + name: statistics.mean(s["counters"][name] for s in samples) for name in sorted(COUNTERS)})
            statistics_per_run.append(stats)
        summaries.append(dict(shared, sample_population="captured_prefix" if truncated else "all_observed_calls",
                              **{"median_run_" + name: statistics.median(run[name] for run in statistics_per_run)
                                         for name in statistics_per_run[0]}))
        phase_runs = [([summary["aggregate"] for summary in idle] if idle else samples,
                       sum(summary["polls"] for summary in idle) if idle else len(samples)) for samples, idle in runs]
        names = sorted({phase["name"] for samples, _ in phase_runs for sample in samples for phase in sample["phases"]})
        for name in names:
            totals = []
            for samples, _ in phase_runs:
                present = [phase for sample in samples for phase in sample["phases"] if phase["name"] == name]
                totals.append({"calls": sum(p["calls"] for p in present),
                               "inclusive_ns": sum(p["inclusive_ns"] for p in present),
                               "exclusive_ns": sum(p["exclusive_ns"] for p in present)})
            row = dict(shared, phase=name, sample_population="all_observed_calls",
                       min_samples_per_run=min(count for _, count in phase_runs),
                       max_samples_per_run=max(count for _, count in phase_runs))
            row.update({"median_run_total_" + field: statistics.median(run[field] for run in totals)
                        for field in ("calls", "inclusive_ns", "exclusive_ns")})
            row["median_run_mean_exclusive_ns"] = statistics.median(run["exclusive_ns"] / count if count else 0
                                                                      for run, (_, count) in zip(totals, phase_runs))
            phases.append(row)
    return summaries, phases


def write_artifacts(directory, reports, manifest, sources, settings, cases, executable):
    with gzip.open(directory / "reports.jsonl.gz", "wt", encoding="utf-8") as output:
        for report in reports:
            output.write(json.dumps(report, ensure_ascii=False, sort_keys=True) + "\n")
    rows = []
    for report in reports:
        row = {name: report[name] for name in SUMMARY_DIMENSIONS}
        row.update(repetition=report["repetition"], operations_recorded=len(report["samples"]),
                   checksum=report["checksum"], append_bytes=report["append_bytes"],
                   cycle_phase_ns=report["cycle_phase_ns"], publish_phase_ns=report["publish_phase_ns"],
                   drain_phase_ns=report["drain_phase_ns"], process_phase_ns=report.get("process_phase_ns", ""),
                   process_exit_ns=report.get("process_exit_ns", ""))
        rows.append(row)
    with (directory / "runs.csv").open("w", newline="", encoding="utf-8") as output:
        if rows:
            writer = csv.DictWriter(output, fieldnames=list(rows[0]))
            writer.writeheader()
            writer.writerows(rows)
    for name, rows in zip(("summary.csv", "phases.csv"), summarize(reports)):
        with (directory / name).open("w", newline="", encoding="utf-8") as output:
            if rows:
                writer = csv.DictWriter(output, fieldnames=list(rows[0]))
                writer.writeheader()
                writer.writerows(rows)
    with (directory / "manifest.toml").open("w", encoding="utf-8") as output:
        write_toml(output, manifest, sources, settings, cases, executable)


def smoke_rejections(executable, temporary, timeout, existing):
    valid = case(messages=2)
    invalid = [dict(messages=0), dict(messages=129), dict(history=2049), dict(payload_bytes=0),
               dict(payload_bytes=4097), dict(fields=0), dict(fields=513), dict(fields=3),
               dict(capacity_scale=2), dict(workload="unknown"), dict(collection="unknown"),
               dict(history_scope="unknown"), dict(idle_pollers=1), dict(work_budget_ms=0), dict(work_budget_ms=60001)]
    for index, overrides in enumerate(invalid):
        path = temporary / f"invalid-{index}"
        invoke(command(executable, path, dict(valid, **overrides)), timeout, succeeds=False)
        check(not path.exists() and not path.is_symlink(), "Invalid arguments created a Session")
    for extra in (["--messages"], ["--messages", "2"], ["--unknown", "1"],
                  ["--cachegrind-region", "unknown"]):
        path = temporary / "invalid-options"
        invoke(command(executable, path, valid) + extra, timeout, succeeds=False)
        check(not path.exists(), "Invalid option list created a Session")
    for values, extra in ((case(messages=2, workload="process"), []),
                          (case(messages=2, workload="process"), ["--role", "unknown"]),
                          (valid, ["--role", "setup"])):
        path = temporary / "invalid-role"
        invoke(command(executable, path, values) + extra, timeout, succeeds=False)
        check(not path.exists(), "Invalid process role created a Session")
    def snapshot():
        return {str(p.relative_to(existing)): sha256(p) if p.is_file() else "directory"
                for p in existing.rglob("*")}
    before = snapshot()
    invoke(command(executable, existing, valid), timeout, succeeds=False)
    check(snapshot() == before, "Existing-Session rejection changed its files")


def write_toml(output, manifest, sources, settings, cases, executable):
    def entries(values):
        for name, value in values.items():
            output.write(f"{json.dumps(name, ensure_ascii=False)} = {json.dumps(value, ensure_ascii=False)}\n")
    entries(manifest)
    for name, values in (("source_sha256", sources), ("cmake", settings)):
        output.write(f"\n[{name}]\n")
        entries(values)
    for values in cases:
        output.write("\n[[cases]]\n")
        entry = dict(values, arguments=command(executable, "NEW_SESSION_DIRECTORY", values))
        if values["workload"] == "process":
            entry["role_arguments"] = [entry["arguments"] + ["--role", role] for role in ("setup", "producer", "consumer")]
        entries(entry)


def parse_arguments():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("executable", type=Path)
    parser.add_argument("--output", type=Path, help="new directory for compressed reports, CSV, and TOML")
    parser.add_argument("--suite", choices=("diagnostic", "ci"), default="diagnostic")
    parser.add_argument("--repetitions", type=int, default=3)
    parser.add_argument("--timeout", type=int, default=90, help="seconds per benchmark invocation")
    parser.add_argument("--host-label", default="")
    parser.add_argument("--smoke", action="store_true")
    options = parser.parse_args()
    try:
        check(options.output is not None or options.smoke, "Normal runs require --output NEWDIR")
        check(1 <= options.repetitions <= 20 and options.timeout > 0,
              "Repetitions must be 1..20 and timeout must be positive")
        executable = options.executable.resolve(strict=True)
        check(executable.is_file(), "Executable must be a regular file")
    except (OSError, RuntimeError) as error:
        parser.error(str(error))
    return options, executable


def progress(index, cases, repetition, repetitions, report):
    print(f"case {index + 1}/{cases} repeat {repetition}/{repetitions}: {report['workload']} "
          f"history={report['history']} collection={report['collection']} {report['outcome']} "
          f"accepted={report['accepted']}/{report['messages']} completed={report['completed']}/{report['messages']}",
          file=sys.stderr, flush=True)


def main():
    options, executable = parse_arguments()
    reports, sources, settings = [], {}, {}
    output = options.output.absolute() if options.output else None
    manifest = {"started_utc": datetime.now(timezone.utc).isoformat(), "status": "failed"}
    first = case(messages=2 if options.smoke else (8 if options.suite == "ci" else 16),
                 history=4 if options.smoke else 0)
    cases = [first]
    try:
        if output:
            output.mkdir()  # Exclusive: existing directories/files/symlinks reject before any write.
        try:
            manifest, sources, settings = experiment_metadata(executable, options)
            manifest["status"] = "failed"
            repetitions = 1 if options.smoke else options.repetitions
            with tempfile.TemporaryDirectory(prefix="angaria-diagnostics-") as temporary_name:
                temporary = Path(temporary_name)
                with tempfile.TemporaryDirectory(prefix="run-", dir=temporary) as run_name:
                    directory = Path(run_name) / "session"
                    report = execute_case(executable, directory, first, options.timeout)
                    enabled = validate(report, first)
                    reports.append(dict(report, repetition=1))
                    manifest["instrumentation_enabled"] = enabled
                    cases = matrix(options, enabled)
                    check(cases[0] == first, "Discovery run differs from first matrix case")
                    progress(0, len(cases), 1, repetitions, reports[-1])
                    if options.smoke:
                        smoke_rejections(executable, temporary, options.timeout, directory)
                for index, values in enumerate(cases):
                    for repetition in range(repetitions):
                        if index == 0 and repetition == 0:
                            continue
                        with tempfile.TemporaryDirectory(prefix="run-", dir=temporary) as run_name:
                            report = execute_case(executable, Path(run_name) / "session", values, options.timeout)
                            validate(report, values, enabled)
                            reports.append(dict(report, repetition=repetition + 1))
                            progress(index, len(cases), repetition + 1, repetitions, reports[-1])
            manifest["status"] = "complete"
        except Exception as error:
            manifest["failure"] = str(error)
            raise
        finally:
            manifest["finished_utc"] = datetime.now(timezone.utc).isoformat()
            manifest["completed_runs"] = len(reports)
            manifest["recorded_runs"] = len(reports)
            manifest["budget_exceeded_runs"] = sum(report["outcome"] == "budget_exceeded" for report in reports)
            manifest["completed_workloads"] = sum(report["outcome"] == "complete" for report in reports)
            if output:
                write_artifacts(output, reports, manifest, sources, settings, cases, executable)
        print(f"Validated {len(reports)} diagnostic runs" + (f"; reports retained at {output}" if output else " (smoke)."))
        return 0
    except (OSError, RuntimeError, ValueError, KeyError, TypeError) as error:
        print(f"run_diagnostics: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
