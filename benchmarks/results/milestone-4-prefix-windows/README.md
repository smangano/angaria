# Bounded metadata scan window measurements

Raw samples and profiler payloads described below are retained in the maintainer archive, not this checkout. Reports, plots, summaries and provenance remain here. [Archive catalog and restore instructions](../README.md#restore-archived-evidence) identify snapshot `2026-11-28-332118be2291`; restore its payloads before running the verification or figure commands below.

Step **4.14**, framework **0.4.16**. The [guide](../../../docs/performance-prefix-windows.md) explains the optimization and its mapped-I/O risk assessment. The [diagnostic method](../../../docs/performance-diagnostics.md) defines counters, geometry, clock domains, and incomplete outcomes.

## Provenance and populations

The normal **before** executable is the exact retained 0.4.15 executable, SHA256 `a2e7a2f0ff6e856b1bef67396a2f5f099468b56e164521231f71bab10e762b36`, built from `a1d94eb6bc19bae02a7ab7740f486d8b3bc41968`. It was preserved before rebuilding. The new before study uses a detached checkout of merged staging `da182666cdf14f85d3d50c07ed1b02c042a038a1`; all **127 recorded measurement input hashes** match the earlier measured inputs. The manifest identifies that measurement checkout, not a claim that the preserved binary was recompiled there. The earlier compiler settings were preserved beside the executable.

The **after** executables are built from `10053fa7227fd26fc7d77bc0ac31410e164ae716`, version 0.4.16. Only `VERSION` and `src/mutation_journal.cpp` differ among measurement inputs. Later commits add documentation/results only. Both builds use Apple clang 21, Release `-O3 -DNDEBUG`, plus `-Wall -Wextra -Werror -g`. Diagnostics are OFF for the speed comparison and ON for explanatory counts/timings.

The local host is Apple M4, ten logical CPUs, 16 GiB, macOS 27.0.1, with native ARM64 executables. The Python 3.10.15 coordinator is x86_64. Measurement date is November 26, 2026 in New York (November 27 UTC). Sessions use the local Data volume. Compilation and local tests finished before timing. Matrices run sequentially: before normal, after normal, after enabled. No randomized ordering, CPU affinity/frequency controls, isolated load, thermal controls, or cache flushing was used. These are three small repetitions per case, not production tail-latency qualification.

- [Before normal](macos-arm64-before-normal/): 33 validated runs, 30 complete workloads and 3 idle-load budget stops.
- [After normal](macos-arm64-after-normal/): 33 validated runs, 30 complete workloads and 3 idle-load budget stops.
- [After enabled](macos-arm64-after-enabled/): 75 validated runs, 69 complete workloads and 6 idle-load budget stops.
- [Hosted Linux after](linux-x86_64-after/): 54 validated enabled runs, 48 complete workloads and 6 idle-load budget stops; includes a separate syscall trace.

The unchanged workload uses a 16-message window, four registered channels, eight-entry rings, and explicit history/bytes/Fields/capacity controls. Serial/backlog retains nine active claims; thread/process studies use two plus requested idle consumers. Histories and active-claim cohorts are not pooled. A budget stop is not a complete-throughput sample.

Each folder retains compressed raw reports, `runs.csv` including every partial/zero-progress run, `summary.csv`, `phases.csv`, and TOML metadata with source/executable hashes, flags, filesystem, exact arguments, and repetitions. Tracked source is clean at measurement start; unrelated untracked workspace copies and new result directories explain `git_dirty=true` in the main checkout. They are neither compiled nor hashed as inputs.

Counts before come from identical-shape [0.4.15 enabled cases](../milestone-4-prefix-reuse/macos-arm64-after-enabled/); fresh normal runs supply the latency comparison. A native read counter counts actual `pread` attempts. Lower read counts now reflect mapped access, not omitted record validation. Mapped extents and cumulative C++ allocation bytes are not peak/resident memory. Idle raw storage can be truncated, while complete per-actor aggregates retain every poll.

The [figure](prefix-windows.svg) shows the normal serial history sweep. Points are medians of three per-run values, with minimum/maximum run values as ranges, not confidence intervals. Latency p50 uses nearest rank. Throughput covers the entire issue/send/receive/ack cycle. The [generator](generate_figure.py) requires matplotlib; the benchmark runner requires only Python's standard library.

## Linux corroboration

[Linux CI run 45922669779](https://github.com/smangano/angaria/actions/runs/45922669779) produced artifact `native-diagnostics-linux-x86_64`, downloaded unchanged. Its checkout is synthetic merge `3f80296c20b73d0465d35e59c2aee0cd0a56cc0b`; all 127 measurement input hashes match `10053fa7227fd26fc7d77bc0ac31410e164ae716`. Its 18-case CI suite uses eight-message windows. Host/compiler/filesystem details remain in its manifest; these hosted absolute times are not a controlled platform or before/after comparison.

All six ordinary two-thread streams and all six independent-process cases complete eight messages in this matrix. All six three-idle-poller streams stop at the budget. This is a better observed two-thread outcome than the earlier hosted .15 matrix, but host scheduling differs and the gate handoff algorithm has not changed. It does not establish fairness under load. The H=128 first-send scan/native counters match the Mac: 2 scans/638 visits, no independent index pass, 324 maps, 791 reads, and 1,409 stats.

The separate [syscalls.txt](linux-x86_64-after/syscalls.txt) covers setup, H=32 preloading, eight serial measured messages, controls, and teardown. It records 123,425 `getpid`, 68,754 `fstat`, 40,914 `pread64`, 20,289 `mmap`, and 20,268 `munmap` calls. Compare logical call totals with the [preceding same-workload trace](../milestone-4-prefix-reuse/linux-x86_64-after/syscalls.txt); do not divide by eight to obtain per-send costs. Tracing and hosted scheduling distort time, so traced percentages/seconds are not speed evidence. Fresh matched normal Mac runs are the primary latency comparison.

## Reproduce

Create baseline/after checkouts at the commits above, build matching normal Release executables and an enabled after executable, then run each checkout's own runner:

```sh
python3 -B benchmarks/run_diagnostics.py PATH_TO_MATCHING_EXECUTABLE \
  --suite diagnostic --repetitions 3 --output NEW_RESULTS_DIRECTORY \
  --host-label 'DESCRIBE THE ACTUAL HOST'
```

Every output and Session directory must be new. Run the matching checkout's version validator for each executable. Rebuilding can change binary hashes; retain your new hashes and compiler/workload details. The preserved old binary is a control for this experiment, not a required distributable artifact.
