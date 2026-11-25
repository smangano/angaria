#!/usr/bin/env python3
"""Capture application records, then inspect them using retained definitions."""

import argparse
from pathlib import Path
import re
import subprocess
import sys
import tempfile


def check(condition, message):
    if not condition:
        raise RuntimeError(message)


def run(executable, *arguments):
    return subprocess.run(
        [str(executable), *(str(argument) for argument in arguments)],
        stdin=subprocess.DEVNULL,
        capture_output=True,
        text=True,
        timeout=10,
    )


def expected_lines(session):
    def header(name, message_type, version, sequence, flags="0x00000000"):
        return (f'message "{name}" session={session} type={message_type} version={version} '
                f'producer=1 incarnation=1 sequence={sequence} time_ns={100 + sequence} flags={flags}')

    return [
        header("Quote", 1, 1, 1, "0x80000001"),
        '  [0] "PriceTicks" int64 = -2',
        '  [1] "Quantity" uint64 = 7',
        header("Quote", 1, 2, 2),
        '  [0] "Quantity" uint64 = 8',
        '  [1] "PriceTicks" int64 = -3',
        header("Order", 2, 1, 3),
        '  [0] "Side" enum<byte> = "Buy" (1)',
        '  [1] "Quantity" uint64 = 9',
        '  [2] "Instrument" chars<6> = "IBM\\x00 \\x0a"',
        '  [3] "Flags" bits16 = 0x8001',
        '  [4] "Enabled" bool = true',
        '  [5] "Adjustment" int8 = -128',
        header("Heartbeat", 3, 1, 4),
    ]


def expect_failure(result, code, context=None):
    check(result.returncode != 0, f"Expected {code}, got successful output: {result.stdout!r}")
    check(result.stderr.startswith(f"{code} at "), f"Expected {code}: {result.stderr!r}")
    if context is not None:
        check(context in result.stderr, f"Missing record/offset {context!r}: {result.stderr!r}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("example", type=Path, help="path to angaria_primer_12_capture")
    parser.add_argument("inspector", type=Path, help="path to angaria_inspect")
    arguments = parser.parse_args()
    example = arguments.example.resolve()
    inspector = arguments.inspector.resolve()
    try:
        with tempfile.TemporaryDirectory(prefix="angaria-capture-primer-") as temporary:
            root = Path(temporary)
            workspace = root / "workspace"
            produced = run(example, workspace)
            check(produced.returncode == 0, f"Capture lesson failed: {produced.stdout!r} {produced.stderr!r}")
            lines = produced.stdout.splitlines()
            check(len(lines) == 5, f"Unexpected capture summary: {lines!r}")
            matched = re.fullmatch(r"Source Session: ([0-9a-f]{32})", lines[0])
            check(matched is not None, f"Missing source Session identity: {lines!r}")
            source_session = matched.group(1)
            check(source_session != "0" * 32, "Producer reported an invalid all-zero Session")
            check(lines[1:] == [
                "Captured records: 4",
                "Capture bytes: 371",
                "Retained schemas: Quote v1, Quote v2, Order v1, Heartbeat v1",
                "Capture encoding only: no messages were sent.",
            ], f"Unexpected capture summary: {lines!r}")

            capture = workspace / "capture.angcap"
            session = workspace / "session"
            original_capture = capture.read_bytes()
            check(len(original_capture) == 371, "Capture length differs from four independent record sizes")
            original_session = (session / "session.ang").read_bytes()

            # The tutorial creates its workspace exclusively. A repeat must
            # preserve both previously captured messages and their catalog.
            retry = run(example, workspace)
            check(retry.returncode != 0 and "Conflict at workspace" in retry.stderr,
                  f"Existing workspace was not protected: {retry.stdout!r} {retry.stderr!r}")
            check(capture.read_bytes() == original_capture, "Repeating capture changed an existing file")
            check((session / "session.ang").read_bytes() == original_session,
                  "Repeating capture changed the existing Session")

            inspected = run(inspector, session, capture)
            check(inspected.returncode == 0, f"Generic inspection failed: {inspected.stdout!r} {inspected.stderr!r}")
            check(inspected.stdout == "\n".join(expected_lines(source_session)) + "\n",
                  f"Inspection differs from complete expected lines: {inspected.stdout!r}")
            check(inspected.stderr == "", f"Unexpected inspection diagnostic: {inspected.stderr!r}")

            # Mutations below exercise the command-line diagnostic boundary;
            # ordinary application code writes records through the codec.
            cases = []
            unknown_schema = bytearray(original_capture)
            unknown_schema[20:24] = (999).to_bytes(4, "little")
            cases.append(("unknown-schema", unknown_schema, "NotFound", "capture.records[0]@0"))
            wrong_session = bytearray(original_capture)
            other_high = 2 if source_session == "00000000000000010000000000000000" else 1
            wrong_session[32:48] = other_high.to_bytes(8, "little") + bytes(8)
            cases.append(("wrong-session", wrong_session, "SchemaConflict", "capture.records[0]@0"))
            reserved = bytearray(original_capture)
            reserved[18] = 1
            cases.append(("reserved", reserved, "InvalidArgument", "capture.records[0]@0"))
            cases.append(("truncated", original_capture[:-1], "InvalidArgument", "capture.records[3]@291"))
            for name, payload, code, context in cases:
                path = root / f"{name}.angcap"
                path.write_bytes(payload)
                result = run(inspector, session, path)
                expect_failure(result, code, context)
                if name == "truncated":
                    check(result.stdout == "\n".join(expected_lines(source_session)[:-1]) + "\n",
                          f"Late truncation did not preserve exactly the validated prefix: {result.stdout!r}")
                check(path.read_bytes() == payload, f"Inspector modified {name} input")

            for option, limit in [
                ("--max-record-bytes", 95),
                ("--max-capture-bytes", 370),
                ("--max-text-bytes", 1),
                ("--max-catalog-bytes", 1),
                ("--max-output-bytes", 1),
            ]:
                limited = run(inspector, session, capture, option, limit)
                expect_failure(limited, "CapacityExceeded")
                if option in {"--max-record-bytes", "--max-text-bytes", "--max-catalog-bytes", "--max-output-bytes"}:
                    check(limited.stdout == "", f"{option} emitted part of a rejected first record")
            expect_failure(run(inspector, session, capture, "--max-record-bytes", "-1"), "InvalidArgument")
            expect_failure(run(inspector, session, capture, "--unknown-limit", "1"), "InvalidArgument")
            check(run(inspector, "--help").returncode == 0, "Inspector help did not succeed")
            check(capture.read_bytes() == original_capture, "Inspection changed the original capture")
    except (OSError, RuntimeError, subprocess.TimeoutExpired) as error:
        print(f"capture_example: {error}", file=sys.stderr)
        return 1
    print("Capture primer passed: generic exact-version inspection, escaped values, and bounded diagnostics.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
