#!/usr/bin/env python3
"""Alternate matched normal binaries to check ordinary serial-call cost."""
import argparse
import csv
from datetime import datetime, timezone
import importlib.util
from pathlib import Path
from types import SimpleNamespace
import sys
import tempfile
import time

sys.dont_write_bytecode = True


def load_driver(root, label):
    # Each driver retains its own checkout/version/metadata globals. Replacing
    # sys.modules for the next import does not change the first module's globals.
    native_spec = importlib.util.spec_from_file_location("run_native", root / "benchmarks/run_native.py")
    native = importlib.util.module_from_spec(native_spec)
    sys.modules["run_native"] = native
    native_spec.loader.exec_module(native)
    spec = importlib.util.spec_from_file_location("comparison_" + label, root / "benchmarks/run_diagnostics.py")
    driver = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = driver
    spec.loader.exec_module(driver)
    return driver


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--before-root", type=Path, required=True)
    parser.add_argument("--before-executable", type=Path, required=True)
    parser.add_argument("--after-root", type=Path, required=True)
    parser.add_argument("--after-executable", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True, help="new comparison directory")
    parser.add_argument("--host-label", default="")
    args = parser.parse_args()
    args.output.mkdir()  # Refuse existing files/directories/symlinks.
    options = SimpleNamespace(suite="alternating-serial-cost", repetitions=5,
                              timeout=90, smoke=False, host_label=args.host_label)
    contexts = {}
    order = []
    for label in ("before", "after"):
        root = getattr(args, label + "_root").resolve(strict=True)
        executable = getattr(args, label + "_executable").resolve(strict=True)
        driver = load_driver(root, label)
        folder = args.output / (label + "-normal")
        folder.mkdir()
        manifest, sources, settings = driver.experiment_metadata(executable, options)
        cases = [driver.case(history=history) for history in (0, 32, 128)]
        manifest.update(status="failed", instrumentation_enabled=False,
                        comparison_harness_sha256=driver.sha256(Path(__file__)),
                        order_method="15 adjacent pairs; AB then BA alternating; H0/32/128 in each repetition")
        contexts[label] = (driver, executable, folder, manifest, sources, settings, cases, [])
    try:
        pair = 0
        with tempfile.TemporaryDirectory(prefix="angaria-diagnostics-") as temporary:
            for repetition in range(1, 6):
                for history in (0, 32, 128):
                    pair += 1
                    labels = ("before", "after") if pair % 2 else ("after", "before")
                    for label in labels:
                        driver, exe, folder, manifest, sources, settings, cases, reports = contexts[label]
                        values = driver.case(history=history)
                        with tempfile.TemporaryDirectory(prefix="run-", dir=temporary) as run:
                            started = datetime.now(timezone.utc).isoformat()
                            begin = time.monotonic()
                            report = driver.execute_case(exe, Path(run) / "session", values, options.timeout)
                            elapsed = time.monotonic() - begin
                        driver.validate(report, values, False)
                        driver.check(report["outcome"] == "complete", "Serial cost study did not complete")
                        reports.append(dict(report, repetition=repetition, comparison_pair=pair,
                                            comparison_sequence=len(order) + 1))
                        order.append(dict(sequence=len(order) + 1, pair=pair, history=history,
                                          repetition=repetition, variant=label, started_utc=started,
                                          invocation_seconds=elapsed))
                        print(f"pair {pair}/15 {label} H={history}: complete", flush=True)
        for context in contexts.values():
            context[3]["status"] = "complete"
    except Exception as error:
        for context in contexts.values():
            context[3]["failure"] = str(error)
        raise
    finally:
        for driver, exe, folder, manifest, sources, settings, cases, reports in contexts.values():
            manifest.update(finished_utc=datetime.now(timezone.utc).isoformat(),
                            recorded_runs=len(reports), completed_runs=len(reports),
                            completed_workloads=len(reports), budget_exceeded_runs=0)
            driver.write_artifacts(folder, reports, manifest, sources, settings, cases, exe)
        with (args.output / "order.csv").open("w", newline="", encoding="utf-8") as output:
            if order:
                writer = csv.DictWriter(output, fieldnames=list(order[0]))
                writer.writeheader()
                writer.writerows(order)


if __name__ == "__main__":
    main()
