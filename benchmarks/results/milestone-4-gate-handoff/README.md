# Local gate handoff measurements

Raw samples and profiler payloads described below are retained in the maintainer archive, not this checkout. Reports, plots, summaries and provenance remain here. [Archive catalog and restore instructions](../README.md#restore-archived-evidence) identify snapshot `2026-11-28-332118be2291`; restore its payloads before running the verification or figure commands below.

Step **4.15**, framework **0.4.17**. The [guide and risk assessment](../../../docs/performance-gate-handoff.md) explain the process-local preference and results. The [diagnostic method](../../../docs/performance-diagnostics.md) defines geometry, operations, phases, counters, and incomplete outcomes.

## Provenance

The **before** normal executable is the exact retained 0.4.16 binary, SHA256 `862bdc761e9b0f6b0c7cd6a216e37daae681d9ebe8cf1761432fd0da97f2514b`, built from `10053fa7227fd26fc7d77bc0ac31410e164ae716`. It was preserved before rebuilding. Its new measurement checkout is merged staging `80d892d52616c70fc6058daa1858a7f17e63e7a4`, with all **127 measurement input hashes** identical to the earlier measured inputs. The manifest identifies that checkout rather than claiming the preserved binary was compiled there. Original compiler/cache settings were retained beside it.

The **after** executables are built from `5feaaac03350acec53efb8d76aa4e2b8eda21ecc`, version 0.4.17. Only VERSION and the two controller files differ among measured inputs. Later commits add documentation/results only. Builds use Apple clang 21, Release `-O3 -DNDEBUG`, plus `-Wall -Wextra -Werror -g`. Diagnostics are OFF for the primary progress/cost studies and ON for internal attribution.

The local host is Apple M4, ten logical CPUs, 16 GiB, macOS 27.0.1, native ARM64 executables, with an x86_64 Python 3.10.15 coordinator. Measurements occurred November 26, 2026 in New York (November 27 UTC). Sessions use the local Data volume. Builds and local tests finished before measurement. Host isolation, thermal/frequency/affinity controls, and cache flushing were not used. Small-run results do not qualify production tail latency.

## Retained populations and ordering

- [Initial before normal](macos-arm64-before-normal/): 33 validated runs, 30 complete workloads and 3 loaded-stream budget stops.
- [Initial after normal](macos-arm64-after-normal/): 33 validated runs, all complete.
- [After enabled](macos-arm64-after-enabled/): 75 validated runs, all complete, using None/Counts/Timings controls.
- [Alternating serial cost](alternating/): 30 validated complete runs, 15 per variant; five repetitions at H=0/32/128. [order.csv](alternating/order.csv) links adjacent pairs and execution sequence to raw reports.
- [Hosted Linux after](linux-x86_64-after/): 54 validated complete runs, including loaded polling; retains a separate syscall trace.

The initial full matrices ran sequentially: before normal, after normal, after enabled. They use fresh Sessions, sixteen-message windows, four registered channels, eight-entry rings, and the unchanged bytes/Fields/capacity/history controls. Serial/backlog keeps nine active claims; thread/process uses two plus requested idle consumers. Before loaded streams are censored progress observations, not successful sixteen-message throughput samples.

The first sequential comparison showed higher ordinary-call medians in several after cases. To check ordering/load drift rather than silently dismiss that result, the additional study used fifteen adjacent before/after pairs, with AB then BA order alternating (eight AB, seven BA pairs). Each repetition visits H=0,32,128; both variants use fresh Sessions and the same sixteen-message serial workload. Ordering is deterministic, not randomized. The [harness](run_interleaved.py) imports each checkout's own driver/version/metadata globals and reuses its validation/artifact writers. Each manifest records the harness hash. Its order log's whole-invocation duration includes setup/preload and is not per-operation latency.

Each study retains compressed raw reports, `runs.csv`, `summary.csv`, `phases.csv`, and TOML source/executable/compiler/filesystem/command metadata. Alternating raw reports additionally name their pair and sequence. Summaries use median per-run statistics with nearest-rank quantiles; adjacent-pair percentage medians are a different statistic and are labeled separately in analysis. Initial ordinary regressions remain retained alongside the alternating results. Neither study proves zero policy overhead.

The [figure](gate-handoff.svg) combines three loaded normal runs (left) with five alternating H=128 serial runs per variant (right). Left labels are acknowledged/accepted, with sixteen requested; before budget stops are not completion times. Right bars are medians of per-run p50 values, not confidence intervals. [Regenerate it](generate_figure.py) with matplotlib. Benchmark runners need only Python's standard library.

## Linux evidence

[Linux CI run 45925548422](https://github.com/smangano/angaria/actions/runs/45925548422) produced the unchanged downloaded artifact. Its synthetic merge is `fbe3db2816c7d471c6ba1362704160e0b7e7e29b`; all 127 inputs match the measured after source. The 18-case CI suite uses eight-message windows and completes all 54 workloads, including all six loaded streams. Hosted scheduling/compiler/hardware differ from the Mac, so absolute timings are not a controlled platform comparison or fairness guarantee.

The separate [syscalls.txt](linux-x86_64-after/syscalls.txt) covers setup, H=32 preload, eight serial messages, controls, and teardown. Counts match the preceding .16 trace: 123,425 `getpid`, 68,754 `fstat`, 40,914 `pread64`, 20,289 `mmap`, and 20,268 `munmap`. This step changes local admission rather than native work in the serial control. Do not divide whole-program counts by eight or use traced percentages/seconds as ordinary latency evidence.

## Reproduce

Build matching before/after checkouts at the recorded commits, and run each checkout's matching executable/runner with a new output directory:

```sh
python3 -B benchmarks/run_diagnostics.py MATCHING_EXECUTABLE \
  --suite diagnostic --repetitions 3 --output NEW_RESULTS \
  --host-label 'DESCRIBE THE ACTUAL HOST'
```

For the additional cost check, run this retained harness with both source trees and binaries:

```sh
python3 -B run_interleaved.py --before-root BEFORE_CHECKOUT --before-executable BEFORE_BINARY \
  --after-root AFTER_CHECKOUT --after-executable AFTER_BINARY \
  --output NEW_COMPARISON --host-label 'DESCRIBE THE ACTUAL HOST'
```

Temporary Session files are removed. Rebuilding may change executable hashes; preserve your new settings/hashes. The comparison directories must be new. Read actual progress/outcomes when a workload reaches its cooperative budget; a matrix finishing successfully does not turn an incomplete workload into a completion.
