#!/usr/bin/env python3
"""Run the portable message record lesson and preserve existing Sessions."""

import argparse
from pathlib import Path
import subprocess
import sys
import tempfile


EXPECTED_LINES = [
    "Message record bytes: 96",
    "Header bytes: 80 Payload bytes: 16",
    "Schema: Quote type=1 version=1",
    "Producer: Task=1 incarnation=1 sequence=1",
    "Publication time (demo): 42 ns",
    "User flags: 2147483649",
    "Decoded Quote: PriceTicks=-2 Quantity=7",
    "Wrong schema version: SchemaConflict",
    "Record cap 95: CapacityExceeded",
    "Borrowed MessageView payload bytes: 16",
    "Owning record survives input buffer release: yes",
    "Record and payload codecs remain usable after Session handles close.",
    "Record encoding only: no messages were sent.",
]


def check(condition, message):
    if not condition:
        raise RuntimeError(message)


def run(executable, session):
    return subprocess.run(
        [str(executable), str(session)],
        stdin=subprocess.DEVNULL,
        capture_output=True,
        text=True,
        timeout=10,
    )


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("example", type=Path, help="path to angaria_primer_11_message_record")
    arguments = parser.parse_args()
    executable = arguments.example.resolve()
    try:
        with tempfile.TemporaryDirectory(prefix="angaria-record-primer-") as temporary:
            session = Path(temporary) / "session"
            result = run(executable, session)
            check(result.returncode == 0, f"Message record lesson failed: {result.stdout!r} {result.stderr!r}")
            check(result.stdout.splitlines() == EXPECTED_LINES,
                  f"Message record lesson differs from its complete expected lines: {result.stdout!r}")

            # Only our temporary parent is cleaned up. Retrying the tutorial
            # must preserve a reader's saved Session, including issued IDs.
            log = session / "session.ang"
            original = log.read_bytes()
            retry = run(executable, session)
            check(retry.returncode != 0, "Message record lesson reused an existing Session")
            check("Conflict" in retry.stderr, f"Expected Conflict: {retry.stderr!r}")
            check(log.read_bytes() == original, "Message record lesson modified an existing Session")
    except (OSError, RuntimeError, subprocess.TimeoutExpired) as error:
        print(f"message_record_example: {error}", file=sys.stderr)
        return 1
    print("Message record primer passed: exact headers, schema selection, and independent ownership.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
