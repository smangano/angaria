#!/usr/bin/env python3
"""Exchange completed captures across processes, schema additions, and restart."""

# These are public example commands, not native Queue transport or journal
# fault-injection helpers. The producer is killed only after its file closes.
import argparse
import os
from pathlib import Path
import re
import selectors
import subprocess
import sys
import tempfile
import time


def check(condition, message):
    if not condition:
        raise RuntimeError(message)


def command(executable, *arguments, succeeds=True):
    result = subprocess.run(
        [str(executable), *map(str, arguments)],
        stdin=subprocess.DEVNULL, capture_output=True, text=True, timeout=10,
    )
    check((result.returncode == 0) == succeeds,
          f"{arguments!r} exited {result.returncode}: {result.stdout!r} {result.stderr!r}")
    if succeeds:
        check(result.stderr == "", f"Unexpected diagnostic for {arguments!r}: {result.stderr!r}")
    return result


def expect_ready(process, marker):
    """Read a child's explicit handshake without blocking forever on readline."""
    received = b""
    deadline = time.monotonic() + 10
    with selectors.DefaultSelector() as selector:
        selector.register(process.stdout, selectors.EVENT_READ)
        while marker.encode() not in received.splitlines():
            remaining = deadline - time.monotonic()
            check(remaining > 0 and selector.select(remaining),
                  f"Timed out waiting for {marker}: {received!r}")
            chunk = os.read(process.stdout.fileno(), 4096)
            check(chunk, f"Child closed before {marker}: {received!r}")
            received += chunk
            check(len(received) <= 16384, "Unexpectedly large child output")
    check(process.poll() is None, f"Child exited instead of holding at {marker}")
    return received.decode("ascii")


def expected_capture(session, phase):
    version, incarnation, price, quantity = {
        "first": (1, 1, -2, 7), "new": (2, 2, -3, 8), "restart": (1, 3, -4, 9),
    }[phase]

    def header(name, message_type, schema_version, sequence):
        return (f'message "{name}" session={session} type={message_type} version={schema_version} '
                f'producer=2 incarnation={incarnation} sequence={sequence} '
                f'time_ns={100 + sequence} flags=0x00000000')

    fields = ([f'  [0] "PriceTicks" int64 = {price}', f'  [1] "Quantity" uint64 = {quantity}']
              if version == 1 else
              [f'  [0] "Quantity" uint64 = {quantity}', f'  [1] "PriceTicks" int64 = {price}'])
    return "\n".join([header("Quote", 1, version, 1), *fields,
                      header("Heartbeat", 2, 1, 2)]) + "\n"


def produced_summary(phase, incarnation):
    version = 2 if phase == "new" else 1
    return (f"PRODUCED phase={phase} version={version} producer=2 "
            f"incarnation={incarnation} records=2 bytes=176\n")


def walkthrough(example, inspector, workspace):
    # A caller-supplied workspace is retained, never cleaned up by this script.
    # Refuse files, directories, and even dangling symlinks before any command.
    check(not os.path.lexists(workspace), f"Workspace path already exists: {workspace}")
    check(workspace.parent.is_dir(), f"Workspace parent directory does not exist: {workspace.parent}")
    processes = []

    def start(*arguments):
        process = subprocess.Popen(
            [str(example), *map(str, arguments)],
            stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, bufsize=0,
        )
        processes.append(process)
        return process

    def capture_path(phase):
        return workspace / phase / "capture.angcap"

    try:
        initialized = command(example, "init", workspace)
        match = re.fullmatch(r"SESSION ([0-9a-f]{32})\nINITIALIZED revision=(\d+) feed=2\n",
                             initialized.stdout)
        check(match is not None, f"Unexpected initialization: {initialized.stdout!r}")
        session, initial_revision = match.group(1), int(match.group(2))
        check(session != "0" * 32 and initial_revision > 0, "Invalid initial Session context")
        log = workspace / "session" / "session.ang"
        original_log = log.read_bytes()
        rejected = command(example, "init", workspace, succeeds=False)
        check("Conflict" in rejected.stderr, f"Existing workspace was not protected: {rejected.stderr!r}")
        check(log.read_bytes() == original_log, "Repeated initialization changed the existing Session")

        # This reader owns the v1 catalog before any new version exists, then
        # releases all native handles while retaining those definitions.
        retained = start("retain", workspace)
        retained_prefix = expect_ready(retained, "READY retained")
        check(retained_prefix == "READY retained\n", f"Unexpected retained handshake: {retained_prefix!r}")
        producer = start("produce", workspace, "first", "--hold")
        producer_prefix = expect_ready(producer, "READY producer")
        check(producer_prefix == produced_summary("first", 1) + "READY producer\n",
              f"Unexpected producer handshake: {producer_prefix!r}")
        first_bytes = capture_path("first").read_bytes()
        check(len(first_bytes) == 176, "First capture is not two complete records")

        # READY means the file has been flushed, closed, and checked. Records
        # use capture-local numbering, so this has not sealed Registry setup.
        # Killing tests claim replacement, not recovery of a torn append.
        producer.kill()
        remaining, _ = producer.communicate(timeout=10)
        check(producer.returncode != 0 and remaining == b"", "Producer did not stop at its completed capture")
        upgraded = command(example, "upgrade", workspace)
        check(upgraded.stdout == f"UPGRADED version=2 revision={initial_revision + 1}\n",
              f"Schema addition changed unexpected Registry state: {upgraded.stdout!r}")
        for phase, incarnation in [("new", 2), ("restart", 3)]:
            produced = command(example, "produce", workspace, phase)
            check(produced.stdout == produced_summary(phase, incarnation),
                  f"Replacement producer identity is wrong: {produced.stdout!r}")
            check(len(capture_path(phase).read_bytes()) == 176, "Replacement capture has an unexpected length")
            check(capture_path("first").read_bytes() == first_bytes, "Schema addition/restart changed old bytes")

        # The same reader must interpret both v1 captures and refuse v2. Its
        # catalog is not silently refreshed by another process's registration.
        check(retained.poll() is None, "Retained reader exited before its release signal")
        output, _ = retained.communicate(input=b"\n", timeout=10)
        check(retained.returncode == 0, f"Retained reader failed: {retained_prefix}{output!r}")
        expected = ("RETAINED phase=first\n" + expected_capture(session, "first") +
                    "RETAINED phase=restart\n" + expected_capture(session, "restart") +
                    "REJECTED phase=new code=NotFound record=0 offset=0\n")
        check(output.decode("ascii") == expected, f"Retained catalog behavior differs: {output!r}")

        # Every application process has exited. Fresh generic CLI processes
        # discover both versions from the persisted catalog, with exact values
        # even though the two Quote layouts have the same payload byte width.
        captures = {phase: capture_path(phase).read_bytes() for phase in ("first", "new", "restart")}
        healthy_log = log.read_bytes()
        for phase in captures:
            inspected = command(inspector, workspace / "session", capture_path(phase))
            check(inspected.stdout == expected_capture(session, phase),
                  f"Cold inspection changed {phase} interpretation: {inspected.stdout!r}")
            check(capture_path(phase).read_bytes() == captures[phase], "Inspection changed its capture")
        check(log.read_bytes() == healthy_log, "Healthy cold inspection changed the native Session")

        # Existing phase directories reserve their output names. A retry must
        # preserve old captures rather than truncating files from prior owners.
        for phase, original in captures.items():
            rejected = command(example, "produce", workspace, phase, succeeds=False)
            check("Conflict" in rejected.stderr, f"Existing {phase} capture was not protected: {rejected.stderr!r}")
            check(capture_path(phase).read_bytes() == original, "Rejected retry changed an existing capture")
        print("Independent producers retained exact v1/v2 bytes across claim replacement.")
        print("An owning v1 inspector rejected v2; cold inspectors decoded every capture after application exit.")
        print(f"Process exchange passed at {workspace}")
    finally:
        for process in processes:
            if process.poll() is None:
                process.kill()
            process.wait(timeout=10)
            for stream in (process.stdin, process.stdout):
                if stream is not None and not stream.closed:
                    stream.close()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("example", type=Path, help="built angaria_primer_13_process_exchange")
    parser.add_argument("inspector", type=Path, help="built angaria_inspect")
    parser.add_argument("--workspace", type=Path, help="new workspace to retain after the walkthrough")
    arguments = parser.parse_args()
    try:
        with tempfile.TemporaryDirectory(prefix="angaria-process-exchange-") as temporary:
            workspace = arguments.workspace.absolute() if arguments.workspace else Path(temporary) / "exchange"
            walkthrough(arguments.example.resolve(), arguments.inspector.resolve(), workspace)
    except (OSError, RuntimeError, ValueError, UnicodeError, subprocess.TimeoutExpired) as error:
        print(f"process_exchange: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
