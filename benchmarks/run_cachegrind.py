#!/usr/bin/env python3
"""Profile nine bounded serial cycles with an explicit simulated cache model.

Setup, preload, controls and teardown are outside the instrumented region.
Instrumented elapsed times are retained for troubleshooting, never native
latency claims. Every attempted profile, including a failed one, is retained.
"""
import argparse
import csv
from datetime import datetime, timezone
import json
from pathlib import Path
import re
import statistics
import subprocess
import sys
import tempfile
import time
from types import SimpleNamespace

sys.dont_write_bytecode = True
import run_diagnostics as driver


HISTORIES = (0, 32, 128)
EVENTS = ("Ir", "I1mr", "ILmr", "Dr", "D1mr", "DLmr", "Dw", "D1mw", "DLmw",
          "Bc", "Bcm", "Bi", "Bim")
EVENT_LABELS = (
    "Ir: executed instructions", "I1mr: simulated first-level instruction read misses",
    "ILmr: simulated last-level instruction read misses", "Dr: data reads",
    "D1mr: simulated first-level data read misses", "DLmr: simulated last-level data read misses",
    "Dw: data writes", "D1mw: simulated first-level data write misses",
    "DLmw: simulated last-level data write misses", "Bc: conditional branches",
    "Bcm: simulated conditional branch mispredictions", "Bi: indirect branches",
    "Bim: simulated indirect branch mispredictions")
DERIVED = ("data_references", "d1_data_misses", "ll_data_misses", "ll_total_misses",
           "branches", "branch_mispredictions")
CACHE_MODEL = {"I1": "32768,8,64", "D1": "32768,8,64", "LL": "8388608,16,64"}
CACHE_OPTIONS = ("--tool=cachegrind", "--instr-at-start=no", "--cache-sim=yes",
                 "--branch-sim=yes") + tuple(f"--{name}={value}" for name, value in CACHE_MODEL.items())


def parse_profile(path):
    events, totals = None, None
    for line in path.read_text(encoding="utf-8", errors="replace").splitlines():
        if line.startswith("events:"):
            driver.check(events is None, "Repeated Cachegrind events header")
            events = line.split()[1:]
        elif line.startswith("summary:"):
            driver.check(totals is None, "Repeated Cachegrind summary")
            values = line.split()[1:]
            driver.check(all(re.fullmatch(r"[0-9]+", value) for value in values),
                         "Invalid Cachegrind summary count")
            totals = [int(value) for value in values]
    driver.check(events is not None and totals is not None,
                 "Cachegrind profile omits its events or summary")
    driver.check(len(events) == len(set(events)) == len(totals) and set(events) == set(EVENTS),
                 "Cachegrind summary does not match the requested cache/branch events")
    counts = dict(zip(events, totals))
    driver.check(counts["Ir"] > 0, "Cachegrind recorded no region instructions")
    driver.check(counts["Dr"] + counts["Dw"] > 0 and counts["Bc"] + counts["Bi"] > 0,
                 "Cachegrind recorded no region data references or branches")
    counts.update(data_references=counts["Dr"] + counts["Dw"],
                  d1_data_misses=counts["D1mr"] + counts["D1mw"],
                  ll_data_misses=counts["DLmr"] + counts["DLmw"],
                  ll_total_misses=counts["ILmr"] + counts["DLmr"] + counts["DLmw"],
                  branches=counts["Bc"] + counts["Bi"],
                  branch_mispredictions=counts["Bcm"] + counts["Bim"])
    return counts


def write_artifacts(output, rows, manifest, sources, settings, cases, executable):
    columns = ("history", "repetition", "outcome", "benchmark_validated", "benchmark_outcome",
               "completed", "returncode", "instrumented_invocation_seconds", "failure",
               "artifact_directory") + EVENTS + DERIVED
    with (output / "runs.csv").open("w", newline="", encoding="utf-8") as stream:
        writer = csv.DictWriter(stream, fieldnames=columns)
        writer.writeheader()
        writer.writerows(rows)
    columns = ("history", "metric", "completed_profiles", "failed_profiles",
               "median_run_total", "min_run_total", "max_run_total",
               "median_run_per_message", "min_run_per_message", "max_run_per_message")
    with (output / "summary.csv").open("w", newline="", encoding="utf-8") as stream:
        writer = csv.DictWriter(stream, fieldnames=columns)
        writer.writeheader()
        for history in HISTORIES:
            retained = [row for row in rows if row["history"] == history]
            complete = [row for row in retained if row["outcome"] == "complete"]
            if not complete:
                continue
            for metric in EVENTS + DERIVED:
                values = [row[metric] for row in complete]
                writer.writerow(dict(history=history, metric=metric, completed_profiles=len(complete),
                                     failed_profiles=len(retained) - len(complete),
                                     median_run_total=statistics.median(values),
                                     min_run_total=min(values), max_run_total=max(values),
                                     median_run_per_message=statistics.median(values) / 16,
                                     min_run_per_message=min(values) / 16,
                                     max_run_per_message=max(values) / 16))
    with (output / "manifest.toml").open("w", encoding="utf-8") as stream:
        driver.write_toml(stream, manifest, sources, settings, cases, executable)


def tool_versions(options, output):
    versions = {}
    with (output / "versions.txt").open("w", encoding="utf-8") as stream:
        for name, tool in (("valgrind", options.valgrind), ("cg_annotate", options.cg_annotate)):
            result = subprocess.run([tool, "--version"], stdin=subprocess.DEVNULL,
                                    capture_output=True, text=True, errors="replace", timeout=10)
            stream.write(f"{name}: {tool}\n{result.stdout}{result.stderr}\n")
            stream.flush()
            driver.check(result.returncode == 0, f"Could not read {name} version")
            versions[name + "_version"] = (result.stdout.strip() or result.stderr.strip())
    return versions


def profile(options, executable, values, repetition, output, row):
    directory = output / f"h{values['history']}-r{repetition}"
    directory.mkdir()
    row["artifact_directory"] = directory.name
    started = time.monotonic()
    try:
        with tempfile.TemporaryDirectory(prefix="angaria-cachegrind-") as temporary:
            arguments = ([options.valgrind, *CACHE_OPTIONS,
                          "--cachegrind-out-file=" + str(directory / "cachegrind.out")]
                         + driver.command(executable, Path(temporary) / "session", values)
                         + ["--cachegrind-region", "cycle"])
            (directory / "command.json").write_text(json.dumps(arguments, indent=2) + "\n", encoding="utf-8")
            with (directory / "report.json").open("w", encoding="utf-8") as stdout, \
                    (directory / "stderr.txt").open("w", encoding="utf-8") as stderr:
                result = subprocess.run(arguments, stdin=subprocess.DEVNULL, stdout=stdout,
                                        stderr=stderr, timeout=options.timeout)
            row["returncode"] = result.returncode
            driver.check(result.returncode == 0,
                         f"Cachegrind invocation exited {result.returncode}; see {directory / 'stderr.txt'}")
            report = json.loads((directory / "report.json").read_text(encoding="utf-8"))
            row.update(benchmark_outcome=report.get("outcome", "missing"), completed=report.get("completed", ""))
            driver.validate(report, values, False)
            driver.check(report.get("cachegrind_region") == "cycle" and report.get("cachegrind_enabled") is True,
                         "Benchmark did not confirm the enabled cycle instrumentation region")
            driver.check(report["outcome"] == "complete" and report["completed"] == 16,
                         "Instrumented serial workload did not complete all sixteen messages")
            row["benchmark_validated"] = True
            row.update(parse_profile(directory / "cachegrind.out"))
        arguments = [options.cg_annotate, "--auto=no", "--show=" + ",".join(EVENTS),
                     "--sort=Ir", str(directory / "cachegrind.out")]
        (directory / "annotate-command.json").write_text(json.dumps(arguments, indent=2) + "\n", encoding="utf-8")
        remaining = options.timeout - (time.monotonic() - started)
        driver.check(remaining > 0, "Profile deadline expired before function annotation")
        with (directory / "annotate.txt").open("w", encoding="utf-8") as stdout, \
                (directory / "annotate.stderr.txt").open("w", encoding="utf-8") as stderr:
            result = subprocess.run(arguments, stdin=subprocess.DEVNULL, stdout=stdout,
                                    stderr=stderr, timeout=remaining)
        driver.check(result.returncode == 0,
                     f"cg_annotate exited {result.returncode}; see {directory / 'annotate.stderr.txt'}")
        driver.check((directory / "annotate.txt").stat().st_size > 0, "cg_annotate produced no function summary")
        row["outcome"] = "complete"
    except Exception as error:
        row["failure"] = str(error)
        raise
    finally:
        row["instrumented_invocation_seconds"] = time.monotonic() - started


def parse_arguments():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("executable", type=Path)
    parser.add_argument("--output", type=Path, required=True, help="new result directory; existing paths are rejected")
    parser.add_argument("--repetitions", type=int, choices=(3,), default=3)
    parser.add_argument("--timeout", type=int, default=180, help="seconds per instrumented profile including annotation")
    parser.add_argument("--host-label", default="")
    parser.add_argument("--valgrind", default="valgrind")
    parser.add_argument("--cg-annotate", default="cg_annotate")
    options = parser.parse_args()
    if options.timeout <= 0:
        parser.error("--timeout must be positive")
    return options


def main():
    options = parse_arguments()
    executable = options.executable.resolve(strict=True)
    driver.check(executable.is_file(), "Benchmark executable must be a regular file")
    options.output.mkdir(parents=True)  # Refuse files, directories and symlinks already at the destination.
    output = options.output.resolve(strict=True)
    rows, sources, settings = [], {}, {}
    cases = [driver.case(history=history) for history in HISTORIES]
    manifest = dict(started_utc=datetime.now(timezone.utc).isoformat(), status="failed",
                    planned_runs=9, cachegrind_region="cycle", instrumentation_enabled=False,
                    cache_model="Explicit simulation, not the host cache hierarchy; size, ways, line bytes",
                    cache_I1=CACHE_MODEL["I1"], cache_D1=CACHE_MODEL["D1"], cache_LL=CACHE_MODEL["LL"],
                    cachegrind_options=list(CACHE_OPTIONS), benchmark_extra_arguments=["--cachegrind-region", "cycle"],
                    event_labels=list(EVENT_LABELS),
                    region_scope="Sixteen serial issue/send/receive/acknowledge cycles; setup/preload/controls/teardown excluded",
                    simulated_state="Instrumentation disabled during preload; cache and branch simulation start at the cycle region",
                    elapsed_time_scope="Instrumented troubleshooting only; not native latency or throughput")
    try:
        metadata, sources, settings = driver.experiment_metadata(
            executable, SimpleNamespace(suite="cachegrind-cycle", smoke=False,
                                        repetitions=options.repetitions, timeout=options.timeout,
                                        host_label=options.host_label))
        manifest.update(metadata)
        manifest["summary_method"] = "median/min/max of three separate region event totals per history; no pooled profiles"
        cache = Path(manifest["cmake_cache"])
        if cache.is_file():
            for line in cache.read_text(encoding="utf-8", errors="replace").splitlines():
                if line.startswith("ANGARIA_ENABLE_CACHEGRIND:"):
                    settings["ANGARIA_ENABLE_CACHEGRIND"] = line.split("=", 1)[1]
        driver.check(settings.get("ANGARIA_ENABLE_CACHEGRIND") == "ON"
                     and settings.get("ANGARIA_ENABLE_DIAGNOSTICS") == "OFF",
                     "Require a recorded CMake build with Cachegrind ON and diagnostics OFF")
        manifest.update(tool_versions(options, output))
        for repetition in range(1, options.repetitions + 1):
            for values in cases:
                row = dict(history=values["history"], repetition=repetition, outcome="failed",
                           benchmark_validated=False, benchmark_outcome="unavailable", completed="",
                           returncode="", instrumented_invocation_seconds=0, failure="", artifact_directory="")
                rows.append(row)
                profile(options, executable, values, repetition, output, row)
                write_artifacts(output, rows, manifest, sources, settings, cases, executable)
                print(f"Profile {len(rows)}/9 H={values['history']} repetition={repetition}: complete", flush=True)
        manifest["status"] = "complete"
    except Exception as error:
        manifest["failure"] = str(error)
        raise
    finally:
        manifest.update(finished_utc=datetime.now(timezone.utc).isoformat(), recorded_runs=len(rows),
                        completed_profiles=sum(row["outcome"] == "complete" for row in rows),
                        failed_profiles=sum(row["outcome"] != "complete" for row in rows),
                        completed_workloads=sum(row["benchmark_validated"] for row in rows))
        write_artifacts(output, rows, manifest, sources, settings, cases, executable)


if __name__ == "__main__":
    main()
