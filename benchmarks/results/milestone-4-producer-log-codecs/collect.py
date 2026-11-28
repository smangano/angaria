#!/usr/bin/env python3
"""Retain ten real invocations of the focused codec test; never overwrite runs."""

import argparse
import hashlib
import json
from pathlib import Path
import platform
import subprocess
import sys


def describe(argv):
    result = subprocess.run(argv, capture_output=True, text=True)
    return {"argv": argv, "exit_code": result.returncode,
            "stdout": result.stdout.strip(), "stderr": result.stderr.strip()}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("binary", type=Path)
    parser.add_argument("--output", type=Path, default=Path(__file__).resolve().parent)
    options = parser.parse_args()
    root = Path(__file__).resolve().parents[3]
    binary = options.binary.resolve()
    output = options.output.resolve()
    raw = output / "raw"
    if raw.exists() and any(raw.iterdir()):
        parser.error("output raw directory is not empty; preserve it and choose a new output")
    raw.mkdir(parents=True, exist_ok=True)
    names = subprocess.check_output(["git", "ls-files", "-z"], cwd=root).decode().split("\0")
    inputs = {name: hashlib.sha256((root / name).read_bytes()).hexdigest()
              for name in names if name and (Path(name).suffix in (".cpp", ".h", ".hpp", ".in")
                                            or name in ("CMakeLists.txt", "VERSION"))}
    manifest = {
        "framework_version": (root / "VERSION").read_text().strip(),
        "source_commit": subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=root, text=True).strip(),
        "scope": "Private codec study, not append, receive, or transport performance",
        "ownership_contracts": {"owned_borrowed": "Caller-buffer encode and borrowed decode",
                                "legacy_owning": "Format-2 owning encode/decode, not the current receive path"},
        "build_type": "Release", "optimization": "-O3 -DNDEBUG",
        "cxx_flags": "-Wall -Wextra -Werror -g", "library_diagnostics": False,
        "allocation_hooks": "Linked for both profiles; capture OFF during timing; allocating control retains hook overhead",
        "execution_context": "Same executable/default managed execution for both profiles; no elevated measurements",
        "clock": "std::chrono::steady_clock; block means, not per-message tail quantiles",
        "compiler": describe(["/usr/bin/c++", "--version"]),
        "binary_architecture": describe(["/usr/bin/file", str(binary)]),
        "hardware": describe(["/usr/sbin/sysctl", "-n", "machdep.cpu.brand_string"]),
        "os": describe(["/usr/bin/sw_vers"]),
        "coordinator_python": sys.version, "coordinator_architecture": platform.machine(),
        "requested_attempts": 10, "payloads": [64, 256, 1024], "timing_blocks_per_profile": 9,
        "cycles_per_block": 2048, "allocation_guard_cycles": 128, "warmup_cycles_per_profile": 128,
        "binary_sha256": hashlib.sha256(binary.read_bytes()).hexdigest(),
        "library_sha256": hashlib.sha256((binary.parent / "libangaria.a").read_bytes()).hexdigest(),
        "source_sha256": inputs,
        "collector_sha256": hashlib.sha256(Path(__file__).read_bytes()).hexdigest(), "commands": [],
    }
    for run in range(1, 11):
        name = f"run-{run:02}"
        argv = [str(binary), "--gtest_color=no", f"--gtest_output=xml:{raw / (name + '.xml')}"]
        result = subprocess.run(argv, capture_output=True, text=True)
        (raw / (name + ".stdout.txt")).write_text(result.stdout)
        (raw / (name + ".stderr.txt")).write_text(result.stderr)
        rows = [json.loads(line[len("CODEC_PERFORMANCE "):]) for line in result.stdout.splitlines()
                if line.startswith("CODEC_PERFORMANCE ")]
        (raw / (name + ".jsonl")).write_text("".join(json.dumps(row, sort_keys=True) + "\n" for row in rows))
        manifest["commands"].append({"run": name, "argv": argv, "exit_code": result.returncode,
                                     "verified_cases": len(rows)})
        (output / "manifest.json").write_text(json.dumps(manifest, indent=2, sort_keys=True) + "\n")
        if result.returncode or sorted(row["payload_bytes"] for row in rows) != [64, 256, 1024]:
            raise RuntimeError(f"{name} failed; its original evidence is retained")
    print(f"Retained 10 complete attempts / 30 payload cases from {manifest['source_commit']}; {len(inputs)} source hashes")


if __name__ == "__main__":
    main()
