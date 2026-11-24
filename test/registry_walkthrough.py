#!/usr/bin/env python3
"""Build and inspect one Registry through independent public-API programs."""

# Unlike the private interruption workers, this script uses only the documented
# example CLI. Its process termination demonstrates claim replacement; journal
# crash-cut/recovery qualification lives in the native Registry tests.
import argparse
import json
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
        stdin=subprocess.DEVNULL,
        capture_output=True,
        text=True,
        timeout=10,
    )
    check(
        (result.returncode == 0) == succeeds,
        f"{arguments!r} exited {result.returncode}: {result.stdout!r} {result.stderr!r}",
    )
    return result.stdout + result.stderr


def expect_line(process, marker):
    expected = marker.encode()
    received = b""
    deadline = time.monotonic() + 10
    with selectors.DefaultSelector() as selector:
        selector.register(process.stdout, selectors.EVENT_READ)
        while expected not in received.splitlines():
            remaining = deadline - time.monotonic()
            check(remaining > 0 and selector.select(remaining), f"Timed out waiting for {marker}: {received!r}")
            chunk = os.read(process.stdout.fileno(), 4096)
            check(chunk, f"Child closed before {marker}: {received!r}")
            received += chunk
            check(len(received) <= 16384, "Unexpectedly large child output")
    check(process.poll() is None, f"Child exited instead of holding at {marker}")
    return received.decode()


def finish(process, prefix=""):
    output, _ = process.communicate(timeout=10)
    check(process.returncode == 0, f"Child exited {process.returncode}: {prefix}{output.decode()}")
    return prefix + output.decode()


def records(output):
    result = {}
    for line in output.splitlines():
        parts = line.split()
        if not parts:
            continue
        values = dict(part.split("=", 1) for part in parts[1:] if "=" in part)
        result.setdefault(parts[0], []).append(values)
    return result


def ids(text):
    return [int(value) for value in text.split(",")] if text else []


def check_snapshot(output):
    """Check relationships within one captured revision, including partial setup."""
    rows = records(output)
    check(len(rows.get("SNAPSHOT", [])) == 1, f"Missing snapshot header: {output!r}")
    header = rows["SNAPSHOT"][0]
    for name, category in (
        ("tasks", "TASK"), ("queues", "QUEUE"), ("sources", "SOURCE"),
        ("publishing", "PUBLISHING"), ("fields", "FIELD"), ("messages", "MESSAGE"),
    ):
        check(int(header[name]) == len(rows.get(category, [])), f"Incorrect {name} count: {output!r}")
    task_ids = {int(row["id"]) for row in rows.get("TASK", [])}
    field_ids = {int(row["id"]) for row in rows.get("FIELD", [])}
    for message in rows.get("MESSAGE", []):
        check(set(ids(message["fields"])) <= field_ids, "Message schema references an absent Field")
    queues = {int(row["id"]): row for row in rows.get("QUEUE", [])}
    sources = {int(row["id"]): row for row in rows.get("SOURCE", [])}
    for source_id, source in sources.items():
        check(int(source["owner"]) in task_ids, "Source owner is not registered in the snapshot")
        expected = {queue_id for queue_id, queue in queues.items() if int(queue["source"]) == source_id}
        check(set(ids(source["members"])) == expected, "Source membership is not the inverse of Queue assignments")
    for queue_id, queue in queues.items():
        check(int(queue["producer"]) in task_ids, "Queue producer is absent")
        check(queue_id in ids(sources[int(queue["source"])]["members"]), "Queue's Source omitted it")
    for publisher in rows.get("PUBLISHING", []):
        for queue_id in ids(publisher["destinations"]):
            check(queues[queue_id]["producer"] == publisher["owner"], "Publishing destination has a different producer")
    check("READ_ONLY ReadOnlyRegistry" in output, "Inspector did not demonstrate its read-only view")
    return rows


def walkthrough(executable, fixture, session, workspace):
    check(not session.exists(), f"Session path already exists: {session}")
    check(session.parent.is_dir(), f"Session parent directory does not exist: {session.parent}")
    configuration = workspace / "registry.toml"
    text = fixture.read_text(encoding="utf-8")
    text, changed = re.subn(
        r'^directory = "primer-session"$', lambda _: "directory = " + json.dumps(str(session), ensure_ascii=False), text, flags=re.MULTILINE,
    )
    check(changed == 1, "Primer fixture no longer contains its documented relative directory")
    configuration.write_text(text, encoding="utf-8")
    processes = []

    def start(*arguments):
        process = subprocess.Popen(
            [str(executable), *map(str, arguments)],
            stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, bufsize=0,
        )
        processes.append(process)
        return process

    try:
        command(executable, "init", configuration)
        initial_output = command(executable, "inspect", session)
        initial = check_snapshot(initial_output)
        original_revision = initial["SNAPSHOT"][0]["revision"]
        check(initial["SNAPSHOT"][0]["tasks"] == "5", "Initialization should leave five declared/setup Tasks")

        # Both independently launched processes own different logical registrars.
        # Release them only after their explicit READY handshakes have arrived.
        registrar_a = start("register", session, "A", "--wait")
        prefix_a = expect_line(registrar_a, "READY registrar=A")
        registrar_b = start("register", session, "B", "--wait")
        prefix_b = expect_line(registrar_b, "READY registrar=B")
        for process in (registrar_a, registrar_b):
            process.stdin.write(b"\n")
            process.stdin.flush()
        # Whichever committed revision this observer captures must be coherent.
        check_snapshot(command(executable, "inspect", session))
        output_a = finish(registrar_a, prefix_a)
        output_b = finish(registrar_b, prefix_b)
        combined = output_a + output_b
        check(combined.count("CONFLICT accepted=") == 1, f"Expected one committed conflicting definition: {combined}")
        check(combined.count("CONFLICT rejected=") == 1, f"Expected one rejected conflicting definition: {combined}")
        shared = re.findall(r"REGISTERED registrar=[AB] shared=(\d+)", combined)
        check(len(shared) == 2 and shared[0] == shared[1], "Exact concurrent Task registrations returned different identities")

        complete_output = command(executable, "inspect", session)
        complete = check_snapshot(complete_output)
        header = complete["SNAPSHOT"][0]
        check(tuple(header[key] for key in ("tasks", "queues", "sources", "publishing", "fields", "messages")) ==
              ("7", "3", "1", "1", "4", "4"), f"Incomplete final catalog: {complete_output}")
        quote_versions = {row["version"] for row in complete["MESSAGE"] if row["name"] == "Quote"}
        check(quote_versions == {"1", "2"}, "Exact Quote versions were not both retained")
        for label in ("A", "B"):
            command(executable, "register", session, label)
        retried = check_snapshot(command(executable, "inspect", session))
        check(retried["SNAPSHOT"][0]["revision"] == header["revision"], "Exact registration/activation retries appended revisions")

        # An owner holds endpoints after its original Task variable has ended.
        # Only this script's child is killed, then reaped before replacement.
        owner = start("owner", session, "--hold")
        expect_line(owner, "READY owner incarnation=1")
        busy = command(executable, "owner", session, succeeds=False)
        check("OwnershipBusy" in busy, f"Second owner did not report OwnershipBusy: {busy}")
        check("CURRENT task=Feed attached=yes incarnation=1" in command(executable, "inspect", session), "Live claim was not observable")
        owner.kill()
        owner.wait(timeout=10)
        check("READY owner incarnation=2" in command(executable, "owner", session), "Replacement did not advance its incarnation")

        before_route = check_snapshot(command(executable, "inspect", session))
        old_revision = before_route["SNAPSHOT"][0]["revision"]
        old_destinations = before_route["PUBLISHING"][0]["destinations"]
        command(executable, "route", session)
        current = check_snapshot(command(executable, "inspect", session))
        historical = check_snapshot(command(executable, "inspect", session, old_revision))
        check(historical["PUBLISHING"][0]["destinations"] == old_destinations, "Historical routing changed with current routing")
        check(len(ids(current["PUBLISHING"][0]["destinations"])) == 2, "Owner did not install its new two-destination route")
        check(current["PUBLISHING"][0]["topology"] == "2", "Changed route did not advance topology generation")

        # All mutators have exited. Metadata and the earliest snapshot remain
        # inspectable; a healthy inspection leaves the saved log byte-identical.
        log = session / "session.ang"
        original_bytes = log.read_bytes()
        offline_output = command(executable, "inspect", session)
        check("CURRENT task=Feed attached=no" in offline_output, "Feed claim remained attached after exit")
        oldest = check_snapshot(command(executable, "inspect", session, original_revision))
        check(oldest["SNAPSHOT"][0]["queues"] == "1", "Initial snapshot gained later queues")
        check(log.read_bytes() == original_bytes, "Healthy offline inspection changed the log")
        print("Registered and discovered every entity kind through independent processes.")
        print("Concurrent retries/conflicts, claim replacement, and historical routing passed.")
        print(f"Inspected current and historical Registry state at {session}")
    finally:
        for process in processes:
            if process.poll() is None:
                process.kill()
            process.wait(timeout=10)
            for stream in (process.stdin, process.stdout):
                stream.close()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("executable", type=Path, help="built angaria_primer_08_registry")
    parser.add_argument("configuration", type=Path, help="primer configuration.toml fixture")
    parser.add_argument("--session", type=Path, help="new Session directory to retain after the walkthrough")
    arguments = parser.parse_args()
    try:
        with tempfile.TemporaryDirectory(prefix="angaria-registry-") as root:
            workspace = Path(root)
            session = arguments.session.absolute() if arguments.session else workspace / "session"
            walkthrough(arguments.executable.resolve(), arguments.configuration.resolve(), session, workspace)
    except (OSError, RuntimeError, ValueError, subprocess.TimeoutExpired) as error:
        print(f"registry_walkthrough: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
