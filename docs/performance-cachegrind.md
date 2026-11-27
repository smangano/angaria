# Profiling the serial transport cycle with Cachegrind

Milestone 4 step **4.24**, framework **0.4.26**, adds an on-demand CPU-work profile after [gate resource-check reduction](performance-gate-resources.md). It profiles an optimized benchmark executable with private diagnostics disabled. Public APIs, stored formats, synchronization and transport algorithms are unchanged. The profile supplies instruction, simulated-cache and simulated-branch evidence for selecting a later optimization; it does not establish an ordinary latency improvement.

## Build and reproduce

Use Linux x86_64 with a C++20 compiler, CMake, Boost headers, Python 3.9 or later, and Valgrind **3.22 or later**, including its headers and `cg_annotate`. On Ubuntu 24.04:

```sh
sudo apt-get update
sudo apt-get install --yes g++ cmake libboost-dev python3 valgrind
cmake -S . -B build/cachegrind -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_CXX_FLAGS=-g -DBUILD_TESTING=OFF \
  -DANGARIA_BUILD_BENCHMARKS=ON \
  -DANGARIA_ENABLE_DIAGNOSTICS=OFF -DANGARIA_ENABLE_CACHEGRIND=ON
cmake --build build/cachegrind --target angaria_transport_diagnostics --parallel 3
python3 -B benchmarks/run_cachegrind.py build/cachegrind/angaria_transport_diagnostics \
  --output build/cachegrind-results --repetitions 3 \
  --host-label "Ubuntu 24.04 x86_64; fixed simulated cache model"
```

The output directory must be new. Release optimization remains `-O3 -DNDEBUG`; `-g` supplies source locations for annotation. The opt-in `ANGARIA_ENABLE_CACHEGRIND` option applies only to the benchmark's profiling hooks and is OFF by default. It requires compatible Cachegrind headers and cannot be combined with `ANGARIA_ENABLE_DIAGNOSTICS=ON`. Ordinary builds and application examples need no Valgrind dependency.

The runner executes **nine fresh-Session cases**: three repetitions each at acknowledged history **H=0, 32 and 128**, followed by **16 new messages** per case. All use the existing serial workload, eight fixed-ASCII payload bytes, one Field, default capacity scale, same-Queue history, no idle pollers and `collection=none`. H describes messages sent and acknowledged before profiling, not the number newly measured.

## Exactly what the profile includes

The benchmark's default `--cachegrind-region none` leaves profiling control inactive. The runner explicitly selects `--cachegrind-region cycle`, with Cachegrind initially uninstrumented through `--instr-at-start=no`. After Session setup, registrations and history preload, the benchmark starts instrumentation once around the existing serial issue/send/receive/acknowledge loop. It stops before status/retry/empty-poll controls, report formatting and Session cleanup. The region includes the loop's ordinary sample/clock bookkeeping as well as its public calls; it is not a sum of isolated library functions.

The report identifies `cachegrind_region` and whether the executable was built with the profiling hooks. The runner checks those fields and the completed workload before treating an attempt as valid. A successful process exit alone is insufficient evidence that the intended region was collected.

Because setup and preload execute without instrumentation, the simulated caches and branch predictor begin cold at the measured loop. Preload still establishes real Session history and can warm the operating system's file cache. This distinction is part of the experiment, rather than a claim to reproduce an already warm native CPU.

## Fixed simulation model and interpretation

Both cache and branch simulation are explicitly enabled. Cache sizes are fixed across all cases instead of inferred from the hosted runner:

| Cache | Bytes | Associativity | Line bytes |
| --- | ---: | ---: | ---: |
| First-level instruction, I1 | 32,768 | 8 | 64 |
| First-level data, D1 | 32,768 | 8 | 64 |
| Unified last-level, LL | 8,388,608 | 16 | 64 |

These settings define a comparison model, not the runner's physical cache hierarchy. `Ir` counts executed instructions; `Dr`/`Dw` count modeled data reads/writes. The `I1mr`/`D1mr`/`D1mw` and `ILmr`/`DLmr`/`DLmw` events describe simulated misses. `Bc`/`Bi` count conditional/indirect branches, and `Bcm`/`Bim` count simulated mispredictions. `cg_annotate --auto=no` retains function summaries; line-level annotations can be generated later from the raw profiles. Inlining and unavailable library symbols affect attribution. See the [Cachegrind manual](https://valgrind.org/docs/manual/cg-manual.html).

The model uses virtual addresses. Repeated mappings of the same file bytes at different addresses can therefore affect simulated misses differently from a physical cache. Kernel execution, TLB behavior, page-fault costs, hardware prefetch/speculation and multicore cache coherence are outside this profile. Valgrind changes execution and scheduling, and address-layout changes can perturb simulation counts. Profiling wall/CPU times are not native speed measurements. Use [ordinary matched timings and native syscall counters](performance-diagnostics.md) for those separate questions.

## Retained evidence

[run_cachegrind.py](../benchmarks/run_cachegrind.py) retains `manifest.toml`, `versions.txt`, `runs.csv` and `summary.csv`. Per-attempt directories such as `h128-r3` retain the raw `cachegrind.out`, validated public report in `report.json`, profiler/process output in `stderr.txt`, and `annotate.txt` plus `annotate.stderr.txt`. The matrix records all attempts and failures. An incomplete or invalid case remains evidence of that attempt, rather than a successful nine-case profile.

Source/executable hashes, compiler/build settings, exact invocation, tool versions, fixed cache settings and host labels accompany the results. Compare repetitions and preserve variation. Derived event totals and per-message counts describe this 16-cycle region; they are not per-call latencies, memory-residency measurements or production-tail bounds. Instruction/cache hotspots guide a proposed change, which still needs its own correctness qualification and matched ordinary measurements.

## Request a GitHub profile

The separate [Cachegrind workflow](../.github/workflows/cachegrind.yml) runs on Ubuntu 24.04 x86_64 with a 30-minute job limit. It builds only `angaria_transport_diagnostics`, then runs the nine cases above. It retains the raw results and matrix as `cachegrind-linux-x86_64-cycle` for **14 days**, including artifacts from a failed profiling attempt. Download any evidence needed for longer-term analysis before expiration.

Add the **`profile-cachegrind`** label to a pull request to request a run for that PR's head commit. The trigger is the `labeled` activity, so later pushes do not automatically repeat the profile. Remove and reapply the label to request the newer commit. Manual dispatch requires the workflow to exist on the default branch; then select the intended ref. GitHub documents the [event and manual-dispatch rules](https://docs.github.com/en/actions/reference/workflows-and-actions/events-that-trigger-workflows).

Step 4.25 adds the alternative label **`profile-cachegrind-compare`** for a [matched base/head profile](performance-single-destination.md#matched-measurements). Both immutable SHAs are checked out and built on the same job with the same flags, tools, model and nine cases per version. Each checkout uses its own runner. The artifact contains separate `before/` and `after/` directories under `build/cachegrind-results/`, each with its own manifest and nine raw profiles. The base must include the profiling hooks and runner. The original label and manual dispatch retain the single-version root layout. Reapply the comparison label after a newer commit when another run is wanted.

The workflow uses read-only repository permissions and checkout without persisted credentials. It does not use `pull_request_target`, secrets or repository-write permissions. Ordinary CI remains responsible for tests and its existing diagnostic matrix; this optional profile is not a speed gate. The seventeen primer programs use the same public calls and require no changes for this research tool.

## Recorded first run

The [Linux results and instruction/cache plot](../benchmarks/results/milestone-4-cachegrind/README.md) retain nine successful profiles from Valgrind 3.22.0. All three repetitions agree exactly at each H. Instructions per completed cycle rise 3.33→5.53→12.12 million at H0/32/128; modeled LL data miss rates are low but do not measure physical-cache behavior. Allocation, repeated manifest/command decoding and an 8 KiB uniqueness bitmap cleared even for one destination provide specific later optimization candidates. No transport optimization or latency improvement is claimed by this profiling step.
