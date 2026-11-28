#!/usr/bin/env python3
"""Run public acknowledged delivery, replay its accepted log, then inspect export.

All three programs use public APIs. The test creates real accepted messages,
checks the frozen publication/Queue order, and confirms that flat capture export
can be inspected with the Session's exact schema catalog. It creates no private
records or fake processing receipts and changes no consumer progress itself.
"""
import argparse
from pathlib import Path
import subprocess
import tempfile


def run(executable, *arguments, expected_error=None):
    result = subprocess.run([str(executable), *map(str, arguments)], stdin=subprocess.DEVNULL,
                            capture_output=True, text=True, timeout=15)
    if (result.returncode == 0) != (expected_error is None):
        raise RuntimeError(f"{executable.name}: {result.stdout}\n{result.stderr}")
    if expected_error is not None and expected_error not in result.stderr:
        raise RuntimeError("Export rejection did not identify its occupied output path")
    return result.stdout


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("receive", type=Path, help="primer lesson 15")
    parser.add_argument("replay", type=Path, help="primer lesson 18")
    parser.add_argument("inspect", type=Path, help="angaria_inspect")
    options = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="angaria-log-primer-") as temporary:
        directory = Path(temporary)
        session = directory / "quotes"
        capture = directory / "accepted.angcap"
        received = run(options.receive.resolve(), session)
        if "Final receive: timeout." not in received:
            raise RuntimeError("Lesson 15 did not finish its three real deliveries")
        replayed = run(options.replay.resolve(), session, capture)
        for marker in ("Replayed publications: 3", "Rewind preserved the captured input",
                       "Exported flat message capture"):
            if marker not in replayed.splitlines():
                raise RuntimeError("Log replay omitted " + marker)
        publications = [line for line in replayed.splitlines() if line.startswith("Publication ")]
        destinations = [line for line in replayed.splitlines() if line.startswith("  Accepted Queue=")]
        if len(publications) != 3 or len(destinations) != 3:
            raise RuntimeError("Replay lost or duplicated an accepted publication/destination")
        for sequence, (publication, destination) in enumerate(zip(publications, destinations), 1):
            if f" sequence={sequence} schema=" not in publication or not destination.endswith(f" sequence={sequence}"):
                raise RuntimeError("Replay did not preserve the producer and Queue sequence order")
        # This lesson has exactly three 8-byte payloads with the established
        # 80-byte portable header. Native producer-log framing is different.
        if capture.stat().st_size != 3 * (80 + 8):
            raise RuntimeError("Flat capture does not contain exactly three canonical records")
        inspected = run(options.inspect.resolve(), session, capture)
        for value in (12345, 12346, 12347):
            if f'"PriceTicks" int64 = {value}' not in inspected:
                raise RuntimeError("Generic public inspector lost the exact Quote field value")
        original = capture.read_bytes()
        run(options.replay.resolve(), session, capture, expected_error="Capture output must be a new file")
        link = directory / "capture-link.angcap"
        link.symlink_to(capture)
        run(options.replay.resolve(), session, link, expected_error="Capture output must be a new file")
        if capture.read_bytes() != original or not link.is_symlink():
            raise RuntimeError("Refused export changed its existing file or symlink target")
        dangling = directory / "dangling-capture.angcap"
        dangling.symlink_to("missing-target.angcap")
        run(options.replay.resolve(), session, dangling, expected_error="Capture output must be a new file")
        if not dangling.is_symlink() or (directory / "missing-target.angcap").exists():
            raise RuntimeError("Refused export replaced or followed a dangling output symlink")
        repeated = run(options.replay.resolve(), session)
        if "Replayed publications: 3" not in repeated.splitlines():
            raise RuntimeError("Inspecting/exporting changed the retained accepted publications")
    print("Public log replay passed: three accepted messages, frozen order, rewind, export, generic inspection, and occupied-output guards.")


if __name__ == "__main__":
    main()
