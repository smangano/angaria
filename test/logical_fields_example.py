#!/usr/bin/env python3
"""Run the logical Field lesson, verify exact bytes, and preserve saved Sessions."""

import argparse
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile


EXPECTED_LINES = [
    "Registered logical Fields: 5",
    "Side labels: 1=Buy 2=Sell",
    "OrderSettings payload bytes: 11",
    "OrderSettings hex: 01 01 80 01 49 42 4d 20 20 20 80",
    "Decoded: Enabled=true Flags=32769 Side=1 Instrument='IBM   ' Adjustment=-128",
    "Rejected undeclared enum: InvalidArgument",
    "Caller buffer unchanged: yes",
    "Caller buffer matches owning payload: yes",
    "Codec remains usable after Session handles close.",
    "Payload encoding only: no messages were sent.",
]


def check(condition, message):
    if not condition:
        raise RuntimeError(message)


def run(executable, configuration):
    return subprocess.run(
        [str(executable), str(configuration)],
        stdin=subprocess.DEVNULL,
        capture_output=True,
        text=True,
        timeout=10,
    )


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("example", type=Path, help="path to angaria_primer_10_logical_fields")
    parser.add_argument("configuration", type=Path, help="path to the lesson's logical-fields.toml")
    arguments = parser.parse_args()
    executable = arguments.example.resolve()
    try:
        with tempfile.TemporaryDirectory(prefix="angaria-logical-primer-") as temporary:
            root = Path(temporary)
            configuration = root / "logical-fields.toml"
            shutil.copyfile(arguments.configuration, configuration)
            result = run(executable, configuration)
            check(result.returncode == 0, f"Logical Field lesson failed: {result.stdout!r} {result.stderr!r}")
            check(result.stdout.splitlines() == EXPECTED_LINES,
                  f"Logical Field lesson output differs from its complete golden lines: {result.stdout!r}")

            # The shipped file and the reader's Session are never cleanup
            # targets. Only this test's temporary parent is removed on exit.
            log = root / "logical-session" / "session.ang"
            original = log.read_bytes()
            retry = run(executable, configuration)
            check(retry.returncode != 0, "Logical Field lesson reused an existing Session")
            check("Conflict" in retry.stderr, f"Expected Conflict: {retry.stderr!r}")
            check(log.read_bytes() == original, "Logical Field lesson modified an existing Session")
    except (OSError, RuntimeError, subprocess.TimeoutExpired) as error:
        print(f"logical_fields_example: {error}", file=sys.stderr)
        return 1
    print("Logical Field primer passed: exact bytes, logical metadata, and transactional validation.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
