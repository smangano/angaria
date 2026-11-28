#!/usr/bin/env python3
"""Independent golden sidecars and corrupt evidence for the qualification tool.

These are private benchmark-format tests, not public Angaria usage examples.
Literal little-endian test records demonstrate which clocks and completion
boundaries the reports must retain. No production C++ encoder creates fixtures.
"""
import copy
from pathlib import Path
import struct
import sys
import tempfile
import unittest
from unittest import mock

sys.dont_write_bytecode = True
ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "benchmarks"))
import end_to_end_validation as validation
import run_ipc_comparison as ipc


def golden_hash(sequence, scheduled, logical, size):
    # This intentionally does not call the validator's payload helper. The
    # fixed version/type/flags bytes are followed by explicit LE clock fields.
    payload = bytearray(b"\x01\x00\x01\x00\x00\x00\x00\x00")
    for value in (sequence, scheduled, logical):
        payload.extend(value.to_bytes(8, "little"))
    payload.extend((sequence + index) % 256 for index in range(32, size))
    result = 14695981039346656037
    for value in payload:
        result = ((result ^ value) * 1099511628211) % (1 << 64)
    return result


class EndToEndValidation(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.directory = Path(self.temporary.name)
        self.profile = "recorded-connection"
        self.t0 = 1000000000
        self.case = dict(messages=5, warmup_messages=1, history=2, payload_bytes=64,
                         interval_ns=0, socket_buffer_bytes=131072, budget_ms=1000,
                         ring_slots=8, processing_delay_ns=0, pause_at=0, pause_ns=0,
                         pacing_spin=0, qualification=1, scenario="concurrent", collection="none")
        self.prepare()

    def prepare(self):
        self.sent, self.received = [], []
        for index in range(self.case["messages"]):
            sequence = self.case["history"] + index + 1
            scheduled = self.t0 + index * self.case["interval_ns"]
            logical = max(self.t0 + 100 + index * 100000, scheduled + 100)
            digest = golden_hash(sequence, scheduled, logical, self.case["payload_bytes"])
            recorded = self.profile in ipc.LOOKUP_PROFILES
            identified = self.profile in ipc.IDENTITY_PROFILES
            self.sent.append([sequence, scheduled, logical, logical + 100,
                              logical + 110 if recorded else 0, digest,
                              sequence if identified else 0, 7 if identified else 0])
            self.received.append([sequence, scheduled, logical, logical + 200, logical + 220,
                                  logical + 230 if self.profile in ipc.ACKNOWLEDGED_PROFILES else 0,
                                  digest, sequence if identified else 0, 7 if identified else 0])
        self.reports = {role: self.report(role) for role in ("producer", "consumer")}
        self.write()

    def report(self, role):
        producer = role == "producer"
        recorded = self.profile in ipc.LOOKUP_PROFILES
        nanomsg = self.profile == "nanomsg"
        n, w = self.case["messages"], self.case["warmup_messages"]
        first = self.sent[w][2] if producer else self.received[w][3]
        last = max(self.sent[-1][3:5]) if producer else max(self.received[-1][4:6])
        data_end = max(self.sent[-1][2], self.received[-1][4]) + 500
        result = dict(event="REPORT", format_version=1, profile=self.profile, role=role,
                      framework_version=(ROOT / "VERSION").read_text().strip(),
                      scenario="concurrent", collection="none", instrumentation_enabled=False,
                      pid=10 if producer else 11, status="complete", error="",
                      clock_id="CLOCK_MONOTONIC", clock_resolution_ns=1, clock_pair_samples=256,
                      clock_pair_min_ns=1, clock_pair_median_ns=2,
                      clock_calibration_scope="Independent consecutive reads",
                      cpu_scope="RUSAGE_SELF from START through FINISH, including library threads",
                      qualification=True, measured_messages=n-w, pacing_spin=bool(self.case["pacing_spin"]),
                      accepted=n if producer else 0, verified=0 if producer else n,
                      acknowledged=n if not producer and self.profile in ipc.ACKNOWLEDGED_PROFILES else 0,
                      completed=0 if producer else n, unexpected_messages=0,
                      data_start_ns=self.t0, data_end_ns=data_end, cpu_start_time_ns=self.t0-100,
                      cpu_start_ns=100, active_cpu_end_ns=500, cpu_end_ns=600,
                      resource_end_ns=data_end+500, measured_window_start_ns=first-10,
                      measured_window_end_ns=last+100, measured_resource_end_ns=data_end+500,
                      measured_cpu_user_ns=100, measured_cpu_system_ns=50,
                      measured_minor_faults=2, measured_major_faults=0,
                      inclusive_cpu_user_ns=150, inclusive_cpu_system_ns=70,
                      inclusive_minor_faults=3, inclusive_major_faults=0,
                      peak_rss_bytes=65536, thread_count_start=1, thread_count_end=1,
                      file_bytes_before=0 if nanomsg else 80*1024*1024,
                      file_bytes_after=0 if nanomsg else 80*1024*1024,
                      catalog_bytes_before=4096 if recorded else 0,
                      catalog_bytes_after=4096 if recorded else 0,
                      socket_send_buffer_bytes=self.case["socket_buffer_bytes"] if nanomsg else 0,
                      socket_receive_buffer_bytes=self.case["socket_buffer_bytes"] if nanomsg else 0,
                      log_limit_bytes=0 if nanomsg else 64*1024*1024,
                      ring_slots=0 if nanomsg else self.case["ring_slots"],
                      pause_start_ns=0, pause_end_ns=0,
                      sample_format=1, sample_file=role+".samples.bin", sample_count=n,
                      sample_stride=64 if producer else 72)
        for key in ("messages", "history", "payload_bytes", "interval_ns", "budget_ms", "warmup_messages",
                    "processing_delay_ns", "pause_at", "pause_ns"):
            result[key] = self.case[key]
        for prefix, population in (("total_", n), ("measured_", n-w)):
            result.update({prefix+"send_attempts": population if producer else 0,
                           prefix+"receive_attempts": 0 if producer else population,
                           prefix+"backpressure_count": 0, prefix+"credit_waits": 0,
                           prefix+"receive_timeouts": 0})
        result.update(ipc.pinned_dependency())
        for kind in ("runtime", "header"):
            for part, number in (("current", 6), ("revision", 1), ("age", 0)):
                result["nanomsg_"+kind+"_abi_"+part] = number
        if recorded:
            result.update(producer_path_scope="ID issuance, try_send retries and ordinary lookup",
                          lookup_observation_category="send",
                          file_byte_scope=("reserved producer log, owner, connection and progress; catalog reported separately"
                                           if self.profile in ipc.ACKNOWLEDGED_PROFILES else
                                           "reserved producer log, owner and connection; catalog reported separately"))
        if self.profile in ipc.PUBLIC_PROFILES:
            result.update(runtime_surface="Session/Task/QueueWriter/SourceQueueSet",
                          producer_path_scope="public ID issuance, send retries and publication_status",
                          file_byte_scope="all reserved public messaging resources; catalog reported separately")
        return result

    def write(self):
        for role, rows, pattern in (("producer", self.sent, "<8Q"), ("consumer", self.received, "<9Q")):
            # Explicit golden bytes; changing production structs cannot make
            # a bad encoder and its validator agree through a shared fixture.
            data = b"ANGAE2E1" + len(rows).to_bytes(8, "little")
            data += b"".join(struct.pack(pattern, *row) for row in rows)
            (self.directory / (role+".samples.bin")).write_bytes(data)

    def validate(self):
        return validation.validate_attempt(self.directory, self.profile, self.case, self.t0,
                                            self.reports["producer"], self.reports["consumer"])

    def reject(self):
        with self.assertRaises(RuntimeError):
            self.validate()

    def test_warm_suffix_retains_and_checks_the_complete_original_sidecars(self):
        self.received[0][3] = self.sent[0][2] + 10000
        self.received[0][4] = self.sent[0][2] + 10010
        self.write()
        before = {role: (self.directory/(role+".samples.bin")).read_bytes() for role in self.reports}
        result = self.validate()
        self.assertEqual((result["accepted"], result["measured_count"]), (5, 4))
        self.assertEqual(result["metrics"]["logical_start_to_delivery_ns"]["p50_ns"], 200)
        self.assertEqual(result["metrics"]["logical_start_to_delivery_ns"]["count"], 4)
        self.assertIsNone(result["metrics"]["logical_start_to_delivery_ns"]["p99_ns"])
        self.assertNotIn("scheduled_to_delivery_ns", result["metrics"])
        for role in self.reports:
            self.assertEqual((self.directory/(role+".samples.bin")).read_bytes(), before[role])
            self.assertEqual(result["samples"][role]["count"], 5)
        # Corruption in excluded warmup still invalidates the whole attempt.
        self.sent[0][5] ^= 1
        self.write()
        self.reject()

    def test_each_profile_has_its_real_ack_and_identity_boundary(self):
        for profile in (*ipc.LOOKUP_PROFILES, "nanomsg", "angaria", "recorded-spsc"):
            with self.subTest(profile=profile):
                self.profile = profile
                self.prepare()
                result = self.validate()
                self.assertEqual(result["acknowledged"], 5 if profile in ipc.ACKNOWLEDGED_PROFILES else 0)
                self.assertEqual("logical_start_to_acknowledgment_ns" in result["metrics"], profile in ipc.ACKNOWLEDGED_PROFILES)

    def test_public_qualification_requires_public_surface_and_complete_lookup(self):
        # A public comparison cannot quietly substitute a private endpoint or
        # omit its ordinary public acceptance-status lookup from completion.
        self.profile = "public-recorded"
        self.prepare()
        del self.reports["producer"]["runtime_surface"]
        self.reject()
        self.prepare()
        self.sent[-1][4] = 0
        self.write()
        self.reject()

    def test_truncated_extra_and_header_mismatch_are_rejected(self):
        path = self.directory / "consumer.samples.bin"
        original = path.read_bytes()
        for damaged in (original[:-1], original+b"\x00", b"WRONG!!!"+original[8:],
                        original[:8]+(4).to_bytes(8, "little")+original[16:]):
            with self.subTest(size=len(damaged)):
                path.write_bytes(damaged)
                self.reject()

    def test_fabricated_ack_series_and_missing_real_ack_are_rejected(self):
        for row in self.received:
            row[5] = row[4] + 10
        self.reports["consumer"]["acknowledged"] = 5
        self.write()
        self.reject()
        self.profile = "recorded-acknowledged-connection"
        self.prepare()
        self.received[-1][5] = 0
        self.write()
        self.reject()

    def test_producer_lookup_return_cannot_hide_behind_a_faster_consumer(self):
        baseline = self.validate()
        self.sent[-1][4] += 200
        self.write()
        for key in ("measured_window_end_ns", "data_end_ns", "resource_end_ns", "measured_resource_end_ns"):
            self.reports["producer"][key] += 200
        delayed = self.validate()
        self.assertEqual(delayed["windows"]["measured_end_ns"], self.sent[-1][4])
        self.assertLess(delayed["rates"]["measured_completed_per_second"], baseline["rates"]["measured_completed_per_second"])

    def test_explicit_ack_return_is_in_the_completed_rate(self):
        self.profile = "recorded-acknowledged-connection"
        self.prepare()
        baseline = self.validate()
        self.received[-1][5] += 200
        self.write()
        for key in ("measured_window_end_ns", "data_end_ns", "resource_end_ns", "measured_resource_end_ns"):
            self.reports["consumer"][key] += 200
        delayed = self.validate()
        self.assertEqual(delayed["windows"]["measured_end_ns"], self.received[-1][5])
        self.assertLess(delayed["rates"]["measured_completed_per_second"], baseline["rates"]["measured_completed_per_second"])

    def test_nanomsg_send_return_also_participates_in_completed_rate(self):
        self.profile = "nanomsg"
        self.prepare()
        baseline = self.validate()
        self.sent[-1][3] += 300
        self.write()
        for key in ("measured_window_end_ns", "data_end_ns", "resource_end_ns", "measured_resource_end_ns"):
            self.reports["producer"][key] += 300
        delayed = self.validate()
        self.assertEqual(delayed["windows"]["measured_end_ns"], self.sent[-1][3])
        self.assertLess(delayed["rates"]["measured_completed_per_second"], baseline["rates"]["measured_completed_per_second"])

    def test_paced_latency_includes_schedule_debt(self):
        self.case["interval_ns"] = 50000
        self.prepare()
        result = self.validate()
        logical = result["metrics"]["logical_start_to_delivery_ns"]["p50_ns"]
        scheduled = result["metrics"]["scheduled_to_delivery_ns"]["p50_ns"]
        self.assertGreater(scheduled, logical)
        self.assertEqual(result["metrics"]["producer_schedule_lag_ns"]["p50_ns"]+logical, scheduled)
        # Matching both role copies and recomputing their hash still cannot
        # change the declared fixed offered-load schedule.
        self.sent[2][1] += 1
        self.received[2][1] += 1
        digest = golden_hash(self.sent[2][0], self.sent[2][1], self.sent[2][2], 64)
        self.sent[2][5] = self.received[2][6] = digest
        self.write()
        self.reject()

    def test_hash_and_cross_role_publication_identity_are_checked(self):
        for column in (6, 7, 8):
            with self.subTest(column=column):
                self.prepare()
                self.received[3][column] ^= 1
                self.write()
                self.reject()

    def test_contiguous_order_is_checked_even_with_matching_role_copies(self):
        self.sent[3][0] += 1
        self.received[3][0] += 1
        self.write()
        self.reject()

    def test_incomplete_counts_status_and_unexpected_messages_are_rejected(self):
        for role, key, value in (("producer", "accepted", 4), ("consumer", "verified", 4),
                                  ("consumer", "completed", 4), ("consumer", "unexpected_messages", 1),
                                  ("producer", "status", "budget_exceeded")):
            with self.subTest(key=key):
                self.prepare()
                self.reports[role][key] = value
                self.reject()

    def test_cpu_fault_and_flow_counter_reversals_are_rejected(self):
        for key, value in (("inclusive_cpu_user_ns", 1), ("inclusive_minor_faults", 0),
                           ("measured_major_faults", -1), ("cpu_end_ns", 0),
                           ("measured_send_attempts", 6), ("total_receive_attempts", 1)):
            with self.subTest(key=key):
                self.prepare()
                self.reports["producer"][key] = value
                self.reject()

    def test_cpu_resources_are_per_role_and_include_finishing_tail(self):
        result = self.validate()
        self.assertEqual(result["resources"]["producer"]["active_cpu_ns_per_publication"], 150/4)
        self.assertEqual(result["resources"]["producer"]["inclusive_cpu_ns_per_publication"], 220/4)
        self.assertEqual(result["resources"]["totals"]["inclusive_cpu_ns"], 440)
        self.assertEqual(result["resources"]["totals"]["minor_faults"], 4)

    def test_sidecar_cannot_escape_or_redirect_the_attempt_directory(self):
        original = self.reports["producer"]["sample_file"]
        for name in ("../producer.samples.bin", str(self.directory/original), "subdir/../"+original):
            with self.subTest(name=name):
                self.reports["producer"]["sample_file"] = name
                self.reject()
        self.reports["producer"]["sample_file"] = original
        outside = self.directory / "outside.bin"
        path = self.directory / original
        path.rename(outside)
        path.symlink_to(outside)
        self.reject()

    def test_fixed_case_bounds_and_budget_are_enforced(self):
        for key, value in (("messages", 262145), ("warmup_messages", 5), ("history", 65537),
                           ("payload_bytes", 65), ("qualification", True), ("ring_slots", 0),
                           ("processing_delay_ns", 1000000001), ("pause_ns", 1)):
            with self.subTest(key=key):
                case = copy.deepcopy(self.case)
                case[key] = value
                with self.assertRaises(RuntimeError):
                    validation.validate_case(case)
        self.reports["producer"]["data_end_ns"] = self.t0+self.case["budget_ms"]*1000000+1
        self.reject()

    def test_report_and_sample_contract_tampering_is_rejected(self):
        for key, value in (("sample_count", 4), ("sample_stride", 72), ("sample_format", 2),
                           ("measured_messages", 5), ("pacing_spin", 0), ("budget_ms", 999),
                           ("catalog_bytes_after", 4097), ("producer_path_scope", "send only"),
                           ("instrumentation_enabled", True),
                           ("nanomsg_commit", "0"*40), ("framework_version", "0.0.0")):
            with self.subTest(key=key):
                self.prepare()
                self.reports["producer"][key] = value
                self.reject()

    def test_requested_consumer_work_cannot_be_skipped(self):
        self.case["processing_delay_ns"] = 1000
        self.prepare()
        self.reject()

    def test_requested_pause_occurs_at_the_declared_measured_position(self):
        self.case.update(pause_at=2, pause_ns=1000)
        self.prepare()
        consumer = self.reports["consumer"]
        target = self.case["warmup_messages"] + self.case["pause_at"]
        consumer["pause_start_ns"] = self.received[target-1][4] + 100
        consumer["pause_end_ns"] = consumer["pause_start_ns"] + 1000
        self.validate()
        # A pause of the correct duration at the wrong stream position must
        # not masquerade as the configured backpressure experiment.
        consumer["pause_start_ns"] = self.received[target-2][4] + 100
        consumer["pause_end_ns"] = consumer["pause_start_ns"] + 1000
        self.reject()

    def test_resource_window_cannot_reinclude_warmup_work(self):
        self.reports["producer"]["measured_window_start_ns"] = self.t0
        self.reject()

    def test_retained_source_version_and_dependency_allow_later_reconstruction(self):
        pins = ipc.pinned_dependency()
        pins["nanomsg_commit"] = "a" * 40
        for report in self.reports.values():
            report["framework_version"] = "0.4.1"
            report.update(pins)
        with mock.patch.object(ipc, "pinned_dependency", side_effect=AssertionError("live source pin read")):
            result = validation.validate_attempt(self.directory, self.profile, self.case, self.t0,
                                                   self.reports["producer"], self.reports["consumer"],
                                                   expected_version="0.4.1", dependency=pins)
        self.assertEqual(result["status"], "complete")
        with self.assertRaises(RuntimeError):
            validation.validate_attempt(self.directory, self.profile, self.case, self.t0,
                                          self.reports["producer"], self.reports["consumer"],
                                          expected_version="0.4.2", dependency=pins)

    def test_tail_quantiles_require_enough_samples_and_use_nearest_rank(self):
        short = validation.distribution(list(range(9999)))
        self.assertIsNone(short["p99_ns"])
        regular = validation.distribution(list(range(10000)))
        self.assertEqual((regular["p50_ns"], regular["p95_ns"], regular["p99_ns"]), (4999, 9499, 9899))
        self.assertIsNone(regular["p999_ns"])
        long = validation.distribution(list(range(100000)))
        self.assertEqual(long["p999_ns"], 99899)


if __name__ == "__main__":
    unittest.main()
