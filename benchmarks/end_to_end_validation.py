#!/usr/bin/env python3
"""Validate every retained qualification sample before deriving per-run results.

Binary sidecars keep the worker control pipe bounded. They are evidence, not a
cache: all warmup and measured rows are checked, and only the measured suffix
contributes to the warm-stream statistics. No failed or partial run is accepted
as a successful capacity observation, and repeated runs are never pooled here.
"""
import math
from pathlib import Path
import re
import struct
import sys

sys.dont_write_bytecode = True
from run_ipc_comparison import (ACKNOWLEDGED_PROFILES, IDENTITY_PROFILES,
                                RECORDED_PROFILES, check, common_identity,
                                integer, payload_hash, sha256)


PROFILES = (*IDENTITY_PROFILES, "nanomsg")
MAGIC = b"ANGAE2E1"
HEADER = struct.Struct("<8sQ")
ROW = {"producer": struct.Struct("<8Q"), "consumer": struct.Struct("<9Q")}
MAX_U64 = (1 << 64) - 1
COUNTERS = ("send_attempts", "receive_attempts", "backpressure_count", "credit_waits", "receive_timeouts")
RESOURCE_COUNTERS = ("cpu_user_ns", "cpu_system_ns", "minor_faults", "major_faults")


def validate_case(case):
    """Apply the same finite bounds as the optional qualification executable."""
    check(type(case) is dict, "Qualification case must be an object")
    limits = {"messages": (1, 262144), "history": (0, 65536),
              "warmup_messages": (0, 262143), "payload_bytes": (64, 1024),
              "interval_ns": (0, 1000000000), "socket_buffer_bytes": (1024, 2147483647),
              "budget_ms": (1, 600000), "ring_slots": (1, 128),
              "processing_delay_ns": (0, 1000000000), "pause_at": (0, 262143),
              "pause_ns": (0, 1000000000), "pacing_spin": (0, 1), "qualification": (1, 1)}
    for key, (minimum, maximum) in limits.items():
        value = integer(case.get(key), key, minimum)
        check(value <= maximum, "Out-of-range qualification " + key)
    check(case["payload_bytes"] in (64, 256, 1024), "Unsupported qualification payload size")
    check(case["warmup_messages"] < case["messages"], "Warmup consumes the measured population")
    measured = case["messages"] - case["warmup_messages"]
    check(case["pause_at"] == 0 or case["pause_at"] < measured, "Pause is outside the measured population")
    check(bool(case["pause_at"]) == bool(case["pause_ns"]), "Pause index and duration must both be enabled")
    check(case["processing_delay_ns"] < case["budget_ms"] * 1000000 and
          case["pause_ns"] < case["budget_ms"] * 1000000, "Application work or pause exceeds the phase budget")
    check((case["messages"] - 1) * case["interval_ns"] < case["budget_ms"] * 1000000,
          "Last scheduled message reaches the phase deadline")
    check(case.get("scenario", "concurrent") == "concurrent", "Qualification requires a concurrent stream")
    check(case.get("collection", "none") == "none", "Qualification must not enable diagnostics collection")
    return measured


def distribution(values):
    """Nearest-rank quantiles for one attempt; short cohorts have no tail claim."""
    check(bool(values), "Cannot summarize an empty measured population")
    ordered = sorted(values)
    count = len(ordered)
    return dict(count=count, min_ns=ordered[0], p50_ns=ordered[(count * 50 + 99) // 100 - 1],
                p95_ns=ordered[(count * 95 + 99) // 100 - 1],
                p99_ns=ordered[(count * 99 + 99) // 100 - 1] if count >= 10000 else None,
                p999_ns=ordered[(count * 999 + 999) // 1000 - 1] if count >= 100000 else None,
                max_ns=ordered[-1], total_ns=sum(ordered))


def sample_rows(directory, role, report, count):
    """Reject escaping paths, truncation, trailing bytes and malformed headers."""
    check(type(report.get("sample_format")) is int and report["sample_format"] == 1,
          "Unsupported binary sample format")
    name = report.get("sample_file")
    check(type(name) is str and name == role + ".samples.bin", "Invalid binary sample path")
    check(type(report.get("sample_stride")) is int and report["sample_stride"] == ROW[role].size,
          "Wrong binary sample stride")
    check(type(report.get("sample_count")) is int and report["sample_count"] == count,
          "Binary sample count differs from the full population")
    directory = Path(directory).resolve(strict=True)
    path = directory / name
    check(not path.is_symlink() and path.is_file() and path.resolve(strict=True).parent == directory,
          "Binary sample path is not a local regular file")
    expected_size = HEADER.size + count * ROW[role].size
    check(path.stat().st_size == expected_size, "Truncated or extra binary sample rows")
    with path.open("rb") as source:
        check(source.read(HEADER.size) == HEADER.pack(MAGIC, count), "Wrong binary sample magic or header count")
        for _ in range(count):
            raw = source.read(ROW[role].size)
            check(len(raw) == ROW[role].size, "Truncated binary sample row")
            yield ROW[role].unpack(raw)
        check(not source.read(1), "Extra bytes after binary sample rows")


def _report(report, role, profile, case, t0, expected_version, dependency):
    check(type(report) is dict, "Qualification report must be an object")
    common_identity(report, role, profile, case, expected_version, dependency=dependency)
    check(report.get("event") == "REPORT", "Expected a qualification REPORT")
    check(report.get("qualification") is True, "Worker omitted qualification mode")
    check("rows" not in report, "Qualification must retain rows in its binary sidecar")
    check(report.get("status") == "complete" and report.get("error") == "",
          "Unsuccessful qualification attempt; retain it without a performance claim")
    for key in ("budget_ms", "interval_ns", "warmup_messages", "processing_delay_ns", "pause_at", "pause_ns"):
        check(type(report.get(key)) is int and report[key] == case[key], "Wrong qualification report " + key)
    check(type(report.get("measured_messages")) is int and
          report["measured_messages"] == case["messages"] - case["warmup_messages"], "Wrong measured population")
    check(report.get("pacing_spin") is bool(case["pacing_spin"]), "Wrong qualification pacing mode")
    check(report.get("scenario") == "concurrent" and report.get("collection") == "none",
          "Qualification report changed the workload or enabled collection")
    check(report.get("instrumentation_enabled") is False, "Ordinary qualification must disable diagnostics instrumentation")
    check(report.get("clock_pair_samples") == 256, "Missing clock-read calibration")
    minimum = integer(report.get("clock_pair_min_ns"), "minimum clock pair")
    median = report.get("clock_pair_median_ns")
    check(type(median) in (int, float) and math.isfinite(median) and median >= minimum,
          "Invalid clock-pair median")
    check(type(report.get("clock_calibration_scope")) is str and bool(report["clock_calibration_scope"]),
          "Missing calibration scope")
    check(type(report.get("cpu_scope")) is str and "START" in report["cpu_scope"] and "FINISH" in report["cpu_scope"],
          "Missing whole-stream CPU scope")
    for key in ("data_start_ns", "data_end_ns", "cpu_start_time_ns", "resource_end_ns",
                "measured_window_start_ns", "measured_window_end_ns", "measured_resource_end_ns"):
        integer(report.get(key), key, 1)
    check(report["cpu_start_time_ns"] <= report["data_start_ns"] and
          t0 <= report["data_start_ns"] <= report["measured_window_start_ns"] <=
          report["measured_window_end_ns"] <= report["data_end_ns"] <= t0 + case["budget_ms"] * 1000000 and
          report["data_end_ns"] <= report["resource_end_ns"] == report["measured_resource_end_ns"],
          "Qualification phase boundaries are out of order or exceed the budget")
    check(integer(report.get("cpu_start_ns"), "CPU start") <=
          integer(report.get("active_cpu_end_ns"), "active CPU end") <=
          integer(report.get("cpu_end_ns"), "CPU end"), "Process CPU counters decreased")
    for key in ("peak_rss_bytes", "file_bytes_before", "file_bytes_after", "catalog_bytes_before", "catalog_bytes_after",
                "socket_send_buffer_bytes", "socket_receive_buffer_bytes", "log_limit_bytes", "ring_slots"):
        integer(report.get(key), key)
    for key in ("thread_count_start", "thread_count_end"):
        if report.get(key) is not None:
            integer(report[key], key, 1)
    for key in RESOURCE_COUNTERS:
        active = integer(report.get("measured_" + key), "measured " + key)
        check(integer(report.get("inclusive_" + key), "inclusive " + key) >= active,
              "Inclusive resource counter is smaller than its measured loop")
    for key in COUNTERS:
        check(integer(report.get("total_" + key), "total " + key) >=
              integer(report.get("measured_" + key), "measured " + key), "Counter excludes its measured suffix")
    is_producer = role == "producer"
    for prefix, population in (("total_", case["messages"]), ("measured_", report["measured_messages"])):
        own = "send_attempts" if is_producer else "receive_attempts"
        other = "receive_attempts" if is_producer else "send_attempts"
        check(report[prefix + own] >= population and report[prefix + other] == 0,
              "Attempt count contradicts the participant role or complete population")
        retry = "backpressure_count" if is_producer else "receive_timeouts"
        check(report[prefix + retry] <= report[prefix + own], "Retry count exceeds API attempts")
        check(report[prefix + ("receive_timeouts" if is_producer else "backpressure_count")] == 0,
              "Retry count includes the other role's work")
        if not is_producer or profile == "nanomsg":
            check(report[prefix + "credit_waits"] == 0, "Profile reports unsupported producer credit waits")
    expected_counts = {"accepted": case["messages"] if is_producer else 0,
                       "verified": 0 if is_producer else case["messages"],
                       "acknowledged": case["messages"] if not is_producer and profile in ACKNOWLEDGED_PROFILES else 0,
                       "completed": 0 if is_producer else case["messages"], "unexpected_messages": 0}
    for key, expected in expected_counts.items():
        check(type(report.get(key)) is int and report[key] == expected, "Wrong exact completion count: " + key)
    if profile in RECORDED_PROFILES:
        check(report.get("producer_path_scope") == "ID issuance, try_send retries and ordinary lookup" and
              report.get("lookup_observation_category") == "send", "Recorded producer omitted ordinary outcome lookup")
        expected_scope = ("reserved producer log, owner, connection and progress; catalog reported separately"
                          if profile in ACKNOWLEDGED_PROFILES else
                          "reserved producer log, owner and connection; catalog reported separately")
        check(report.get("file_byte_scope") == expected_scope, "Wrong recorded-resource accounting scope")
        check(0 < report["file_bytes_before"] == report["file_bytes_after"] and
              0 < report["catalog_bytes_before"] == report["catalog_bytes_after"],
              "Reserved recorded resources or sealed catalog changed during communication")
        check(0 < report["log_limit_bytes"] <= 512 * 1024 * 1024, "Invalid producer-log reservation")
    if profile == "nanomsg":
        check(report["file_bytes_before"] == report["file_bytes_after"] == report["catalog_bytes_before"] ==
              report["catalog_bytes_after"] == report["log_limit_bytes"] == report["ring_slots"] == 0,
              "nanomsg claims Angaria resources")
        check(report["socket_send_buffer_bytes"] == report["socket_receive_buffer_bytes"] == case["socket_buffer_bytes"],
              "nanomsg socket buffers differ from the requested bounds")
    else:
        check(report["socket_send_buffer_bytes"] == report["socket_receive_buffer_bytes"] == 0,
              "Angaria claims nanomsg socket buffers")
        check(report["ring_slots"] == case["ring_slots"],
              "Angaria ring differs from the requested bound")
    pause_start, pause_end = (integer(report.get(key), key) for key in ("pause_start_ns", "pause_end_ns"))
    if role == "consumer" and case["pause_at"]:
        check(report["measured_window_start_ns"] <= pause_start <= pause_end <= report["measured_window_end_ns"] and
              pause_end - pause_start >= case["pause_ns"], "Consumer pause was omitted or shorter than requested")
    else:
        check(pause_start == pause_end == 0, "Unexpected participant pause")


def validate_attempt(directory, profile, case, t0, producer_report, consumer_report,
                     expected_version=None, dependency=None):
    """Return independently reconstructed per-attempt statistics, or fail closed.

    Completion includes both roles: consumer payload verification, producer send
    return, ordinary recorded-ID lookup, and explicit consumer ACK where used.
    The warm suffix starts at its first producer logical-send timestamp. Paced
    streams additionally report schedule-to-delivery so source backlog is visible.
    """
    check(type(profile) is str and profile in PROFILES, "Unknown qualification profile")
    measured = validate_case(case)
    integer(t0, "coordinator T0", 1)
    check(t0 + case["budget_ms"] * 1000000 <= MAX_U64, "Qualification deadline overflows its wire clock")
    reports = {"producer": producer_report, "consumer": consumer_report}
    check(all(type(report) is dict for report in reports.values()), "Qualification reports must be objects")
    if expected_version is None:
        expected_version = producer_report.get("framework_version")
    check(type(expected_version) is str and re.fullmatch(r"\d+\.\d+\.\d+", expected_version),
          "Invalid frozen framework version")
    if dependency is not None:
        check(type(dependency) is dict and set(dependency) ==
              {"nanomsg_version", "nanomsg_commit", "nanomsg_archive_sha256"}, "Invalid frozen dependency identity")
    for role, report in reports.items():
        _report(report, role, profile, case, t0, expected_version, dependency)
    check(producer_report["pid"] != consumer_report["pid"], "Participants are not independent processes")
    check(producer_report["instrumentation_enabled"] == consumer_report["instrumentation_enabled"],
          "Participant instrumentation differs")
    metrics = {"logical_start_to_delivery_ns": [], "producer_schedule_lag_ns": [], "logical_producer_path_ns": []}
    if profile in ACKNOWLEDGED_PROFILES:
        metrics["logical_start_to_acknowledgment_ns"] = []
    if case["interval_ns"]:
        metrics["scheduled_to_delivery_ns"] = []
        if profile in ACKNOWLEDGED_PROFILES:
            metrics["scheduled_to_acknowledgment_ns"] = []
    full_start = warm_start = None
    completion = 0
    previous_producer_end = previous_consumer_end = previous_identity = incarnation = 0
    count, warmup = case["messages"], case["warmup_messages"]
    producer_rows = sample_rows(directory, "producer", producer_report, count)
    consumer_rows = sample_rows(directory, "consumer", consumer_report, count)
    for index, (sent, received) in enumerate(zip(producer_rows, consumer_rows)):
        sequence, scheduled, logical, send_end, lookup_end, digest, identity, current_incarnation = sent
        r_sequence, r_scheduled, r_logical, arrived, verified, ack_end, r_digest, r_identity, r_incarnation = received
        check(sequence == r_sequence == case["history"] + index + 1, "Sample order has a gap, duplicate or extra message")
        check(scheduled == r_scheduled == t0 + index * case["interval_ns"], "Sample scheduled time differs from fixed offered load")
        check(logical == r_logical and digest == r_digest and identity == r_identity and current_incarnation == r_incarnation,
              "Consumer payload or publication identity differs from producer")
        check(digest == int(payload_hash(sequence, scheduled, logical, case["payload_bytes"]), 16),
              "Sample payload hash differs from independent expected bytes")
        check(producer_report["data_start_ns"] <= logical and scheduled <= logical <= send_end <=
              producer_report["data_end_ns"] and previous_producer_end <= logical,
              "Producer sample clocks or sequential calls are out of order")
        check(consumer_report["data_start_ns"] <= arrived <= verified <= consumer_report["data_end_ns"] and
              logical <= arrived and previous_consumer_end <= arrived,
              "Consumer sample clocks or sequential calls are out of order")
        check(verified - arrived >= case["processing_delay_ns"], "Consumer omitted the requested application work")
        if case["pause_at"] and index == warmup + case["pause_at"]:
            check(previous_consumer_end <= consumer_report["pause_start_ns"] <=
                  consumer_report["pause_end_ns"] <= arrived, "Consumer paused at a different stream position")
        producer_end = send_end
        if profile in RECORDED_PROFILES:
            check(send_end <= lookup_end <= producer_report["data_end_ns"], "Recorded outcome lookup is missing or out of order")
            producer_end = lookup_end
        else:
            check(lookup_end == 0, "Profile reports an unsupported recorded outcome lookup")
        consumer_end = verified
        if profile in ACKNOWLEDGED_PROFILES:
            check(verified <= ack_end <= consumer_report["data_end_ns"] and ack_end != 0,
                  "Explicit acknowledgment is missing or precedes verification")
            consumer_end = ack_end
        else:
            check(ack_end == 0, "Unacknowledged profile fabricates an acknowledgment")
        if profile in IDENTITY_PROFILES:
            check(identity > 0 and current_incarnation > 0 and
                  (not previous_identity or identity == previous_identity + 1) and
                  (not incarnation or current_incarnation == incarnation), "Publication identity is invalid or not contiguous")
        else:
            check(identity == current_incarnation == 0, "nanomsg reports an Angaria publication identity")
        if full_start is None:
            full_start = logical
        if index == warmup:
            warm_start = logical
            check(previous_producer_end <= producer_report["measured_window_start_ns"] <= logical and
                  previous_consumer_end <= consumer_report["measured_window_start_ns"] <= arrived,
                  "Measured resource windows omit their first measured operation")
        previous_identity, incarnation = identity, current_incarnation
        previous_producer_end, previous_consumer_end = producer_end, consumer_end
        if index >= warmup:
            completion = max(completion, producer_end, consumer_end)
            metrics["logical_start_to_delivery_ns"].append(arrived - logical)
            metrics["producer_schedule_lag_ns"].append(logical - scheduled)
            metrics["logical_producer_path_ns"].append(producer_end - logical)
            if profile in ACKNOWLEDGED_PROFILES:
                metrics["logical_start_to_acknowledgment_ns"].append(ack_end - logical)
            if case["interval_ns"]:
                metrics["scheduled_to_delivery_ns"].append(arrived - scheduled)
                if profile in ACKNOWLEDGED_PROFILES:
                    metrics["scheduled_to_acknowledgment_ns"].append(ack_end - scheduled)
    # Advance both generators through their final EOF checks. zip stops after
    # the first one ends; a sidecar changed during reading must still fail.
    check(next(producer_rows, None) is None and next(consumer_rows, None) is None, "Unmatched binary sample rows")
    check(warm_start is not None and full_start is not None and completion > warm_start,
          "Missing or nonpositive measured completion window")
    check(previous_producer_end <= producer_report["measured_window_end_ns"] and
          previous_consumer_end <= consumer_report["measured_window_end_ns"],
          "Measured resource windows omit the final completion")
    windows = dict(measured_start_ns=warm_start, measured_end_ns=completion,
                   measured_elapsed_ns=completion - warm_start, cohort_start_ns=t0,
                   cohort_elapsed_ns=completion - t0, active_all_start_ns=full_start,
                   active_all_elapsed_ns=completion - full_start)
    rates = dict(measured_completed_per_second=measured * 1e9 / windows["measured_elapsed_ns"],
                 bounded_cohort_completed_per_second=count * 1e9 / windows["cohort_elapsed_ns"],
                 active_all_completed_per_second=count * 1e9 / windows["active_all_elapsed_ns"])
    resources, counters, samples = {}, {}, {}
    for role, report in reports.items():
        active_cpu = sum(report["measured_" + key] for key in ("cpu_user_ns", "cpu_system_ns"))
        inclusive_cpu = sum(report["inclusive_" + key] for key in ("cpu_user_ns", "cpu_system_ns"))
        resources[role] = dict(active_cpu_ns=active_cpu, inclusive_cpu_ns=inclusive_cpu,
                               active_cpu_ns_per_publication=active_cpu / measured,
                               inclusive_cpu_ns_per_publication=inclusive_cpu / measured,
                               minor_faults=report["measured_minor_faults"], major_faults=report["measured_major_faults"],
                               inclusive_minor_faults=report["inclusive_minor_faults"],
                               inclusive_major_faults=report["inclusive_major_faults"],
                               window_start_ns=report["measured_window_start_ns"],
                               window_end_ns=report["measured_window_end_ns"],
                               resource_end_ns=report["measured_resource_end_ns"], peak_rss_bytes=report["peak_rss_bytes"])
        counters[role] = {prefix + key: report[prefix + key] for prefix in ("total_", "measured_") for key in COUNTERS}
        samples[role] = dict(file=report["sample_file"], format=1, stride=ROW[role].size,
                             count=count, sha256=sha256(Path(directory) / report["sample_file"]))
    total_keys = ("active_cpu_ns", "inclusive_cpu_ns", "active_cpu_ns_per_publication", "inclusive_cpu_ns_per_publication",
                  "minor_faults", "major_faults", "inclusive_minor_faults", "inclusive_major_faults")
    resources["totals"] = {key: sum(resources[role][key] for role in reports) for key in total_keys}
    resources["totals"]["inclusive_cpu_per_completed_window_core_equivalents"] = (
        resources["totals"]["inclusive_cpu_ns"] / windows["measured_elapsed_ns"])
    resources["scope"] = ("Per-role RUSAGE_SELF includes every library thread. Active counters cover that role's measured "
                          "loop; inclusive counters continue from the same warm boundary through coordinated FINISH, "
                          "including library and control tail. CPU per publication is not isolated API cost. Role windows "
                          "differ; summed CPU divided by the common completion window is a descriptive ratio.")
    return dict(status="complete", accepted=count, delivered=count,
                acknowledged=count if profile in ACKNOWLEDGED_PROFILES else 0, pending=0,
                measured_count=measured, warmup_messages=warmup,
                metrics={name: distribution(values) for name, values in metrics.items()},
                rates=rates, windows=windows, resources=resources, counters=counters, samples=samples)
