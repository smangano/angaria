#!/usr/bin/env python3
"""Alternate normal before/after binaries over each driver's diagnostic matrix."""
import argparse
import csv
from datetime import datetime, timezone
import importlib.util
from pathlib import Path
import sys
import tempfile
import time
from types import SimpleNamespace

sys.dont_write_bytecode = True
ROOT = Path(__file__).resolve().parent
spec = importlib.util.spec_from_file_location(
    "matched_driver_loader", ROOT.parent / "milestone-4-gate-handoff/run_interleaved.py")
loader = importlib.util.module_from_spec(spec)
spec.loader.exec_module(loader)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for variant in ("before", "after"):
        parser.add_argument("--" + variant + "-root", type=Path, required=True)
        parser.add_argument("--" + variant + "-executable", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True, help="new comparison directory")
    parser.add_argument("--host-label", default="")
    args = parser.parse_args()
    args.output.mkdir()  # Existing files, directories and symlinks are rejected.
    options = SimpleNamespace(suite="diagnostic", repetitions=3, timeout=90,
                              smoke=False, host_label=args.host_label)
    contexts, order = {}, []
    try:
        for variant in ("before", "after"):
            root = getattr(args, variant + "_root").resolve(strict=True)
            executable = getattr(args, variant + "_executable").resolve(strict=True)
            driver = loader.load_driver(root, "matched_controls_" + variant)
            folder = args.output / (variant + "-normal")
            folder.mkdir()
            manifest, sources, settings = driver.experiment_metadata(executable, options)
            cases = driver.matrix(options, False)
            driver.check(len(cases) == 11, "Expected eleven unchanged normal diagnostic cases")
            manifest.update(
                status="failed", instrumentation_enabled=False,
                comparison="matched-normal-controls",
                comparison_harness_sha256=driver.sha256(Path(__file__)),
                execution_context_request="use_default; both variants launched by the same coordinator",
                order_method="33 adjacent pairs; AB then BA alternating; eleven diagnostic cases per repetition; three repetitions")
            contexts[variant] = SimpleNamespace(
                driver=driver, executable=executable, folder=folder, manifest=manifest,
                sources=sources, settings=settings, cases=cases, reports=[])
        contexts["after"].driver.check(contexts["before"].cases == contexts["after"].cases,
                                         "Before/after diagnostic matrices differ")
        pair = 0
        with tempfile.TemporaryDirectory(prefix="angaria-diagnostics-") as temporary:
            for repetition in range(1, options.repetitions + 1):
                for index in range(len(contexts["before"].cases)):
                    pair += 1
                    variants = ("before", "after") if pair % 2 else ("after", "before")
                    for variant in variants:
                        context = contexts[variant]
                        values = context.cases[index]
                        sequence = len(order) + 1
                        row = dict(sequence=sequence, pair=pair, case=index + 1,
                                   repetition=repetition, variant=variant, **values,
                                   started_utc=datetime.now(timezone.utc).isoformat(),
                                   invocation_seconds=0.0, outcome="failed")
                        order.append(row)
                        begin = time.monotonic()
                        try:
                            with tempfile.TemporaryDirectory(prefix="run-", dir=temporary) as run:
                                report = context.driver.execute_case(
                                    context.executable, Path(run) / "session", values, options.timeout)
                            context.driver.validate(report, values, False)
                            context.reports.append(dict(report, repetition=repetition,
                                comparison_case=index + 1, comparison_pair=pair,
                                comparison_sequence=sequence))
                            row["outcome"] = report["outcome"]
                            # Retain a validated budget stop before failing; it
                            # must never be reported as a completed workload.
                            context.driver.check(report["outcome"] == "complete",
                                                 "Matched normal control did not complete")
                        finally:
                            row["invocation_seconds"] = time.monotonic() - begin
                        print(f"pair {pair}/33 {variant} case {index + 1}/11: "
                              f"{values['workload']} H={values['history']} complete", flush=True)
        for context in contexts.values():
            context.manifest["status"] = "complete"
    except Exception as error:
        for context in contexts.values():
            context.manifest["failure"] = str(error)
        raise
    finally:
        for context in contexts.values():
            completed = sum(report["outcome"] == "complete" for report in context.reports)
            context.manifest.update(
                finished_utc=datetime.now(timezone.utc).isoformat(),
                recorded_runs=len(context.reports), completed_runs=completed,
                completed_workloads=completed,
                budget_exceeded_runs=sum(report["outcome"] == "budget_exceeded"
                                         for report in context.reports))
            context.driver.write_artifacts(context.folder, context.reports, context.manifest,
                context.sources, context.settings, context.cases, context.executable)
        with (args.output / "order.csv").open("w", newline="", encoding="utf-8") as output:
            if order:
                writer = csv.DictWriter(output, fieldnames=list(order[0]))
                writer.writeheader()
                writer.writerows(order)


if __name__ == "__main__":
    main()
