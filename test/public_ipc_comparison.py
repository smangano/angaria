#!/usr/bin/env python3
"""Exercise both public messaging modes and nanomsg in independent processes.

This is benchmark-adapter correctness, not a speed threshold or application
primer. The C++ public profiles create and attach real public Sessions/Tasks;
each actual payload, public acceptance lookup, delivery and selected ACK is
checked before the coordinator accepts its retained timing evidence.
"""
import argparse
from pathlib import Path
import shutil
import sys
import tempfile
from types import SimpleNamespace

sys.dont_write_bytecode = True
ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "benchmarks"))
import run_end_to_end as coordinator


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("executable", type=Path)
    parser.add_argument("--output", type=Path)
    options = parser.parse_args()
    directory = options.output.resolve() if options.output else Path(tempfile.mkdtemp(prefix="apublic-smoke-"))
    if options.output:
        directory.mkdir(parents=True, exist_ok=False)
    succeeded = False
    try:
        # Check the same build/provenance route as full collection, without
        # requiring a committed source tree during ordinary developer tests.
        # In particular, common metadata must not require retired drivers.
        _, build = coordinator.build_snapshot(options.executable.resolve(), directory)
        host, sources, settings = coordinator.ipc.comparison_metadata(options.executable.resolve(),
            SimpleNamespace(host_label="public IPC correctness smoke", smoke=True,
                            timeout=30, repetitions=1, angaria_profile="public-recorded"))
        coordinator.write_json(directory / "preflight.json",
                               dict(build=build, host=host, source_sha256=sources, settings=settings))
        cases, attempts = coordinator.plan(True, "public")
        results = []
        for request in attempts:
            target = directory / ("attempt-%03d" % request["attempt"])
            target.mkdir()
            result = coordinator.execute(options.executable.resolve(), request["profile"], cases[request["case"]],
                                         target, (ROOT / "VERSION").read_text().strip(), 30, 30)
            coordinator.ipc.check(result["status"] == "complete" and result["measured_count"] == 32,
                                  "Incomplete public adapter smoke")
            expected_ack = cases[request["case"]]["messages"] if request["profile"] == "public-acknowledged" else 0
            coordinator.ipc.check(result["acknowledged"] == expected_ack,
                                  "Public acknowledgment boundary changed")
            results.append(dict(request, **result))
        coordinator.write_json(directory / "results.json", results)
        print("Validated %d public/nanomsg process pairs; every payload and selected acknowledgment checked" % len(results))
        succeeded = True
    finally:
        if succeeded and not options.output:
            shutil.rmtree(directory)
        else:
            print("Retained public IPC smoke evidence: " + str(directory), file=sys.stderr)


if __name__ == "__main__":
    main()
