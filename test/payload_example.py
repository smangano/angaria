#!/usr/bin/env python3
"""Run the primer's binary payload lesson and check its observable results."""

import argparse
from pathlib import Path
import subprocess
import sys
import tempfile


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
    parser.add_argument("example", type=Path, help="path to angaria_primer_09_payload")
    arguments = parser.parse_args()
    executable = arguments.example.resolve()
    try:
        with tempfile.TemporaryDirectory(prefix="angaria-payload-primer-") as root:
            session = Path(root) / "session"
            result = run(executable, session)
            check(result.returncode == 0, f"Payload lesson failed: {result.stdout!r} {result.stderr!r}")
            # Match complete lines so numeric prefixes and extra hex bytes
            # cannot masquerade as the expected sizes, versions, or payload.
            output_lines = result.stdout.splitlines()
            for marker in (
                "Quote v1 bytes: 16",
                "Quote v1 hex: fe ff ff ff ff ff ff ff 07 00 00 00 00 00 00 00",
                "Decoded v1: PriceTicks=-2 Quantity=7",
                "Quote v2 bytes: 20",
                "Decoded v2: PriceTicks=-2 Quantity=7 Instrument=42",
                "Retained codec version: 1",
                "Codecs remain usable after Session handles close.",
                "Payload encoding only: no messages were sent.",
            ):
                check(marker in output_lines, f"Payload lesson omitted exact line {marker!r}: {result.stdout!r}")

            # Repeating a tutorial command must preserve the reader's saved
            # Session. Only the temporary parent created here is cleaned up.
            log = session / "session.ang"
            original = log.read_bytes()
            retry = run(executable, session)
            check(retry.returncode != 0, "Payload lesson unexpectedly reused an existing Session")
            check("Conflict" in retry.stderr, f"Expected Conflict: {retry.stderr!r}")
            check(log.read_bytes() == original, "Payload lesson modified an existing Session")
    except (OSError, RuntimeError, subprocess.TimeoutExpired) as error:
        print(f"payload_example: {error}", file=sys.stderr)
        return 1
    print("Payload primer passed: exact bytes, schema versions, and safe Session creation.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
