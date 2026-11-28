#!/usr/bin/env python3
"""Check the matched control schedule and honest per-run summarization.

These synthetic private benchmark records test analysis plumbing, not Angaria
messaging. Real collection revalidates binary samples from every raw archive.
"""
from collections import Counter
from copy import deepcopy
from pathlib import Path
import sys
import unittest

sys.dont_write_bytecode = True
ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "benchmarks"))
import public_runtime_controls as controls


class PublicRuntimeControls(unittest.TestCase):
    def reports(self):
        _, attempts = controls.plan()
        reports = []
        for request in attempts:
            value = request["round"] * 100
            reports.append(dict(request, status="complete",
                rates=dict(measured_completed_per_second=value),
                metrics=dict(logical_start_to_delivery_ns=dict(p50_ns=value, p99_ns=value * 2),
                             logical_producer_path_ns=dict(p50_ns=value, p99_ns=value * 3)),
                resources=dict(totals=dict(inclusive_cpu_ns_per_publication=value)),
                counters=dict(producer=dict(measured_backpressure_count=value))))
        return reports

    def test_25_attempts_balance_each_profile_across_every_position(self):
        cases, attempts = controls.plan()
        self.assertEqual(set(cases), {"saturation-64"})
        case = cases["saturation-64"]
        self.assertEqual((case["messages"], case["warmup_messages"], case["history"],
                          case["ring_slots"], case["payload_bytes"]), (20480, 4096, 8192, 8, 64))
        self.assertEqual((case["qualification"], case["interval_ns"], case["processing_delay_ns"],
                          case["pause_ns"]), (1, 0, 0, 0))
        self.assertEqual(len(attempts), 25)
        self.assertEqual([row["attempt"] for row in attempts], list(range(1, 26)))
        self.assertEqual(Counter(row["profile"] for row in attempts), Counter({p: 5 for p in controls.PROFILES}))
        self.assertEqual(Counter((row["profile"], row["position"]) for row in attempts),
                         Counter({(p, position): 1 for p in controls.PROFILES for position in range(1, 6)}))
        for rotation in range(5):
            ordered = [row for row in attempts if row["round"] == rotation + 1]
            self.assertEqual([row["profile"] for row in ordered],
                             list(controls.PROFILES[rotation:] + controls.PROFILES[:rotation]))

    def test_summary_keeps_full_range_instead_of_pooling_or_dropping_a_slow_run(self):
        reports = self.reports()
        reports[0]["rates"]["measured_completed_per_second"] = 1
        summary = controls.summarize(reports)
        public = summary["profiles"]["public-recorded"]
        self.assertEqual(public["runs"], 5)
        self.assertEqual(public["metrics"]["completed_per_second"], dict(median=300, minimum=1, maximum=500))
        self.assertEqual(public["metrics"]["delivery_p99_ns"], dict(median=600, minimum=200, maximum=1000))

    def test_failed_missing_or_reordered_attempt_prevents_a_complete_summary(self):
        reports = self.reports()
        failed = deepcopy(reports)
        failed[0]["status"] = "failed"
        for invalid in (failed, reports[:-1], [reports[1], reports[0], *reports[2:]]):
            with self.subTest(records=len(invalid)):
                with self.assertRaisesRegex(RuntimeError, "every successful planned attempt"):
                    controls.summarize(invalid)


if __name__ == "__main__":
    unittest.main()
