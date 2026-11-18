#!/usr/bin/env python3
"""Exercise endpoint restart and offline inspection through the demo executable."""

import argparse
import hashlib
import os
from pathlib import Path
import selectors
import signal
import subprocess
import sys
import tempfile
import time


def check(condition, message):
    if not condition:
        raise RuntimeError(message)


def expect_line(process, expected):
    deadline = time.monotonic() + 5
    received = b""
    with selectors.DefaultSelector() as selector:
        selector.register(process.stdout, selectors.EVENT_READ)
        while b"\n" not in received:
            remaining = deadline - time.monotonic()
            check(remaining > 0 and selector.select(remaining), f"Timed out waiting for {expected!r}")
            chunk = os.read(process.stdout.fileno(), 4096)
            check(chunk, f"Process closed stdout while waiting for {expected!r}, received {received!r}")
            received += chunk
            check(len(received) <= 4096, "Unexpectedly large process output")
    check(received == (expected + "\n").encode(), f"Expected {expected!r}, received {received!r}")


def run_demo(demo, session):
    processes = []

    def command(*arguments, status=0):
        result = subprocess.run(
            [str(demo), *map(str, arguments)],
            stdin=subprocess.DEVNULL,
            capture_output=True,
            text=True,
            timeout=5,
        )
        check(
            result.returncode == status,
            f"{arguments!r} exited with {result.returncode}, stdout={result.stdout!r}, stderr={result.stderr!r}",
        )
        return result.stdout

    def start(command_name):
        process = subprocess.Popen(
            [str(demo), command_name, str(session)],
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
        )
        processes.append(process)
        return process

    def send_line(process, line):
        process.stdin.write((line + "\n").encode())
        process.stdin.flush()

    def finish(process):
        process.stdin.close()
        status = process.wait(timeout=5)
        remaining = process.stdout.read()
        check(status == 0 and not remaining, f"Process exited with {status}, remaining output={remaining!r}")

    def crash(process):
        check(process.poll() is None, "Endpoint exited before the simulated crash")
        process.kill()
        check(process.wait(timeout=5) == -signal.SIGKILL, "Endpoint did not terminate with SIGKILL")

    try:
        command("create", session)
        print(f"Created session {session}", flush=True)
        producer = start("produce")
        send_line(producer, "41")
        expect_line(producer, "Sent")
        consumer = start("receive")
        expect_line(consumer, "Reading sequence=1 value=41")

        crash(producer)
        producer = start("produce")
        send_line(producer, "42")
        expect_line(producer, "Sent")
        command("receive", session, status=5)
        send_line(consumer, "ack")
        finish(consumer)
        print("Restarted producer while the original consumer stayed attached", flush=True)

        consumer = start("receive")
        expect_line(consumer, "Reading sequence=2 value=42")
        crash(consumer)
        consumer = start("receive")
        expect_line(consumer, "Reading sequence=2 value=42")
        send_line(consumer, "ack")
        finish(consumer)
        print("Restarted consumer and acknowledged the redelivered sequence 2", flush=True)

        send_line(producer, "-7")
        expect_line(producer, "Sent")
        consumer = start("receive")
        expect_line(consumer, "Reading sequence=3 value=-7")
        send_line(consumer, "ack")
        finish(consumer)
        finish(producer)

        log = session / "session.ang"
        original_digest = hashlib.sha256(log.read_bytes()).digest()
        output = command("inspect", log)
        lines = output.splitlines()
        check(len(lines) == 3, f"Expected three committed records, received {output!r}")
        for sequence, value in enumerate((41, 42, -7), start=1):
            prefix = f"queue={session.name} sequence={sequence} timestamp_ns="
            suffix = f" type=Reading value={value}"
            line = lines[sequence - 1]
            check(line.startswith(prefix) and line.endswith(suffix), f"Unexpected log record {line!r}")
            timestamp = int(line[len(prefix) : -len(suffix)])
            check(timestamp > 0, f"Unexpected timestamp {timestamp}")
        check(hashlib.sha256(log.read_bytes()).digest() == original_digest, "Inspection changed the log")
        command("remove", session)
        check(command("inspect", log) == output, "Queue removal changed the captured session")
        print("Both endpoints exited. The log remains readable after removing the live queue.", flush=True)
        print(output, end="")
    finally:
        for process in processes:
            if process.poll() is None:
                process.kill()
            process.wait(timeout=5)
            for stream in (process.stdin, process.stdout):
                stream.close()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("demo", type=Path, help="path to the built angaria_queue_demo executable")
    parser.add_argument("--session", type=Path, help="new session directory to retain after the walkthrough")
    arguments = parser.parse_args()

    try:
        if arguments.session is not None:
            run_demo(arguments.demo.resolve(), arguments.session.resolve())
        else:
            with tempfile.TemporaryDirectory(prefix="angaria-demo-") as root:
                run_demo(arguments.demo.resolve(), Path(root) / "demo")
    except (OSError, RuntimeError, ValueError, subprocess.TimeoutExpired) as error:
        print(f"restart_demo: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
