#!/usr/bin/env python3
"""Coordinate bounded 1:1 Angaria/libnanomsg IPC cohorts and retain every attempt.

Five ABBA blocks are the ordinary matrix; --smoke uses one two-message block.
These are finite-cohort observations, never sustained-capacity or tail claims.
Control barriers are per batch; no per-message acknowledgment is added to PUSH.
"""
import argparse
from collections import deque
import csv
from datetime import datetime, timezone
import json
import math
import os
from pathlib import Path
import resource
import re
import selectors
import statistics
import struct
import subprocess
import sys
import tempfile
import time

sys.dont_write_bytecode = True
from run_native import ROOT, check, metadata, sha256


PROFILES = ("angaria", "nanomsg")
ROLES = ("producer", "consumer")
DIMENSIONS = ("messages", "history", "payload_bytes", "interval_ns", "socket_buffer_bytes", "budget_ms")
MAX_STDOUT = 4 * 1024 * 1024
MAX_STDERR = 1024 * 1024
MAX_LINE = 1024 * 1024
FORMAT_VERSION = 1
DEPENDENCY_FIELDS = ("nanomsg_version", "nanomsg_commit", "nanomsg_archive_sha256") + tuple(
    "nanomsg_" + kind + "_abi_" + part for kind in ("runtime", "header") for part in ("current", "revision", "age"))


def pinned_dependency():
    """Read the selected source pin from its owning build definition."""
    source = (ROOT / "CMakeLists.txt").read_text()
    result = {}
    for field in ("version", "commit", "archive_sha256"):
        match = re.search(r'ANGARIA_NANOMSG_' + field.upper() + r'="([^"\n]+)"', source)
        check(match is not None, "Missing comparison dependency build pin")
        result["nanomsg_" + field] = match[1]
    return result


def clock_ns():
    return time.clock_gettime_ns(time.CLOCK_MONOTONIC)


def integer(value, label, minimum=0):
    check(type(value) is int and value >= minimum, f"Invalid {label}: {value!r}")
    return value


def payload_hash(sequence, scheduled, logical_start, size):
    data = struct.pack("<HHIQQQ", 1, 1, 0, sequence, scheduled, logical_start)
    data += bytes((sequence + index) & 255 for index in range(32, size))
    value = 14695981039346656037
    for byte in data:
        value = ((value ^ byte) * 1099511628211) & ((1 << 64) - 1)
    return f"{value:016x}"


def command(executable, profile, role, directory, case):
    result = [str(executable), "--profile", profile, "--role", role, "--directory", str(directory)]
    for name in DIMENSIONS:
        result += ["--" + name.replace("_", "-"), str(case[name])]
    return result


class Workers:
    """Drain bounded raw pipes throughout every barrier, including failed runs."""
    def __init__(self, directory, deadline, control_name="control.jsonl"):
        self.directory, self.deadline = directory, deadline
        self.selector = selectors.DefaultSelector()
        self.workers = {}
        self.control = (directory / control_name).open("w", encoding="utf-8")

    def record(self, **values):
        self.control.write(json.dumps(values, sort_keys=True) + "\n")
        self.control.flush()

    def start(self, role, arguments):
        (self.directory / (role + "-command.json")).write_text(json.dumps(arguments, indent=2) + "\n")
        process = subprocess.Popen(arguments, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                   stderr=subprocess.PIPE, bufsize=0)
        state = dict(process=process, events=deque(), buffer=bytearray(),
                     sizes={"stdout": 0, "stderr": 0}, streams={}, eof=set())
        self.workers[role] = state
        for pipe in ("stdout", "stderr"):
            state["streams"][pipe] = (self.directory / (role + "." + pipe)).open("wb")
            self.selector.register(getattr(process, pipe), selectors.EVENT_READ, (role, pipe))

    def send(self, role, line):
        check(len(line) <= 128 and "\n" not in line, "Invalid bounded control command")
        before = clock_ns()
        stream = self.workers[role]["process"].stdin
        data = (line + "\n").encode("ascii")
        check(stream.write(data) == len(data), f"Short {role} control write")
        stream.flush()
        after = clock_ns()
        self.record(direction="send", role=role, command=line, before_ns=before, after_ns=after)
        return before, after

    def poll(self):
        remaining = self.deadline - time.monotonic()
        check(remaining > 0, "IPC participant deadline expired; raw streams are retained")
        for key, _ in self.selector.select(min(remaining, 0.1)):
            role, pipe = key.data
            state = self.workers[role]
            chunk = os.read(key.fileobj.fileno(), 65536)
            if not chunk:
                self.selector.unregister(key.fileobj)
                state["eof"].add(pipe)
                if pipe == "stdout":
                    check(not state["buffer"], f"{role} ended with an unterminated JSON event")
                continue
            maximum = MAX_STDOUT if pipe == "stdout" else MAX_STDERR
            available = max(0, maximum - state["sizes"][pipe])
            state["streams"][pipe].write(chunk[:available])
            state["streams"][pipe].flush()
            state["sizes"][pipe] += len(chunk)
            check(state["sizes"][pipe] <= maximum, f"{role} {pipe} exceeds bounded capture")
            if pipe == "stdout":
                state["buffer"].extend(chunk)
                while b"\n" in state["buffer"]:
                    line, _, rest = state["buffer"].partition(b"\n")
                    state["buffer"][:] = rest
                    check(0 < len(line) <= MAX_LINE, f"Invalid {role} JSON event length")
                    event = json.loads(line)
                    check(type(event) is dict and type(event.get("event")) is str,
                          f"Invalid {role} JSON event")
                    received = clock_ns()
                    state["events"].append((event, received))
                    check(len(state["events"]) <= 16, f"Unbounded queued {role} events")
                    self.record(direction="receive", role=role, event=event["event"], received_ns=received)
                check(len(state["buffer"]) <= MAX_LINE, f"Oversized partial {role} JSON line")

    def events(self, expected, profile):
        found = {}
        while len(found) != len(expected):
            for role, name in expected.items():
                if role in found:
                    continue
                state = self.workers[role]
                if state["events"]:
                    event, received = state["events"].popleft()
                    check(event["event"] == name, f"Expected {role} {name}, received {event['event']}")
                    check(event.get("profile") == profile and event.get("role") == role,
                          f"Wrong profile/role in {role} {name}")
                    found[role] = (event, received)
                else:
                    check("stdout" not in state["eof"], f"{role} exited before {name}")
            if len(found) != len(expected):
                self.poll()
        return found

    def exited(self, expected_codes=None):
        while self.selector.get_map():
            self.poll()
        for role, state in self.workers.items():
            check(not state["events"], f"Unexpected extra {role} events")
            remaining = max(0.01, self.deadline - time.monotonic())
            expected = 0 if expected_codes is None else expected_codes[role]
            check(state["process"].wait(timeout=remaining) == expected, f"{role} exit disagrees with its reported outcome")

    def close(self):
        # Cleanup must retain available raw tails without replacing the original
        # failure or losing reports from previously completed matrix cells.
        errors = []
        for state in self.workers.values():
            if state["process"].poll() is None:
                try:
                    state["process"].kill()
                except OSError as error:
                    errors.append(str(error))
        self.deadline = time.monotonic() + 0.5
        while self.selector.get_map() and time.monotonic() < self.deadline:
            try:
                self.poll()
            except Exception as error:
                errors.append(str(error))
        for state in self.workers.values():
            try:
                state["process"].wait(timeout=5)
            except (OSError, subprocess.TimeoutExpired) as error:
                errors.append(str(error))
            finally:
                for pipe in ("stdin", "stdout", "stderr"):
                    try:
                        getattr(state["process"], pipe).close()
                    except OSError as error:
                        errors.append(str(error))
                for stream in state["streams"].values():
                    stream.close()
        self.selector.close()
        if errors:
            self.record(direction="cleanup", errors=errors)
        self.control.close()
        return errors


def common_identity(report, role, profile, case, version):
    check(type(report.get("format_version")) is int and report["format_version"] == FORMAT_VERSION,
          "Unsupported IPC report version")
    check(report.get("profile") == profile and report.get("role") == role, "Wrong IPC report identity")
    check(report.get("framework_version") == version, "Executable version differs from this checkout")
    check(type(report.get("nanomsg_version")) is str and bool(report["nanomsg_version"]), "Missing nanomsg version")
    check(type(report.get("nanomsg_commit")) is str and re.fullmatch(r"[0-9a-f]{40}", report["nanomsg_commit"]),
          "Missing exact nanomsg source commit")
    check(type(report.get("nanomsg_archive_sha256")) is str and re.fullmatch(r"[0-9a-f]{64}", report["nanomsg_archive_sha256"]),
          "Missing nanomsg source archive hash")
    check(all(report.get(key) == value for key, value in pinned_dependency().items()),
          "Executable dependency differs from this checkout's selected source pin")
    for part in ("current", "revision", "age"):
        check(integer(report.get("nanomsg_runtime_abi_" + part), "nanomsg runtime ABI") ==
              integer(report.get("nanomsg_header_abi_" + part), "nanomsg header ABI"), "nanomsg runtime/header ABI mismatch")
    for key in ("messages", "history", "payload_bytes"):
        check(type(report.get(key)) is int and report[key] == case[key], f"Wrong report {key}")
    check(report.get("clock_id") == "CLOCK_MONOTONIC", "Worker uses a different monotonic clock")
    integer(report.get("clock_resolution_ns"), "clock resolution", 1)
    integer(report.get("pid"), "worker pid", 1)


def common_report(report, role, profile, case, version):
    common_identity(report, role, profile, case, version)
    check(report.get("clock_pair_samples") == 256, "Missing clock-read calibration")
    minimum = integer(report.get("clock_pair_min_ns"), "minimum clock-read pair time")
    median = report.get("clock_pair_median_ns")
    check(type(median) in (int, float) and math.isfinite(median) and median >= minimum,
          "Invalid median clock-read pair time")
    check(type(report.get("clock_calibration_scope")) is str and bool(report["clock_calibration_scope"]),
          "Missing clock calibration scope")
    check(type(report.get("cpu_scope")) is str and "START" in report["cpu_scope"] and "FINISH" in report["cpu_scope"],
          "Missing participant CPU interval scope")
    check(type(report.get("rows")) is list and len(report["rows"]) <= case["messages"]
          and all(type(row) is dict for row in report["rows"]), "Invalid bounded participant rows")
    for key in ("peak_rss_bytes", "file_bytes_before", "file_bytes_after", "socket_send_buffer_bytes", "socket_receive_buffer_bytes"):
        integer(report.get(key), key)
    for key in ("thread_count_start", "thread_count_end"):
        if report.get(key) is not None:
            integer(report[key], key, 1)


def validate_rows(producer, consumer, profile, case, t0):
    count, history, size = (case[key] for key in ("messages", "history", "payload_bytes"))
    sent, received, metrics = {}, {}, {}
    def sample(name, value):
        integer(value, name)
        metrics.setdefault(name, []).append(value)
    for report in (producer, consumer):
        for key in ("accepted", "verified", "completed", "acknowledged"):
            integer(report.get(key), key)
        check(type(report.get("rows")) is list and len(report["rows"]) <= count, "Invalid participant rows")
        check([row.get("sequence") for row in report["rows"]] == list(range(history + 1, history + len(report["rows"]) + 1)),
              "Attempted 1:1 sequence prefix changed")
    for row in producer["rows"]:
        sequence = integer(row.get("sequence"), "producer sequence", history + 1)
        check(sequence <= history + count and sequence not in sent, "Repeated/out-of-range producer sequence")
        check(type(row.get("accepted")) is bool, "Missing acceptance outcome")
        scheduled = integer(row.get("scheduled_ns"), "scheduled timestamp")
        check(scheduled == t0 + (sequence - history - 1) * case["interval_ns"], "Offered schedule changed")
        logical = integer(row.get("logical_start_ns"), "logical start")
        start, end = (integer(row.get(key), key) for key in ("send_start_ns", "send_end_ns"))
        sent[sequence] = row
        attempts = integer(row.get("send_attempts"), "send attempts")
        check(integer(row.get("backpressure_count"), "backpressure count") <= attempts, "Too many backpressure results")
        api = integer(row.get("send_api_ns"), "aggregate send API time")
        if logical:
            check(scheduled <= logical and row.get("payload_hash") == payload_hash(sequence, scheduled, logical, size),
                  "Producer payload hash or logical boundary differs from independent bytes")
        if not row["accepted"]:
            check(producer.get("status") != "complete", "Successful producer retained an unaccepted row")
            if start:
                check(logical <= start <= producer["data_end_ns"], "Incomplete send escapes its data interval")
                sample("send_incomplete_api_total_ns", api)
                sample("send_incomplete_backpressure_wall_ns", producer["data_end_ns"] - start)
            continue
        if row["accepted"]:
            check(attempts > 0 and scheduled <= logical <= start <= end, "Producer clock boundaries are out of order")
            check(producer["data_start_ns"] <= logical and end <= producer["data_end_ns"], "Accepted send escapes its data phase")
            check(integer(row.get("send_wall_ns"), "send/backpressure wall") == end - start and api <= end - start,
                  "Send API and backpressure accounting changed")
            success_start = integer(row.get("successful_send_start_ns"), "successful send start")
            success_end = integer(row.get("successful_send_end_ns"), "successful send end")
            check(start <= success_start <= success_end <= end, "Successful send escapes its attempt interval")
            sample("send_api_total_ns", api)
            sample("successful_send_api_ns", success_end - success_start)
            sample("send_backpressure_wall_ns", end - start)
            sample("scheduled_acceptance_ns", end - scheduled)
            sample("logical_producer_path_ns", end - logical)
            if profile == "angaria":
                id_start, id_end = (integer(row.get(key), key) for key in ("id_start_ns", "id_end_ns"))
                check(logical <= id_start <= id_end <= start, "ID issuance escapes the producer path")
                sample("issue_api_ns", id_end - id_start)
            else:
                check(row.get("id_start_ns") == row.get("id_end_ns") == 0, "nanomsg reports Angaria ID issuance")
    accepted = {sequence: row for sequence, row in sent.items() if row["accepted"]}
    for row in consumer["rows"]:
        sequence = integer(row.get("sequence"), "consumer sequence", history + 1)
        check(sequence <= history + count, "Out-of-range consumer sequence")
        ack_start, ack_end = (integer(row.get(key), key) for key in ("ack_start_ns", "ack_end_ns"))
        if not integer(row.get("verified_ns"), "verification timestamp"):
            check(consumer.get("status") != "complete", "Successful consumer retained an unverified row")
            check(ack_start == ack_end == 0, "Unverified delivery reports an acknowledgment")
            if row.get("receive_start_ns"):
                sample("receive_incomplete_api_total_ns", integer(row.get("receive_api_ns"), "incomplete receive API time"))
                sample("receive_incomplete_wait_wall_ns", consumer["data_end_ns"] - row["receive_start_ns"])
            continue
        check(sequence in accepted and sequence not in received, "Unexpected/repeated consumer sequence")
        original = accepted[sequence]
        for key in ("scheduled_ns", "logical_start_ns", "payload_hash"):
            check(row.get(key) == original[key], f"Consumer payload {key} differs from producer")
        check(row["payload_hash"] == payload_hash(sequence, row["scheduled_ns"], row["logical_start_ns"], size),
              "Consumer payload hash differs from independent bytes")
        start, arrived, verified = (integer(row.get(key), key) for key in ("receive_start_ns", "receive_ns", "verified_ns"))
        success = integer(row.get("successful_receive_start_ns"), "successful receive start")
        check(consumer["data_start_ns"] <= start <= success <= arrived <= verified <= consumer["data_end_ns"]
              and start >= t0 and arrived >= row["logical_start_ns"],
              "Consumer clock boundaries are out of order")
        attempts = integer(row.get("receive_attempts"), "receive attempts", 1)
        check(integer(row.get("receive_timeouts"), "receive timeouts") <= attempts, "Too many receive timeouts")
        api = integer(row.get("receive_api_ns"), "aggregate receive API time")
        check(integer(row.get("receive_wall_ns"), "receive wait wall") == arrived - start and api <= arrived - start,
              "Receive API and waiting accounting changed")
        sample("receive_api_total_ns", api)
        sample("successful_receive_api_ns", arrived - success)
        sample("receive_wait_wall_ns", arrived - start)
        sample("verification_ns", verified - arrived)
        sample("logical_start_to_delivery_ns", arrived - row["logical_start_ns"])
        sample("scheduled_to_delivery_ns", arrived - row["scheduled_ns"])
        if profile == "angaria" and ack_end:
            check(verified <= ack_start <= ack_end <= consumer["data_end_ns"],
                  "Acknowledgment escapes the verified consumer data phase")
            sample("acknowledge_api_ns", ack_end - ack_start)
        else:
            check(ack_start == ack_end == 0, "Unacknowledged profile reports an acknowledgment")
        if profile == "angaria":
            for key in ("publication_sequence", "publication_incarnation"):
                check(integer(row.get(key), key, 1) == integer(original.get(key), key, 1), "Angaria library identity differs between roles")
        received[sequence] = row
    check(list(sent) == sorted(sent) and list(received) == sorted(received), "1:1 delivery order changed")
    check(list(accepted) == list(range(history + 1, history + len(accepted) + 1))
          and list(received) == list(range(history + 1, history + len(received) + 1)),
          "Accepted/verified 1:1 prefixes contain gaps")
    check(producer.get("accepted") == len(accepted), "Producer acceptance count disagrees with rows")
    check(consumer.get("verified") == len(received), "Consumer verification count disagrees with rows")
    acknowledgments = sum(bool(row["ack_end_ns"]) for row in received.values())
    check(consumer.get("acknowledged") == acknowledgments, "Acknowledgment count disagrees with rows")
    check(consumer.get("completed") == (acknowledgments if profile == "angaria" else len(received)),
          "Consumer completed count disagrees with its profile boundary")
    check(producer.get("completed") == producer.get("acknowledged") == producer.get("verified") == consumer.get("accepted") == 0,
          "Producer/consumer counters include the other role's work")
    for report in (producer, consumer):
        check(report.get("status") in ("complete", "budget_exceeded"), "Worker reports an error")
        check(integer(report.get("unexpected_messages"), "unexpected messages") == 0, "Extra cohort messages were observed")
        integer(report.get("data_start_ns"), "data start")
        check(report["data_start_ns"] >= t0 and integer(report.get("data_end_ns"), "data end") >= report["data_start_ns"],
              "Worker phase clock boundaries changed")
        check(integer(report.get("cpu_end_ns"), "process CPU end") >= integer(report.get("cpu_start_ns"), "process CPU start"),
              "Process CPU time decreased")
    complete_counts = len(accepted) == len(received) == count and (profile == "nanomsg" or acknowledgments == count)
    complete = complete_counts and all(report["status"] == "complete" for report in (producer, consumer))
    if all(report["status"] == "complete" for report in (producer, consumer)):
        check(complete_counts, "Successful workers did not complete the requested cohort")
    return dict(accepted=len(accepted), delivered=len(received), acknowledged=acknowledgments,
                pending=len(accepted) - len(received), metrics=metrics,
                status="complete" if complete else "budget_exceeded")


def derive_result(producer, consumer, profile, case, t0):
    """Reconstruct counts, timings and rates from validated participant rows.

    An incomplete attempt is observed until both data phases stop, including
    time spent stalled after the last delivery. Prefix rates remain separate
    diagnostics and never substitute for completed work over that interval.
    """
    result = validate_rows(producer, consumer, profile, case, t0)
    result["rates"] = {}
    rows = [row for row in consumer["rows"] if row["verified_ns"] > 0]
    accepted_rows = [row for row in producer["rows"] if row["accepted"]]
    complete = result["status"] == "complete"
    if not complete:
        stopped = max(producer["data_end_ns"], consumer["data_end_ns"]) - t0
        check(stopped > 0, "Nonpositive incomplete-attempt stop interval")
        result["cohort_stop_ns"] = stopped
        result["rates"]["bounded_cohort_verified_per_second"] = result["delivered"] * 1e9 / stopped
        if profile == "angaria":
            result["rates"]["bounded_cohort_acknowledged_per_second"] = result["acknowledged"] * 1e9 / stopped
    if rows:
        check(bool(accepted_rows), "Verified work has no accepted producer interval")
        last_verified = max(row["verified_ns"] for row in rows)
        first_send = min(row["send_start_ns"] for row in accepted_rows)
        cohort_ns, active_ns = last_verified - t0, last_verified - first_send
        check(cohort_ns > 0 and active_ns > 0, "Nonpositive common cohort interval")
        if complete:
            result["cohort_verified_ns"], result["active_verified_ns"] = cohort_ns, active_ns
            result["rates"]["bounded_cohort_verified_per_second"] = result["delivered"] * 1e9 / cohort_ns
            result["rates"]["first_send_to_last_verified_per_second"] = result["delivered"] * 1e9 / active_ns
        else:
            result["verified_prefix_ns"], result["active_verified_prefix_ns"] = cohort_ns, active_ns
            result["rates"]["verified_prefix_per_second"] = result["delivered"] * 1e9 / cohort_ns
            result["rates"]["first_send_to_last_verified_prefix_per_second"] = result["delivered"] * 1e9 / active_ns
        if profile == "angaria" and result["acknowledged"]:
            ack_ns = max(row["ack_end_ns"] for row in rows) - t0
            check(ack_ns > 0, "Nonpositive acknowledged prefix interval")
            name = "bounded_cohort_acknowledged_per_second" if complete else "acknowledged_prefix_per_second"
            result["rates"][name] = result["acknowledged"] * 1e9 / ack_ns
    return result


def validate_cached_summary(report):
    """Reject cached aggregates that disagree with their authoritative rows."""
    workers = report["worker_reports"]
    derived = derive_result(workers["producer"], workers["consumer"], report["profile"], report,
                            report["control_timing"]["t0_ns"])
    fields = ("accepted", "delivered", "acknowledged", "pending", "status", "metrics", "rates",
              "cohort_verified_ns", "active_verified_ns", "cohort_stop_ns", "verified_prefix_ns",
              "active_verified_prefix_ns")
    for key in fields:
        if key in derived:
            check(key in report and report[key] == derived[key], f"Cached {key} differs from reconstructed rows")
        else:
            check(key not in report, f"Unexpected cached {key} for this completion boundary")
    return derived


def children_cpu_ns():
    usage = resource.getrusage(resource.RUSAGE_CHILDREN)
    return round((usage.ru_utime + usage.ru_stime) * 1e9)


def attempt(executable, profile, case, directory, options, version):
    result = dict(case, profile=profile, status="failed", failure="", metrics={}, rates={})
    cpu_before, children_before = time.process_time_ns(), children_cpu_ns()
    deadline = time.monotonic() + options.timeout
    setup_workers = Workers(directory, deadline, "setup-control.jsonl")
    workers = None
    try:
        with tempfile.TemporaryDirectory(prefix="aipc-", dir="/tmp") as temporary:
            session = Path(temporary) / "s"
            setup_workers.start("setup", command(executable, profile, "setup", session, case))
            setup = setup_workers.events({"setup": "SETUP"}, profile)["setup"][0]
            result["setup"] = setup
            common_identity(setup, "setup", profile, case, version)
            check(setup["pid"] == setup_workers.workers["setup"]["process"].pid, "Setup PID differs from launched process")
            check(setup.get("history_count") == case["history"], "Setup history differs from its request")
            integer(setup.get("file_bytes"), "setup file bytes")
            setup_workers.exited()
            setup_workers.close()
            setup_workers = None
            workers = Workers(directory, deadline)
            for role in ROLES:
                workers.start(role, command(executable, profile, role, session, case))
            workers.events({role: "CONNECTED" for role in ROLES}, profile)
            probes = []
            tolerance = max(1, round(time.clock_getres(time.CLOCK_MONOTONIC) * 1e9))
            for nonce in range(1, 4):
                for role in ROLES:
                    before, _ = workers.send(role, f"CLOCK {nonce}")
                    event, received = workers.events({role: "CLOCK"}, profile)[role]
                    check(event.get("nonce") == nonce and event.get("pid") == workers.workers[role]["process"].pid,
                          "Clock probe identity changed")
                    check(event.get("clock_id") == "CLOCK_MONOTONIC", "Clock probe uses a different domain")
                    resolution = integer(event.get("clock_resolution_ns"), "probe clock resolution", 1)
                    worker_time = integer(event.get("clock_ns"), "probe monotonic time")
                    check(before - max(tolerance, resolution) <= worker_time <= received + max(tolerance, resolution),
                          "Participant monotonic epoch does not match the coordinator bracket")
                    probes.append(dict(role=role, nonce=nonce, before_ns=before, worker_ns=worker_time,
                                       received_ns=received, clock_resolution_ns=resolution))
            result["clock_probes"] = probes
            result["clock_domain_validated"] = True
            for role in ROLES:
                workers.send(role, "PRELOAD")
            drained = workers.events({role: "DRAINED" for role in ROLES}, profile)
            check(all(event[0].get("history_count") == case["history"] for event in drained.values()), "Preload was not fully drained")
            for role in ROLES:
                workers.send(role, "READY")
            workers.events({role: "READY" for role in ROLES}, profile)
            t0 = clock_ns() + options.start_lead_ms * 1_000_000
            start_cpu = time.process_time_ns()
            sent_start = {}
            for role in ROLES:
                sent_start[role] = workers.send(role, f"START {t0}")
            done = workers.events({role: "DONE" for role in ROLES}, profile)
            done_received = max(value[1] for value in done.values())
            finish_sent = {}
            for role in ROLES:
                finish_sent[role] = workers.send(role, "FINISH")
                workers.workers[role]["process"].stdin.close()
            coordinator_phase_cpu = time.process_time_ns() - start_cpu
            reports = workers.events({role: "REPORT" for role in ROLES}, profile)
            result["worker_reports"] = {role: packet[0] for role, packet in reports.items()}
            workers.exited({role: 0 if packet[0].get("status") == "complete" else 1 for role, packet in reports.items()})
            for role, report in result["worker_reports"].items():
                common_report(report, role, profile, case, version)
                check(report["pid"] == workers.workers[role]["process"].pid, "Worker PID differs from launched process")
            producer, consumer = (result["worker_reports"][role] for role in ROLES)
            check(all(producer[key] == consumer[key] == setup[key] for key in DEPENDENCY_FIELDS), "Library dependency identities disagree")
            result["dependency"] = {key: producer[key] for key in DEPENDENCY_FIELDS}
            for role, report in result["worker_reports"].items():
                resource_end = integer(report.get("resource_end_ns"), "resource interval end")
                check(finish_sent[role][0] <= resource_end <= reports[role][1], "Resource interval does not end at FINISH")
                active_cpu = integer(report.get("active_cpu_end_ns"), "active process CPU end")
                check(report["cpu_start_ns"] <= active_cpu <= report["cpu_end_ns"], "Active/drain process CPU intervals changed")
                check(sent_start[role][0] <= integer(report.get("cpu_start_time_ns"), "CPU interval start") <= report["data_start_ns"],
                      "Participant CPU interval does not include its START-to-T0 wait")
            result.update(derive_result(producer, consumer, profile, case, t0))
            result["control_timing"] = dict(t0_ns=t0, start_sent=sent_start, done_received_ns={role: value[1] for role, value in done.items()},
                                             finish_sent=finish_sent, coordinator_barrier_ns=done_received - min(value[0] for value in sent_start.values()))
            result["start_jitter_ns"] = {role: report["data_start_ns"] - t0 for role, report in result["worker_reports"].items()}
            result["resources"] = dict(coordinator_phase_cpu_ns=coordinator_phase_cpu,
                                        worker_process_phase_cpu_ns={role: report["cpu_end_ns"] - report["cpu_start_ns"] for role, report in result["worker_reports"].items()},
                                        worker_peak_rss_bytes={role: report.get("peak_rss_bytes") for role, report in result["worker_reports"].items()},
                                        worker_threads={role: dict(start=report.get("thread_count_start"), end=report.get("thread_count_end")) for role, report in result["worker_reports"].items()})
            result["resources"]["complete_system_phase_cpu_ns"] = coordinator_phase_cpu + sum(result["resources"]["worker_process_phase_cpu_ns"].values())
    except Exception as error:
        result["status"], result["failure"] = "failed", f"{type(error).__name__}: {error}"
    finally:
        cleanup_errors = []
        if setup_workers is not None:
            cleanup_errors.extend(setup_workers.close())
        if workers is not None:
            cleanup_errors.extend(workers.close())
        if cleanup_errors:
            result["cleanup_errors"] = cleanup_errors
            result["status"] = "failed"
        result["coordinator_attempt_cpu_ns"] = time.process_time_ns() - cpu_before
        result["children_lifetime_cpu_ns"] = children_cpu_ns() - children_before
        (directory / "report.json").write_text(json.dumps(result, indent=2, sort_keys=True) + "\n")
    return result


def rank(values, numerator, denominator):
    ordered = sorted(values)
    return ordered[(numerator * len(ordered) + denominator - 1) // denominator - 1]


def write_artifacts(output, reports, manifest, sources, settings, cases):
    with (output / "reports.jsonl").open("w", encoding="utf-8") as stream:
        for report in reports:
            stream.write(json.dumps(report, sort_keys=True) + "\n")
    columns = ("attempt", "profile", "block", "position", "order") + DIMENSIONS + ("status", "accepted", "delivered", "acknowledged", "pending", "cohort_verified_ns", "active_verified_ns", "cohort_stop_ns", "verified_prefix_ns", "active_verified_prefix_ns", "coordinator_attempt_cpu_ns", "children_lifetime_cpu_ns", "failure", "artifact_directory")
    with (output / "runs.csv").open("w", newline="", encoding="utf-8") as stream:
        writer = csv.DictWriter(stream, fieldnames=columns, extrasaction="ignore")
        writer.writeheader()
        writer.writerows(reports)
    groups = {}
    for report in reports:
        if report["status"] == "failed":
            continue
        for name, values in report.get("metrics", {}).items():
            groups.setdefault((report["profile"], *(report[key] for key in DIMENSIONS), report["status"], name, "ns"), []).append(values)
        for name, value in report.get("rates", {}).items():
            groups.setdefault((report["profile"], *(report[key] for key in DIMENSIONS), report["status"], name, "per_second"), []).append([value])
    dimensions = ("profile",) + DIMENSIONS + ("status", "metric", "unit")
    columns = dimensions + ("runs", "samples", "min_samples_per_run", "max_samples_per_run", "median_run_stat", "min_run_stat", "max_run_stat", "pooled_p99", "p99_rank", "pooled_p999", "p999_rank", "tail_population")
    with (output / "summary.csv").open("w", newline="", encoding="utf-8") as stream:
        writer = csv.DictWriter(stream, fieldnames=columns)
        writer.writeheader()
        for key, samples in sorted(groups.items()):
            values = [value for run in samples for value in run]
            per_run = [rank(run, 1, 2) for run in samples]
            row = dict(zip(dimensions, key), runs=len(samples), samples=len(values),
                       min_samples_per_run=min(map(len, samples)), max_samples_per_run=max(map(len, samples)),
                       median_run_stat=statistics.median(per_run), min_run_stat=min(per_run), max_run_stat=max(per_run),
                       tail_population="matched_complete_fresh_cohorts" if key[-3] == "complete" else "incomplete_observed_prefix")
            for name, numerator, denominator, minimum in (("p99", 99, 100, 10000), ("p999", 999, 1000, 1000000)):
                eligible = key[-1] == "ns" and key[-3] == "complete" and len(values) >= minimum
                row["pooled_" + name] = rank(values, numerator, denominator) if eligible else (
                    "not_applicable" if key[-1] != "ns" else "insufficient_samples" if key[-3] == "complete" else "incomplete_population")
                row[name + "_rank"] = (numerator * len(values) + denominator - 1) // denominator if eligible else ""
            writer.writerow(row)
    with (output / "manifest.toml").open("w", encoding="utf-8") as stream:
        for section, values in ((None, manifest), ("source_sha256", sources), ("cmake", settings)):
            if section:
                stream.write(f"\n[{section}]\n")
            for name, value in values.items():
                stream.write(f"{json.dumps(name, ensure_ascii=False)} = {json.dumps(value, ensure_ascii=False)}\n")
        for case in cases:
            stream.write("\n[[cases]]\n")
            for name, value in case.items():
                stream.write(f"{json.dumps(name)} = {json.dumps(value)}\n")


def parse_arguments():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("executable", type=Path)
    parser.add_argument("--output", type=Path, required=True, help="new result directory")
    parser.add_argument("--blocks", type=int, default=5, help="ABBA/BAAB blocks; two runs per profile per block")
    parser.add_argument("--messages", type=int, default=16)
    parser.add_argument("--histories", default="0,32,128")
    parser.add_argument("--payload-bytes", default="64,256,1024")
    parser.add_argument("--budget-ms", type=int, default=10000)
    parser.add_argument("--timeout", type=int, default=30, help="seconds for setup and both participant lifetimes per attempt")
    parser.add_argument("--socket-buffer-bytes", type=int, default=131072)
    parser.add_argument("--interval-ns", type=int, default=0)
    parser.add_argument("--start-lead-ms", type=int, default=100)
    parser.add_argument("--host-label", default="")
    parser.add_argument("--smoke", action="store_true", help="one block, two messages, H0 and 64-byte payload")
    options = parser.parse_args()
    try:
        histories = list(dict.fromkeys(int(value) for value in options.histories.split(",")))
        payloads = list(dict.fromkeys(int(value) for value in options.payload_bytes.split(",")))
        check(options.blocks > 0 and 1 <= options.messages <= 128, "Positive blocks and 1..128 messages required")
        check(histories and all(0 <= value <= 128 for value in histories), "History must be 0..128")
        check(payloads and all(value in (64, 256, 1024) for value in payloads), "Payload must be 64, 256 or 1024")
        check(1 <= options.budget_ms <= 600000 and options.timeout > 0 and 1024 <= options.socket_buffer_bytes <= (1 << 31) - 1,
              "Invalid time/buffer limit")
        check(0 <= options.interval_ns <= 1_000_000_000 and options.start_lead_ms > 0, "Invalid offered schedule/start lead")
        check(hasattr(time, "CLOCK_MONOTONIC"), "An explicit shared OS monotonic clock is required")
        executable = options.executable.resolve(strict=True)
        check(executable.is_file(), "Executable must be a regular file")
        if options.smoke:
            options.blocks, options.messages, histories, payloads = 1, 2, [0], [64]
        check((options.messages - 1) * options.interval_ns < options.budget_ms * 1_000_000,
              "The last scheduled message must precede the phase deadline")
        options.repetitions = options.blocks * 2
        cases = [dict(messages=options.messages, history=history, payload_bytes=payload,
                      interval_ns=options.interval_ns, socket_buffer_bytes=options.socket_buffer_bytes,
                      budget_ms=options.budget_ms) for history in histories for payload in payloads]
    except (ValueError, OSError, RuntimeError) as error:
        parser.error(str(error))
    return options, executable, cases


def main():
    options, executable, cases = parse_arguments()
    output = options.output.absolute()
    output.mkdir(parents=True) # Refuse any existing destination, including symlinks.
    reports, manifest, sources, settings = [], {}, {}, {}
    failed = False
    try:
        manifest, sources, settings = metadata(executable, options)
        cache = Path(manifest["cmake_cache"])
        if cache.is_file():
            for line in cache.read_text(errors="replace").splitlines():
                match = re.match(r"(CMAKE_(?:C_COMPILER|C_FLAGS(?:_[A-Z]+)?)|ANGARIA_[A-Z0-9_]+):[^=]+=(.*)", line)
                if match:
                    settings[match[1]] = match[2]
        if settings.get("CMAKE_C_COMPILER"):
            try:
                compiler = subprocess.check_output([settings["CMAKE_C_COMPILER"], "--version"],
                                                   stderr=subprocess.DEVNULL, text=True, timeout=10)
                settings["CMAKE_C_COMPILER_VERSION"] = compiler.splitlines()[0]
            except (OSError, subprocess.SubprocessError, IndexError):
                settings["CMAKE_C_COMPILER_VERSION"] = "unavailable"
        for path in sorted((ROOT / "benchmarks").glob("ipc_comparison*")):
            if path.is_file() and path.suffix in (".cpp", ".h") and " 2." not in path.name:
                sources[str(path.relative_to(ROOT))] = sha256(path)
        sources["benchmarks/run_ipc_comparison.py"] = sha256(Path(__file__))
        if (ROOT / "test/ipc_comparison.py").is_file():
            sources["test/ipc_comparison.py"] = sha256(ROOT / "test/ipc_comparison.py")
        manifest.update(status="failed", suite="ipc-comparison-1-to-1", format_version=FORMAT_VERSION,
                        planned_runs=len(cases) * options.blocks * 4, blocks=options.blocks, repetitions=options.blocks * 2,
                        participant_processes=2, producers=1, consumers=1, start_lead_ms=options.start_lead_ms,
                        clock_id="CLOCK_MONOTONIC", clock_resolution_ns=max(1, round(time.clock_getres(time.CLOCK_MONOTONIC) * 1e9)),
                        cpu_scope="Worker PROCESS CPU after START receipt before T0 wait through FINISH includes lead/control waits and library background threads; lifetime child CPU and coordinator CPU retained separately",
                        throughput_scope="Bounded cohort, not sustained capacity; common T0 and active intervals reported separately",
                        summary_method="Median/min/max of separate-run nearest-rank p50; qualified pooled tails only within identical complete fresh-cohort cells",
                        p99_min_samples=10000, p999_min_samples=1000000,
                        semantic_profiles=["Angaria acknowledged retained delivery", "classic libnanomsg IPC unacknowledged observed delivery"])
        write_artifacts(output, reports, manifest, sources, settings, cases)
        version = (ROOT / "VERSION").read_text().strip()
        for case in cases:
            for block in range(1, options.blocks + 1):
                order = ("angaria", "nanomsg", "nanomsg", "angaria") if block % 2 else (
                    "nanomsg", "angaria", "angaria", "nanomsg")
                for position, profile in enumerate(order, 1):
                    index = len(reports) + 1
                    name = f"attempt-{index:04d}-{profile}-h{case['history']}-b{case['payload_bytes']}"
                    directory = output / name
                    directory.mkdir()
                    report = attempt(executable, profile, case, directory, options, version)
                    report.update(attempt=index, block=block, position=position,
                                  order="ABBA" if block % 2 else "BAAB", artifact_directory=name)
                    reports.append(report)
                    if "dependency" in report:
                        for key, value in report["dependency"].items():
                            check(key not in manifest or manifest[key] == value, "Dependency changed within the matrix")
                            manifest[key] = value
                    write_artifacts(output, reports, manifest, sources, settings, cases)
                    print(f"Attempt {index}/{manifest['planned_runs']} {profile} H={case['history']} B={case['payload_bytes']}: {report['status']}", flush=True)
                    if report["status"] == "failed" or (options.smoke and report["status"] != "complete"):
                        failed = True
                        raise RuntimeError(report["failure"] or "Smoke cohort did not complete")
        manifest["status"] = "complete"
    except Exception as error:
        failed = True
        manifest.update(status="failed", failure=f"{type(error).__name__}: {error}")
        print(manifest["failure"], file=sys.stderr)
    finally:
        manifest.update(finished_utc=datetime.now(timezone.utc).isoformat(), recorded_runs=len(reports),
                        completed_runs=sum(r["status"] == "complete" for r in reports),
                        budget_exceeded_runs=sum(r["status"] == "budget_exceeded" for r in reports),
                        failed_runs=sum(r["status"] == "failed" for r in reports))
        write_artifacts(output, reports, manifest, sources, settings, cases)
    return 1 if failed else 0


if __name__ == "__main__":
    raise SystemExit(main())
