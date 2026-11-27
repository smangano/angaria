# First validated-prefix optimization measurements

Step **4.13**, framework **0.4.15**. The [optimization guide](../../../docs/performance-prefix-reuse.md) explains the change, risk assessment, and results. The [diagnostic method](../../../docs/performance-diagnostics.md) defines the workload, counters, partial outcomes, and limitations.

## Provenance

The normal **before** executable is the exact retained 0.4.14 diagnostic executable, SHA256 `1bdfb784f0e99e754b7c4ed49593b69a165ad30ce841ec8b5678351b89a6f8ae`, originally built from `d2b84634bfee781818481f39c22bfbc9ac9a6b58`. It was preserved before rebuilding. The new run uses a detached checkout at merged staging commit `617cf167fc4cfc0420ef57b051c75a10745696f8`; all **127 measurement input hashes** in that checkout match the original measured inputs exactly. Its manifest records that measurement checkout, rather than claiming the preserved binary was newly compiled there. The original cached compiler/build settings were preserved beside it.

The **after** executables are built from `a1d94eb6bc19bae02a7ab7740f486d8b3bc41968`, version 0.4.15. Later commits add results/documentation only. Normal and enabled builds use Apple clang 21, Release `-O3 -DNDEBUG`, plus `-Wall -Wextra -Werror -g`. Internal diagnostics are OFF for the latency comparison, ON for the explanatory study. Enabling CTest in the after normal build adds test targets; it does not change library/diagnostic-executable compile definitions or link inputs. Per-executable hashes and flags are in the manifests.

The host is Apple M4, ten logical CPUs, 16 GiB, macOS 27.0.1, with native ARM64 executables and an x86_64 Python 3.10.15 coordinator. Measurements occurred November 26, 2026, New York time (November 27 UTC). Sessions are temporary files on the local Data volume. Compilation and local tests finished before timing began. Case order is sequential: complete before-normal matrix, after-normal matrix, then after-enabled matrix; it is not randomized. CPU affinity/frequency, thermals, external host load, and cache state are not controlled. These observations are not production tail-latency qualification.

Each matrix uses fresh Sessions and three repetitions per case. The 16-message window, four registered channels, eight-entry rings, payload/schema controls, active claim/incarnation cohorts, cooperative work budgets, and cleanup remain the unchanged diagnostic workload. Serial/backlog use nine active claims; stream/process use two plus any requested idle consumers. Incomplete streams are explicitly separate from completed throughput.

## Retained files

- [Before normal](macos-arm64-before-normal/): 33 validated runs from the unchanged 0.4.14 executable.
- [After normal](macos-arm64-after-normal/): 33 validated runs from 0.4.15, with internal observations compiled away.
- [After enabled](macos-arm64-after-enabled/): 75 validated runs with None, Counts, and Timings controls.
- [Hosted Linux after](linux-x86_64-after/): 54 validated enabled runs, including a separate whole-program syscall trace.

Each contains compressed raw operation reports, `runs.csv` including incomplete/zero-progress invocations, `summary.csv`, `phases.csv`, and TOML provenance with exact arguments and source hashes. A manifest status of complete means the matrix finished and every recorded outcome validated; budget stops remain incomplete workloads. Raw idle-call storage can be truncated, while complete aggregate counters and times cover every poll. Cumulative allocation requests/bytes and requested mapping extents are not peak/resident memory.

The [comparison figure](prefix-reuse.svg) uses ordinary-build serial runs at H=0,32,128. Each point is the median of three per-run values; ranges are their minimum/maximum, not confidence intervals. Latency p50 uses nearest rank within each run. Throughput includes the complete issue/send/receive/ack window. [Regenerate it](generate_figure.py) with matplotlib; the benchmark runner itself needs only Python's standard library.

## Linux corroboration

[Linux CI run 45919149304](https://github.com/smangano/angaria/actions/runs/45919149304) produced artifact `native-diagnostics-linux-x86_64`, downloaded unchanged. Its checkout is synthetic merge `07933ec0c921202ae4d3f579addb04cad2ac1791`; all 127 measurement input hashes match source `a1d94eb6bc19bae02a7ab7740f486d8b3bc41968`. Its 18-case CI suite has eight-message windows, not sixteen. Compiler/host/filesystem details remain in its manifest; cross-host wall times are not a controlled performance comparison.

Of 54 runs, 43 complete and 11 stop at the cooperative budget. Five of six two-thread streams and all six three-idle-poller streams stop early; all six independent-process cases complete. The same progress problem remains visible. H=128 first-send native counts match macOS: 2 scans/638 record visits, no independent index pass, 958 mappings, 3,007 reads, and 5,215 stats.

The additional [syscalls.txt](linux-x86_64-after/syscalls.txt) trace covers setup, 32 historical messages, eight serial measured messages, controls, and teardown. It records 438,501 `getpid`, 304,722 `fstat`, 177,562 `pread64`, 59,843 `mmap`, and 59,822 `munmap` calls. These totals decrease from the same-workload [step-4.12 trace](../milestone-4-diagnostics/linux-x86_64/syscalls.txt), while process-ownership checks remain frequent. Do not divide whole-program counts by eight for per-send counts, or treat traced timings as ordinary-operation latency. The paired normal Mac measurements are the speed evidence.

## Reproduce

Create a baseline checkout at `617cf167fc4cfc0420ef57b051c75a10745696f8` and an after checkout at `a1d94eb6bc19bae02a7ab7740f486d8b3bc41968`. Build a normal Release executable in each, plus an enabled after executable, with the matched flags above and `ANGARIA_ENABLE_DIAGNOSTICS=OFF`/`ON`. In each matching checkout run its own runner, which validates the framework version against that checkout:

```sh
python3 -B benchmarks/run_diagnostics.py PATH_TO_MATCHING_EXECUTABLE \
  --suite diagnostic --repetitions 3 --output NEW_RESULTS_DIRECTORY \
  --host-label 'DESCRIBE THE ACTUAL HOST'
```

Use new output and Session directories. Recreating the exact original binary is not required to reproduce the algorithm comparison, but keep compiler/optimization/workload settings matched and record the new hashes. Do not use one checkout's version validator to label a different executable's code.
