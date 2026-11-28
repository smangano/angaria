#!/usr/bin/env python3
"""Check paired order, version routing, and raw-evidence summary integrity.

Synthetic benchmark records exercise coordinator plumbing, not messaging.
The existing qualification validator independently checks real binary samples.
"""
from collections import Counter
from copy import deepcopy
import json
from pathlib import Path
import sys
import tempfile
import unittest
from unittest import mock

sys.dont_write_bytecode = True
ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "benchmarks"))
import ownership_performance as study


def measurement(request):
    value = request["round"] * (200 if request["revision"] == "head" else 100)
    return dict(status="complete", measured_count=16384,
                rates=dict(measured_completed_per_second=value),
                metrics=dict(logical_start_to_delivery_ns=dict(count=16384, p50_ns=value, p99_ns=value * 2),
                             logical_producer_path_ns=dict(count=16384, p50_ns=value, p99_ns=value * 3)),
                resources=dict(totals=dict(inclusive_cpu_ns_per_publication=value)),
                counters={})


class OwnershipPerformance(unittest.TestCase):
    def reports(self):
        return [dict(request, **measurement(request)) for request in study.plan()[1]]

    def test_every_profile_revision_occupies_all_six_positions_and_balances_ab_ba(self):
        cases, attempts = study.plan()
        case = cases["saturation-64"]
        self.assertEqual((case["messages"], case["warmup_messages"], case["history"],
                          case["ring_slots"], case["payload_bytes"]), (20480, 4096, 8192, 8, 64))
        self.assertEqual([row["attempt"] for row in attempts], list(range(1, 37)))
        positions = Counter((row["profile"], row["revision"], row["position"]) for row in attempts)
        self.assertEqual(positions, Counter({(p, r, position): 1 for p in study.PROFILES
                                            for r in study.REVISIONS for position in range(1, 7)}))
        orders = Counter()
        for number in range(1, 19):
            pair = [row for row in attempts if row["pair"] == number]
            self.assertEqual(len(pair), 2)
            self.assertEqual(pair[0]["profile"], pair[1]["profile"])
            self.assertEqual(pair[0]["round"], pair[1]["round"])
            self.assertEqual(pair[1]["attempt"], pair[0]["attempt"] + 1)
            orders[pair[0]["profile"], tuple(row["revision"] for row in pair)] += 1
        self.assertEqual(orders, Counter({(p, order): 3 for p in study.PROFILES
                                         for order in (("base", "head"), ("head", "base"))}))

    def test_summary_preserves_all_runs_and_pairs_by_round_not_adjacent_list_index(self):
        reports = self.reports()
        summary = study.summaries(reports, {})
        public = next(row for row in summary["paired"] if row["profile"] == "public-recorded"
                      and row["metric"] == "completed_rate")
        self.assertEqual(public["median_head_over_base"], 2)
        self.assertEqual([pair["round"] for pair in public["pairs"]], list(range(1, 7)))
        self.assertEqual([pair["difference"] for pair in public["pairs"]], [100, 200, 300, 400, 500, 600])
        # An extreme first BASE run stays in the range and in its own pair.
        reports[0]["rates"]["measured_completed_per_second"] = 1
        summary = study.summaries(reports, {})
        row = next(row for row in summary["rows"] if row["revision"] == "base" and
                   row["profile"] == "public-recorded" and row["metric"] == "completed_rate")
        self.assertEqual((row["runs"], row["median"], row["minimum"], row["maximum"]), (6, 350, 1, 600))
        self.assertEqual(len(row["attempt_values"]), 6)

    def test_missing_failed_or_reordered_attempt_prevents_an_improvement_summary(self):
        reports = self.reports()
        failed = deepcopy(reports)
        failed[0]["status"] = "failed"
        for invalid in (failed, reports[:-1], [reports[1], reports[0], *reports[2:]]):
            with self.subTest(records=len(invalid)):
                with self.assertRaisesRegex(RuntimeError, "every successful planned BASE/HEAD pair"):
                    study.summaries(invalid, {})

    def test_zero_baseline_quantile_is_kept_without_inventing_a_relative_speedup(self):
        reports = self.reports()
        reports[0]["metrics"]["logical_start_to_delivery_ns"]["p50_ns"] = 0
        summary = study.summaries(reports, {})
        row = next(row for row in summary["paired"] if row["profile"] == "public-recorded"
                   and row["metric"] == "delivery_p50")
        self.assertIsNone(row["median_head_over_base"])
        self.assertIsNone(row["pairs"][0]["head_over_base"])
        self.assertEqual(len(row["pairs"]), 6)

    def test_reconstruction_routes_each_archive_to_its_own_frozen_framework_version(self):
        with tempfile.TemporaryDirectory() as temporary:
            output = Path(temporary)
            (output / "raw").mkdir()
            cases, attempts = study.plan()
            revisions = {revision: dict(source=dict(version=version))
                         for revision, version in (("base", "0.4.39"), ("head", "0.4.40"))}
            manifest = dict(format=1, suite=study.SUITE, complete=True, cases=cases, attempts=attempts,
                            revisions=revisions, dependency={}, raw_sha256={})
            reports = []
            for request in attempts:
                directory = output / "fixture"
                directory.mkdir(exist_ok=True)
                study.qualification.write_json(directory / "evidence.json", request)
                artifact = "raw/attempt-%04d.tar.gz" % request["attempt"]
                study.qualification.archive(directory, output / artifact)
                manifest["raw_sha256"][artifact] = study.ipc.sha256(output / artifact)
                reports.append(dict(request, artifact=artifact, **measurement(request)))
            study.qualification.write_json(output / "manifest.json", manifest)
            (output / "reports.jsonl").write_text("\n".join(
                json.dumps(row, sort_keys=True) for row in reports) + "\n")
            def validate(directory, profile, case, evidence, version, dependency):
                self.assertEqual(version, revisions[evidence["revision"]]["source"]["version"])
                self.assertEqual(profile, evidence["profile"])
                return measurement(evidence)
            with mock.patch.object(study.qualification, "validate_evidence", side_effect=validate) as validating:
                self.assertEqual(study.reconstruct(output)[0], reports)
                self.assertEqual(validating.call_count, 36)
                (output / reports[0]["artifact"]).write_bytes(b"changed raw archive")
                with self.assertRaisesRegex(RuntimeError, "identity/hash mismatch"):
                    study.reconstruct(output)


if __name__ == "__main__":
    unittest.main()
