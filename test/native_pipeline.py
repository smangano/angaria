#!/usr/bin/env python3
"""Run the public native market-monitor primer as independent applications."""

# Pipes carry readiness reports only. Quotes and alerts travel through the
# Session's real mapped Queues; no private worker or fault hook is involved.
import argparse
import os
from pathlib import Path
import re
import selectors
import shutil
import signal
import subprocess
import sys
import tempfile
import time


FEED = (r"FEED quote=(?P<quote>\d+) version=(?P<version>\d+) price=(?P<price>\d+) "
        r"quantity=(?P<quantity>\d+) task=(?P<task>\d+) incarnation=(?P<incarnation>\d+) "
        r"publication=(?P<publication>\d+)")
BORROW = (r"BORROW queue_sequence=(?P<queue_sequence>\d+) source_task=(?P<source_task>\d+) "
          r"source_incarnation=(?P<source_incarnation>\d+) source_publication=(?P<source_publication>\d+) "
          r"version=(?P<version>\d+) price=(?P<price>\d+) quantity=(?P<quantity>\d+) "
          r"issuance=(?P<issuance>\d+)")
HEADER = (r"HEADER publication_ns=(?P<publication_ns>-?\d+) enqueue_ns=(?P<enqueue_ns>-?\d+) "
          r"dequeue_ns=(?P<dequeue_ns>-?\d+) copied=yes")
REDELIVER = (r"REDELIVER queue_sequence=(?P<queue_sequence>\d+) old_issuance=(?P<old_issuance>\d+) "
             r"new_issuance=(?P<new_issuance>\d+)")
FORWARD = (r"FORWARD source_queue_sequence=(?P<source_queue_sequence>\d+) "
           r"source_incarnation=(?P<source_incarnation>\d+) source_publication=(?P<source_publication>\d+) "
           r"version=(?P<version>\d+) price=(?P<price>\d+) quantity=(?P<quantity>\d+) "
           r"above_limit=(?P<above_limit>true|false) task=(?P<task>\d+) "
           r"incarnation=(?P<incarnation>\d+) publication=(?P<publication>\d+)")
AUDIT = (r"AUDIT input_queue_sequence=(?P<input_queue_sequence>\d+) source_task=(?P<source_task>\d+) "
         r"source_incarnation=(?P<source_incarnation>\d+) source_publication=(?P<source_publication>\d+) "
         r"source_queue_sequence=(?P<source_queue_sequence>\d+) price=(?P<price>\d+) "
         r"quantity=(?P<quantity>\d+) above_limit=(?P<above_limit>true|false) "
         r"producer_task=(?P<producer_task>\d+) producer_incarnation=(?P<producer_incarnation>\d+) "
         r"publication=(?P<publication>\d+)")
STATUS = (r"STATUS task=(?P<task>Feed|Strategy) incarnation=(?P<incarnation>\d+) "
          r"publication=(?P<publication>\d+) accepted=(?P<accepted>yes|no) "
          r"destinations=(?P<destinations>\d+)")
STRATEGY_READY = r"READY strategy task=(?P<task>\d+) incarnation=(?P<incarnation>\d+)"
AUDIT_READY = r"READY audit task=(?P<task>\d+) incarnation=(?P<incarnation>\d+)"


def check(condition, message):
    if not condition:
        raise RuntimeError(message)


def records(output, prefix, pattern):
    """Parse complete application reports, checking every printed field."""
    result = []
    for line in output.splitlines():
        if line.startswith(prefix):
            matched = re.fullmatch(pattern, line)
            check(matched is not None, f"Malformed {prefix} report: {line!r}")
            result.append(matched.groupdict())
    return result


def expect_fields(actual, expected, context):
    converted = {key: int(value) if isinstance(expected.get(key), int) else value
                 for key, value in actual.items()}
    check(converted == expected, f"{context}: expected {expected!r}, got {converted!r}")


def validate_lines(output, patterns, context):
    check(output.endswith("\n"), f"Incomplete {context} output: {output!r}")
    for line in output.splitlines():
        check(any(re.fullmatch(pattern, line) for pattern in patterns),
              f"Unexpected {context} output: {line!r}")


def quote_fields(number):
    return {"version": 1 if number % 2 else 2, "price": 10000 + number, "quantity": number * 10}


def provenance(number):
    # Queue order stays 1..6 while the replacement Feed starts publication IDs
    # at one under incarnation two. They are deliberately different counters.
    return {"source_incarnation": 1 if number <= 4 else 2,
            "source_publication": number if number <= 4 else number - 4}


class Application:
    """One public role, with bounded pipe readiness and a retained transcript."""

    def __init__(self, executable, arguments, transcript):
        self.transcript = transcript
        self.output = bytearray()
        self.finished = False
        self.process = subprocess.Popen(
            [str(executable), *map(str, arguments)], stdin=subprocess.PIPE,
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, bufsize=0,
        )

    def text(self):
        return self.output.decode("utf-8")

    def save(self):
        self.transcript.write_bytes(self.output)

    def expect_ready(self, pattern):
        deadline = time.monotonic() + 10
        with selectors.DefaultSelector() as selector:
            selector.register(self.process.stdout, selectors.EVENT_READ)
            while not any(re.fullmatch(pattern, line)
                          for line in self.text().split("\n")[:-1]):
                remaining = deadline - time.monotonic()
                check(remaining > 0 and selector.select(remaining),
                      f"Timed out waiting for {pattern!r}: {self.text()!r}")
                chunk = os.read(self.process.stdout.fileno(), 4096)
                check(chunk, f"Application closed before {pattern!r}: {self.text()!r}")
                self.output.extend(chunk)
                check(len(self.output) <= 65536, "Application report exceeds the walkthrough bound")
                self.save()
        check(self.process.poll() is None, f"Application exited instead of holding: {self.text()!r}")
        return self.text()

    def finish(self, killed=False):
        remaining, _ = self.process.communicate(timeout=10)
        self.output.extend(remaining)
        self.finished = True
        self.save()
        expected = -signal.SIGKILL if killed else 0
        check(self.process.returncode == expected,
              f"Application exited {self.process.returncode}, expected {expected}: {self.text()!r}")
        return self.text()

    def kill(self):
        check(self.process.poll() is None, "Application exited before the deliberate termination")
        self.process.kill()
        return self.finish(killed=True)

    def close(self):
        try:
            if not self.finished:
                if self.process.poll() is None:
                    self.process.kill()
                remaining, _ = self.process.communicate(timeout=10)
                self.output.extend(remaining)
                self.finished = True
                self.save()
        finally:
            for stream in (self.process.stdin, self.process.stdout):
                if stream is not None and not stream.closed:
                    stream.close()


class Walkthrough:
    def __init__(self, executable, workspace):
        self.executable = executable
        self.workspace = workspace
        self.logs = workspace / "logs"
        self.logs.mkdir()
        self.children = []

    def command(self, label, *arguments, succeeds=True):
        result = subprocess.run(
            [str(self.executable), *map(str, arguments)], stdin=subprocess.DEVNULL,
            capture_output=True, text=True, encoding="utf-8", timeout=10,
        )
        (self.logs / f"{label}.log").write_text(
            result.stdout + result.stderr, encoding="utf-8")
        check((result.returncode == 0) == succeeds,
              f"{label} exited {result.returncode}: {result.stdout!r} {result.stderr!r}")
        if succeeds:
            check(result.stderr == "", f"Unexpected {label} diagnostic: {result.stderr!r}")
        return result

    def start(self, label, *arguments):
        child = Application(self.executable, arguments, self.logs / f"{label}.log")
        self.children.append(child)
        return child

    def close(self):
        failures = []
        for child in self.children:
            try:
                child.close()
            except (OSError, subprocess.TimeoutExpired) as error:
                failures.append(str(error))
        check(not failures, f"Could not finish application cleanup: {failures!r}")


def workspace_refusals(example, fixture, workspace, session_file):
    # Exercise the runner's own public command-line guard as well as init's
    # Session conflict. An existing file or dangling symlink is occupied too.
    probes = workspace / "workspace-probes"
    probes.mkdir()
    occupied_file = probes / "occupied-file"
    occupied_file.write_bytes(b"Preserve this existing file.\n")
    occupied_link = probes / "occupied-link"
    occupied_link.symlink_to("missing-target")
    original = session_file.read_bytes()
    for target in (workspace, occupied_file, occupied_link):
        result = subprocess.run(
            [sys.executable, str(Path(__file__).resolve()), str(example), str(fixture),
             "--workspace", str(target)], stdin=subprocess.DEVNULL,
            capture_output=True, text=True, encoding="utf-8", timeout=10,
        )
        check(result.returncode != 0 and "Workspace path already exists" in result.stderr,
              f"Occupied workspace was not rejected: {target}: {result.stdout!r} {result.stderr!r}")
        check(session_file.read_bytes() == original, "Workspace conflict changed the existing Session")
    check(occupied_file.read_bytes() == b"Preserve this existing file.\n", "Workspace probe file was replaced")
    check(occupied_link.is_symlink() and os.readlink(occupied_link) == "missing-target",
          "Workspace probe symlink was replaced")


def hold_eof_control(example, fixture, workspace):
    """Closing control input abandons the borrow before any alert is sent."""
    workspace.mkdir()
    configuration = workspace / "settings.toml"
    shutil.copyfile(fixture, configuration)
    run = Walkthrough(example, workspace)
    try:
        run.command("init", "init", configuration)
        seeded = run.command("feed", "feed", configuration, 1, 1)
        validate_lines(seeded.stdout, [FEED], "EOF-control Feed")
        quotes = records(seeded.stdout, "FEED ", FEED)
        check(len(quotes) == 1, "EOF-control Feed did not publish exactly one Quote")
        expect_fields(quotes[0], {"quote": 1, **quote_fields(1), "task": 2,
                                  "incarnation": 1, "publication": 1}, "EOF-control Feed")

        # command() supplies DEVNULL. EOF is application control, not a receive
        # timeout: receive and explicit close legitimately change native state.
        stopped = run.command("strategy-eof", "strategy", configuration, 1, "--hold-first", succeeds=False)
        check("pipeline.strategy.control" in stopped.stderr,
              f"Strategy did not report its hold EOF: {stopped.stderr!r}")
        validate_lines(stopped.stdout, [STRATEGY_READY, BORROW, HEADER, r"READY strategy-before-forward"],
                       "EOF-control Strategy")
        ready = records(stopped.stdout, "READY strategy task=", STRATEGY_READY)
        check(len(ready) == 1, "EOF-control Strategy omitted its Task")
        expect_fields(ready[0], {"task": 3, "incarnation": 1}, "EOF-control Strategy")
        first = records(stopped.stdout, "BORROW ", BORROW)
        check(len(first) == 1 and stopped.stdout.splitlines().count("READY strategy-before-forward") == 1,
              "Strategy did not stop at its first borrow")
        expected_borrow = {"queue_sequence": 1, "source_task": 2, **provenance(1),
                           **quote_fields(1), "issuance": 1}
        expect_fields(first[0], expected_borrow, "EOF-control borrow")
        first_headers = records(stopped.stdout, "HEADER ", HEADER)
        check(len(first_headers) == 1 and
              first_headers[0]["publication_ns"] == first_headers[0]["enqueue_ns"],
              "EOF-control Strategy omitted the original standard header")

        resumed = run.command("strategy-replacement", "strategy", configuration, 1)
        validate_lines(resumed.stdout, [STRATEGY_READY, BORROW, HEADER, REDELIVER, r"STALE token rejected",
                                       FORWARD, r"STRATEGY completed=1"], "EOF replacement Strategy")
        ready = records(resumed.stdout, "READY strategy task=", STRATEGY_READY)
        check(len(ready) == 1, "EOF replacement omitted its Task")
        expect_fields(ready[0], {"task": 3, "incarnation": 2}, "EOF replacement Strategy")
        borrowed = records(resumed.stdout, "BORROW ", BORROW)
        check(len(borrowed) == 1, "EOF replacement did not recover the one Pending Quote")
        expect_fields(borrowed[0], expected_borrow, "EOF replacement borrow")
        headers = records(resumed.stdout, "HEADER ", HEADER)
        check(len(headers) == 1 and
              headers[0]["publication_ns"] == first_headers[0]["publication_ns"] and
              headers[0]["enqueue_ns"] == first_headers[0]["enqueue_ns"],
              "Hold EOF changed the original publication/enqueue metadata")
        redelivered = records(resumed.stdout, "REDELIVER ", REDELIVER)
        check(len(redelivered) == 1, "EOF replacement missed its deliberate redelivery")
        expect_fields(redelivered[0], {"queue_sequence": 1, "old_issuance": 1, "new_issuance": 2},
                      "EOF replacement redelivery")
        forwarded = records(resumed.stdout, "FORWARD ", FORWARD)
        check(len(forwarded) == 1 and resumed.stdout.splitlines().count("STALE token rejected") == 1 and
              resumed.stdout.splitlines().count("STRATEGY completed=1") == 1,
              "EOF replacement did not forward exactly one alert")
        expect_fields(forwarded[0], {"source_queue_sequence": 1, **provenance(1), **quote_fields(1),
                                    "above_limit": "false", "task": 3, "incarnation": 2, "publication": 1},
                      "EOF replacement alert")

        audited = run.command("audit", "audit", configuration, 1)
        validate_lines(audited.stdout, [AUDIT_READY, AUDIT, r"AUDIT completed=1"], "EOF-control Audit")
        ready = records(audited.stdout, "READY audit task=", AUDIT_READY)
        check(len(ready) == 1, "EOF-control Audit omitted its Task")
        expect_fields(ready[0], {"task": 4, "incarnation": 1}, "EOF-control Audit")
        alerts = records(audited.stdout, "AUDIT input_queue_sequence=", AUDIT)
        check(len(alerts) == 1 and audited.stdout.splitlines().count("AUDIT completed=1") == 1,
              "Hold EOF produced an unexpected extra alert")
        expect_fields(alerts[0], {"input_queue_sequence": 1, "source_task": 2, **provenance(1),
                                 "source_queue_sequence": 1, "price": 10001, "quantity": 10,
                                 "above_limit": "false", "producer_task": 3, "producer_incarnation": 2,
                                 "publication": 1}, "EOF-control Audit")

        # Query only after the replacement has finished, preserving its expected
        # incarnation two. The failed original Strategy created no publication.
        absent = run.command("status-original-strategy", "status", configuration, "Strategy", 1, 1)
        validate_lines(absent.stdout, [STATUS], "EOF-control original Strategy status")
        statuses = records(absent.stdout, "STATUS ", STATUS)
        check(len(statuses) == 1, "EOF-control lookup omitted its report")
        expect_fields(statuses[0], {"task": "Strategy", "incarnation": 1, "publication": 1,
                                   "accepted": "no", "destinations": 0}, "EOF-control absent output")
    finally:
        run.close()


def walkthrough(example, fixture, workspace):
    check(not os.path.lexists(workspace), f"Workspace path already exists: {workspace}")
    check(workspace.parent.is_dir(), f"Workspace parent directory does not exist: {workspace.parent}")
    workspace.mkdir()  # Exclusive creation also refuses a concurrent claimant.
    configuration = workspace / "settings.toml"
    shutil.copyfile(fixture, configuration)
    run = Walkthrough(example, workspace)
    session_file = workspace / "market-session" / "session.ang"
    try:
        initialized = run.command("init", "init", configuration)
        matched = re.fullmatch(
            r"SESSION ([0-9a-f]{32})\nINITIALIZED tasks=4 queues=2 sources=2 schemas=3 ring=16\n",
            initialized.stdout)
        check(matched is not None and matched.group(1) != "0" * 32,
              f"Unexpected initialization: {initialized.stdout!r}")
        original = session_file.read_bytes()
        conflict = run.command("init-conflict", "init", configuration, succeeds=False)
        check("Conflict" in conflict.stderr, f"Repeated init missed Conflict: {conflict.stderr!r}")
        check(session_file.read_bytes() == original, "Repeated init changed the existing Session")

        # Invalid arguments must fail before attaching any Task or consuming an
        # incarnation. This is observable even with a healthy existing Session.
        invalid = [
            ("unknown", configuration), ("feed", configuration, 0, 1),
            ("feed", configuration, 1, 0), ("feed", configuration, "nope", 1),
            ("feed", configuration, 1000, 2), ("feed", configuration, 1, "18446744073709551616"),
            ("feed", configuration, 1, 1, "--unknown"),
            ("strategy", configuration, 0), ("strategy", configuration, 1, "--unknown"),
            ("audit", configuration, 0), ("audit", configuration, "1x"),
            ("status", configuration, "Feed", 0, 1),
            ("status", configuration, "Audit", 1, 1),
        ]
        for index, arguments in enumerate(invalid, 1):
            rejected = run.command(f"invalid-{index:02d}", *arguments, succeeds=False)
            check(rejected.stdout == "" and "InvalidArgument" in rejected.stderr,
                  f"Invalid arguments reached application work: {arguments!r}: {rejected!r}")
            check(session_file.read_bytes() == original, "Invalid CLI arguments changed the existing Session")

        feed = run.start("feed-original", "feed", configuration, 1, 4, "--hold")
        feed_prefix = feed.expect_ready(r"READY feed")
        validate_lines(feed_prefix, [FEED, r"READY feed"], "original Feed")
        first_quotes = records(feed_prefix, "FEED ", FEED)
        check(len(first_quotes) == 4, f"Feed did not accept four quotes: {feed_prefix!r}")
        for number, record in enumerate(first_quotes, 1):
            expect_fields(record, {"quote": number, **quote_fields(number), "task": 2,
                                   "incarnation": 1, "publication": number}, "original Feed")

        occupied = session_file.read_bytes()
        busy = run.command("feed-busy", "status", configuration, "Feed", 1, 1, succeeds=False)
        check("OwnershipBusy" in busy.stderr and busy.stdout == "", "A competing Feed claim was accepted")
        check(session_file.read_bytes() == occupied, "OwnershipBusy changed the active Feed's Session")

        strategy = run.start("strategy-original", "strategy", configuration, 6, "--hold-first")
        strategy_prefix = strategy.expect_ready(r"READY strategy-before-forward")
        validate_lines(strategy_prefix, [STRATEGY_READY, BORROW, HEADER, r"READY strategy-before-forward"],
                       "original Strategy")
        ready = records(strategy_prefix, "READY strategy task=", STRATEGY_READY)
        check(len(ready) == 1, "Original Strategy missed its Task report")
        expect_fields(ready[0], {"task": 3, "incarnation": 1}, "original Strategy")
        borrowed = records(strategy_prefix, "BORROW ", BORROW)
        check(len(borrowed) == 1, "Original Strategy did not hold exactly one borrow")
        expect_fields(borrowed[0], {"queue_sequence": 1, "source_task": 2, **provenance(1),
                                  **quote_fields(1), "issuance": 1}, "retained backlog")
        first_headers = records(strategy_prefix, "HEADER ", HEADER)
        check(len(first_headers) == 1, "Original Strategy omitted its copied header")
        check(first_headers[0]["publication_ns"] == first_headers[0]["enqueue_ns"],
              "Direct publication/enqueue timestamps disagree")

        # The flushed marker precedes any output identity or send. Killing here
        # leaves an unfinished Quote without duplicating an application action.
        check(strategy.kill() == strategy_prefix, "Original Strategy forwarded after its hold marker")
        check(feed.kill() == feed_prefix, "Original Feed changed after its hold marker")
        next_feed = run.command("feed-replacement", "feed", configuration, 5, 2)
        validate_lines(next_feed.stdout, [FEED], "replacement Feed")
        next_quotes = records(next_feed.stdout, "FEED ", FEED)
        check(len(next_quotes) == 2, "Replacement Feed did not publish two quotes")
        for number, record in enumerate(next_quotes, 5):
            expect_fields(record, {"quote": number, **quote_fields(number), "task": 2,
                                   "incarnation": 2, "publication": number - 4}, "replacement Feed")

        audit = run.start("audit", "audit", configuration, 6)
        audit.expect_ready(AUDIT_READY)
        replacement = run.start("strategy-replacement", "strategy", configuration, 6)
        strategy_output = replacement.finish()
        audit_output = audit.finish()
        validate_lines(strategy_output, [STRATEGY_READY, BORROW, HEADER, REDELIVER, r"STALE token rejected",
                                        FORWARD, r"STRATEGY completed=6"], "replacement Strategy")
        ready = records(strategy_output, "READY strategy task=", STRATEGY_READY)
        check(len(ready) == 1, "Replacement Strategy missed its Task report")
        expect_fields(ready[0], {"task": 3, "incarnation": 2}, "replacement Strategy")
        borrows = records(strategy_output, "BORROW ", BORROW)
        forwards = records(strategy_output, "FORWARD ", FORWARD)
        check(len(borrows) == len(forwards) == 6, "Strategy did not recover and forward all six quotes")
        for number, (borrow, forwarded) in enumerate(zip(borrows, forwards), 1):
            expect_fields(borrow, {"queue_sequence": number, "source_task": 2, **provenance(number),
                                  **quote_fields(number), "issuance": 1 if number == 1 else number + 1},
                          "replacement borrow")
            expect_fields(forwarded, {"source_queue_sequence": number, **provenance(number),
                                     **quote_fields(number), "above_limit": "true" if number >= 4 else "false",
                                     "task": 3, "incarnation": 2, "publication": number}, "forwarded alert")
        headers = records(strategy_output, "HEADER ", HEADER)
        check(len(headers) == 1 and headers[0]["publication_ns"] == first_headers[0]["publication_ns"] and
              headers[0]["enqueue_ns"] == first_headers[0]["enqueue_ns"],
              "Consumer replacement changed the original publication/enqueue header")
        redelivered = records(strategy_output, "REDELIVER ", REDELIVER)
        check(len(redelivered) == 1, "Deliberate release did not cause exactly one redelivery")
        expect_fields(redelivered[0], {"queue_sequence": 1, "old_issuance": 1, "new_issuance": 2},
                      "release/redelivery")
        check(strategy_output.splitlines().count("STALE token rejected") == 1 and
              strategy_output.splitlines().count("STRATEGY completed=6") == 1, "Strategy completion markers differ")

        validate_lines(audit_output, [AUDIT_READY, AUDIT, r"AUDIT completed=6"], "Audit")
        audit_ready = records(audit_output, "READY audit task=", AUDIT_READY)
        check(len(audit_ready) == 1, "Audit missed its Task report")
        expect_fields(audit_ready[0], {"task": 4, "incarnation": 1}, "Audit")
        alerts = records(audit_output, "AUDIT input_queue_sequence=", AUDIT)
        check(len(alerts) == 6 and audit_output.splitlines().count("AUDIT completed=6") == 1,
              "Audit did not acknowledge exactly six alerts")
        for number, record in enumerate(alerts, 1):
            expect_fields(record, {"input_queue_sequence": number, "source_task": 2, **provenance(number),
                                   "source_queue_sequence": number, "price": 10000 + number,
                                   "quantity": number * 10, "above_limit": "true" if number >= 4 else "false",
                                   "producer_task": 3, "producer_incarnation": 2, "publication": number},
                          "audited alert")

        # These cold status commands attach new incarnations. Their retained
        # lookup is observational, but attachment legitimately changes claims.
        for task, incarnation, accepted in [("Strategy", 1, "no"), ("Feed", 1, "yes"),
                                            ("Feed", 2, "yes"), ("Strategy", 2, "yes")]:
            status = run.command(f"status-{task}-{incarnation}", "status", configuration, task, incarnation, 1)
            validate_lines(status.stdout, [STATUS], "cold status")
            found = records(status.stdout, "STATUS ", STATUS)
            check(len(found) == 1, "Cold status omitted its publication report")
            expect_fields(found[0], {"task": task, "incarnation": incarnation, "publication": 1,
                                    "accepted": accepted, "destinations": 1 if accepted == "yes" else 0},
                          "cold acceptance lookup")
        workspace_refusals(example, fixture, workspace, session_file)
        hold_eof_control(example, fixture, workspace / "eof-control")
        print("Recovered six recorded quotes across Feed/Strategy replacement.")
        print("Decoded exact Quote v1/v2 layouts and audited six linked alerts.")
        print("Release/redelivery rejected the old token; cold lookups preserved acceptance.")
        print("Initialization, ownership, CLI, and workspace conflicts preserved existing data.")
        print("EOF before forwarding leaves the Quote available for replacement.")
        print("Native pipeline passed.")
    finally:
        run.close()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("example", type=Path, help="built angaria_primer_17_native_pipeline")
    parser.add_argument("configuration", type=Path, help="native-pipeline.toml fixture")
    parser.add_argument("--workspace", type=Path, help="new workspace to retain, including role transcripts")
    arguments = parser.parse_args()
    try:
        example = arguments.example.resolve()
        fixture = arguments.configuration.resolve()
        if arguments.workspace is None:
            with tempfile.TemporaryDirectory(prefix="angaria-native-pipeline-") as temporary:
                walkthrough(example, fixture, Path(temporary) / "pipeline")
        else:
            workspace = arguments.workspace.absolute()
            walkthrough(example, fixture, workspace)
            print(f"Workspace retained at {workspace}")
    except (OSError, RuntimeError, ValueError, UnicodeError, subprocess.TimeoutExpired) as error:
        print(f"native_pipeline: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
