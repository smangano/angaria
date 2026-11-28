#!/usr/bin/env python3
"""Check qualification scheduling and safe retained-evidence reconstruction.

The archive fixtures are deliberately hostile private benchmark artifacts.
They do not create transport files or exercise the public Angaria API.
"""
from collections import Counter
import gzip
import io
from pathlib import Path
import sys
import tarfile
import tempfile
import unittest

sys.dont_write_bytecode = True
ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "benchmarks"))
import run_end_to_end as coordinator


class EndToEndCoordinator(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.directory = Path(self.temporary.name)
        self.output = self.directory / "extracted"
        self.output.mkdir()
        self.archive = self.directory / "raw.tar.gz"

    def write_archive(self, entries):
        with tarfile.open(self.archive, "w:gz") as target:
            for entry in entries:
                member = tarfile.TarInfo(entry["name"])
                member.type = entry.get("type", tarfile.REGTYPE)
                member.linkname = entry.get("linkname", "")
                data = entry.get("data", b"")
                member.size = len(data)
                target.addfile(member, io.BytesIO(data))

    def test_flat_regular_evidence_extracts_without_changing_bytes(self):
        self.write_archive([dict(name="evidence.json", data=b'{"complete":true}\n'),
                            dict(name="producer.samples.bin", data=b"\x00\xff\x01"),
                            dict(name="producer.stderr", data=b"")])
        coordinator.extract(self.archive, self.output)
        self.assertEqual((self.output/"producer.samples.bin").read_bytes(), b"\x00\xff\x01")
        self.assertEqual((self.output/"evidence.json").read_bytes(), b'{"complete":true}\n')
        self.assertEqual(len(list(self.output.iterdir())), 3)

    def test_traversal_absolute_nested_and_dot_names_are_rejected(self):
        for name in ("../escape", "/escape", "nested/evidence.json", ".", ".."):
            with self.subTest(name=name):
                self.write_archive([dict(name=name, data=b"bad")])
                with self.assertRaises(RuntimeError):
                    coordinator.extract(self.archive, self.output)
                self.assertFalse((self.directory/"escape").exists())

    def test_duplicate_archive_names_are_rejected(self):
        self.write_archive([dict(name="evidence.json", data=b"first"),
                            dict(name="evidence.json", data=b"replacement")])
        with self.assertRaises(RuntimeError):
            coordinator.extract(self.archive, self.output)
        self.assertEqual((self.output/"evidence.json").read_bytes(), b"first")

    def test_symlinks_hardlinks_directories_and_devices_are_rejected(self):
        for kind in (tarfile.SYMTYPE, tarfile.LNKTYPE, tarfile.DIRTYPE, tarfile.CHRTYPE):
            with self.subTest(type=kind):
                self.write_archive([dict(name="evidence.json", type=kind, linkname="../outside")])
                with self.assertRaises(RuntimeError):
                    coordinator.extract(self.archive, self.output)
                self.assertFalse((self.output/"evidence.json").exists())

    def test_declared_oversize_is_rejected_before_body_extraction(self):
        member = tarfile.TarInfo("producer.samples.bin")
        member.size = 100*1024*1024+1
        # Write a deliberately incomplete archive directly: newer Python
        # versions require addfile() to receive a real nonempty member body.
        # The extractor must reject this declared size before reading a body.
        with gzip.open(self.archive, "wb") as target:
            target.write(member.tobuf(format=tarfile.USTAR_FORMAT))
            target.write(b"\0" * (2 * tarfile.BLOCKSIZE))
        with self.assertRaisesRegex(RuntimeError, "Raw attempt exceeds extraction bound"):
            coordinator.extract(self.archive, self.output)
        self.assertFalse((self.output/"producer.samples.bin").exists())
        self.assertLess(self.archive.stat().st_size, 4096)

    def test_full_matrix_has_fixed_population_and_balanced_profile_positions(self):
        cases, attempts = coordinator.plan(False)
        self.assertEqual(len(cases), 14)
        self.assertEqual(len(attempts), 257)
        self.assertEqual([row["attempt"] for row in attempts], list(range(1, 258)))
        for name, case in cases.items():
            selected = [row for row in attempts if row["case"] == name]
            if name.startswith("short-control-"):
                expected_profiles, repetitions = coordinator.CONTROLS, 5
                self.assertEqual((case["messages"], case["warmup_messages"]), (16, 0))
            elif name == "long-64":
                expected_profiles, repetitions = coordinator.PROFILES, 3
                self.assertEqual((case["messages"]-case["warmup_messages"], case["warmup_messages"]), (131072, 8192))
            else:
                expected_profiles, repetitions = coordinator.PROFILES, 6
                self.assertEqual((case["messages"]-case["warmup_messages"], case["warmup_messages"]), (16384, 4096))
            self.assertEqual(Counter(row["profile"] for row in selected),
                             Counter({profile: repetitions for profile in expected_profiles}))
            by_position = Counter((row["profile"], row["position"]) for row in selected)
            self.assertEqual(set(by_position.values()), {repetitions//len(expected_profiles)})
            for block in {row["round"] for row in selected}:
                ordered = [row for row in selected if row["round"] == block]
                self.assertEqual([row["position"] for row in ordered], list(range(1, len(expected_profiles)+1)))
                self.assertEqual({row["profile"] for row in ordered}, set(expected_profiles))

    def test_smoke_matrix_is_short_and_still_exercises_both_modes_and_controls(self):
        cases, attempts = coordinator.plan(True)
        self.assertEqual((len(cases), len(attempts)), (6, 22))
        self.assertEqual([row["attempt"] for row in attempts], list(range(1, 23)))
        for name, case in cases.items():
            selected = [row for row in attempts if row["case"] == name]
            profiles = coordinator.CONTROLS if name.startswith("short-control-") else coordinator.PROFILES
            self.assertEqual([row["profile"] for row in selected], list(profiles))
            self.assertEqual((case["messages"], case["warmup_messages"]),
                             (16, 0) if name.startswith("short-control-") else (40, 8))

    def test_duplicate_and_nonfinite_json_evidence_is_rejected(self):
        for encoded in ('{"x":1,"x":2}', '{"x":NaN}', '{"x":Infinity}'):
            with self.subTest(encoded=encoded):
                with self.assertRaises(RuntimeError):
                    coordinator.load_json(encoded)


if __name__ == "__main__":
    unittest.main()
