#!/usr/bin/env python3
"""Run the primer programs and their independent-process ownership walkthrough."""

# These are format-2 public API examples. They configure connections and open
# endpoints. Lesson 14 adds direct Queue acceptance and retained outcome lookup;
# receive and acknowledgment are introduced only after their implementation.
import argparse
import os
from pathlib import Path
import selectors
import shutil
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
        stdin=subprocess.DEVNULL,
        capture_output=True,
        text=True,
        timeout=5,
    )
    check(
        (result.returncode == 0) == succeeds,
        f"{executable.name} {arguments!r} exited {result.returncode}: "
        f"stdout={result.stdout!r}, stderr={result.stderr!r}",
    )
    return result.stdout + result.stderr


def expect_ready(process, role, incarnation):
    # Wait for the program's flushed handshake rather than guessing how long
    # initialization takes. READY occurs after the original Task has closed:
    # only its endpoint handles retain the role's exclusive claim at this point.
    expected = f"READY {role} incarnation={incarnation}".encode()
    deadline = time.monotonic() + 5
    received = b""
    with selectors.DefaultSelector() as selector:
        selector.register(process.stdout, selectors.EVENT_READ)
        while expected not in received.splitlines():
            remaining = deadline - time.monotonic()
            check(
                remaining > 0 and selector.select(remaining),
                f"Timed out waiting for {expected!r}; received {received!r}",
            )
            chunk = os.read(process.stdout.fileno(), 4096)
            check(chunk, f"Process closed before {expected!r}; received {received!r}")
            received += chunk
            check(len(received) <= 16384, "Unexpectedly large role output")
    check(process.poll() is None, f"{role} closed instead of holding its claim")


def finish(process):
    output, _ = process.communicate(input=b"\n", timeout=5)
    check(process.returncode == 0, f"Role exited {process.returncode}: {output!r}")


def simple_examples(executables, workspace):
    expected = (
        ("Created Session: First session", "Task capacity: 8"),
        ("Expected Conflict:", "Expected ReadOnlyRegistry:", "Consumer replacement incarnation: 2"),
        ("Quote v1 Fields: PriceTicks Quantity", "Quote v2 Fields: PriceTicks Quantity Instrument", "Expected SchemaConflict:"),
        ("Active Queue: FeedToConsumer", "Source member count: 1", "Setup only:"),
        ("QuoteUpdates destinations: 2 -> 1", "No messages were sent;"),
    )
    for number, (executable, markers) in enumerate(zip(executables, expected), start=1):
        session = workspace / f"lesson-{number}"
        output = command(executable, session)
        for marker in markers:
            check(marker in output, f"{executable.name} did not demonstrate {marker!r}: {output!r}")

        # Readers may accidentally repeat a command. Every creating example
        # must refuse the existing path and preserve the saved Session bytes.
        log = session / "session.ang"
        original = log.read_bytes()
        retry = command(executable, session, succeeds=False)
        check("Conflict" in retry, f"Existing Session did not report Conflict: {retry!r}")
        check(log.read_bytes() == original, f"{executable.name} changed an existing Session")


def process_example(executable, session):
    processes = []

    def start(role, incarnation):
        process = subprocess.Popen(
            [str(executable), role, str(session), "--hold"],
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            bufsize=0,
        )
        processes.append(process)
        expect_ready(process, role, incarnation)
        return process

    try:
        command(executable, "init", session)
        offline = command(executable, "inspect", session)
        for marker in ("Feed attached=no", "Strategy attached=no", "StrategyInputs members=1"):
            check(marker in offline, f"Unexpected initial inspection: {offline!r}")

        consumer = start("consumer", 1)
        producer = start("producer", 1)
        attached = command(executable, "inspect", session)
        for marker in ("Feed attached=yes incarnation=1", "Strategy attached=yes incarnation=1"):
            check(marker in attached, f"Inspector did not find the held claims: {attached!r}")
        for role in ("producer", "consumer"):
            busy = command(executable, role, session, succeeds=False)
            check("OwnershipBusy" in busy, f"Second {role} did not report OwnershipBusy: {busy!r}")

        # Closing the producer's last endpoints permits a new incarnation,
        # while the original consumer remains attached independently.
        finish(producer)
        replacement = command(executable, "producer", session)
        check("READY producer incarnation=2" in replacement, f"Unexpected replacement: {replacement!r}")

        # Terminate only a child created by this walkthrough. The native claim
        # lock must also permit replacement after an ungraceful process exit.
        crashed = start("producer", 3)
        crashed.kill()
        crashed.wait(timeout=5)
        recovered = command(executable, "producer", session)
        check("READY producer incarnation=4" in recovered, f"Unexpected crash replacement: {recovered!r}")
        check(consumer.poll() is None, "Producer restart disturbed the independent consumer")
        finish(consumer)

        final = command(executable, "inspect", session)
        for marker in ("Feed attached=no incarnation=4", "Strategy attached=no incarnation=1"):
            check(marker in final, f"Unexpected final inspection: {final!r}")
    finally:
        for process in processes:
            if process.poll() is None:
                process.kill()
            process.wait(timeout=5)
            for stream in (process.stdin, process.stdout):
                stream.close()


def configuration_example(executable, fixture, workspace):
    # Copy the documented file unchanged. Its relative Session directory must
    # be resolved beside the copied TOML, independently of this script's cwd.
    directory = workspace / "configuration"
    directory.mkdir()
    configuration = directory / "configuration.toml"
    shutil.copyfile(fixture, configuration)
    output = command(executable, configuration, "Setup")
    for marker in (
        "Created from TOML: Primer configuration",
        "Creator: Setup",
        "Source members: 1",
        "Quote v1 Fields: 2",
        "Connection active=yes",
        "Reattached from TOML; creator attached=no",
    ):
        check(marker in output, f"TOML lesson did not demonstrate {marker!r}: {output!r}")
    log = directory / "primer-session" / "session.ang"
    original = log.read_bytes()
    retry = command(executable, configuration, "Setup", succeeds=False)
    check("Conflict" in retry, f"Repeated file-based creation did not report Conflict: {retry!r}")
    check(log.read_bytes() == original, "TOML lesson changed an existing Session")


def direct_send_example(executable, workspace):
    session = workspace / "direct-send"
    output = command(executable, session)
    for marker in (
        "Accepted quote while Strategy is offline.",
        "Identical repeat: accepted once.",
        "Different bytes with the same ID: Conflict.",
        "Unused issued ID: rejected, no destinations.",
        "Replacement Feed incarnation: 2",
        "Earlier incarnation's acceptance remains queryable.",
        "Acceptance records Queue entry; consumer processing is a later step.",
    ):
        check(marker in output.splitlines(), f"Direct-send lesson omitted {marker!r}: {output!r}")
    log = session / "session.ang"
    original = log.read_bytes()
    retry = command(executable, session, succeeds=False)
    check("Conflict" in retry, f"Repeated direct-send creation did not report Conflict: {retry!r}")
    check(log.read_bytes() == original, "Direct-send lesson changed an existing Session")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("examples", type=Path, nargs=7, help="primer executables in lesson order")
    parser.add_argument("configuration", type=Path, help="lesson 7 TOML fixture")
    parser.add_argument("direct_send", type=Path, help="lesson 14 direct-send executable")
    arguments = parser.parse_args()
    executables = [path.resolve() for path in arguments.examples]
    try:
        with tempfile.TemporaryDirectory(prefix="angaria-primer-") as root:
            workspace = Path(root)
            simple_examples(executables[:5], workspace)
            process_example(executables[5], workspace / "processes")
            configuration_example(executables[6], arguments.configuration.resolve(), workspace)
            direct_send_example(arguments.direct_send.resolve(), workspace)
    except (OSError, RuntimeError, ValueError, subprocess.TimeoutExpired) as error:
        print(f"primer_examples: {error}", file=sys.stderr)
        return 1
    print("Primer examples passed: setup, ownership, TOML, direct acceptance, and retained publication lookup.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
