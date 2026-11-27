# Validation-only journal manifest traversal

Milestone 4 step **4.26**, framework **0.4.28**, follows [single-destination validation](performance-single-destination.md). It targets temporary owning vectors and payload copies while validating committed history. Twenty matched native pairs per history show a 9.42% complete-cycle throughput gain at H=128, with a noisier low-history result. Matched Linux profiles confirm lower instruction and data-write counts.

## Scope and rationale

A **manifest** describes one committed journal batch: its actor, Registry revisions, ordered record references and redo commands. A **redo command** describes a state update that recovery can apply again. See [the concurrency/recovery contract](concurrency.md) and [the glossary](glossary.md).

Before this change, complete history validation decoded every manifest into an owning reference vector and command vector, copying command payloads. The scan checked the data and used references/revisions without retaining those command objects. The preceding step's H=128 [Cachegrind profile](../benchmarks/results/milestone-4-single-destination/cachegrind-comparison/after/h128-r1/annotate.txt) attributes 18,065,248 of 184,661,441 instruction events (9.78%) to manifest/command decoding. Allocation code has additional cost across the program. These observations identify avoidable construction; they do not establish a predicted percentage latency improvement or attribute all allocations to these decoders.

The change validates bounded manifest/command bytes synchronously during the scan. Shared parsing retains range, alignment, identity, revision, ordered-reference, opcode, target, payload, reserved-byte and padding checks. Owning decoding remains available for recovery and command application. Both paths share the manifest framing parser, metadata/reference checks and command walker. The private `ManifestView` contains scalar metadata and borrowed little-endian reference bytes; it does not reinterpret them as aligned host integers. Public APIs, stored formats, synchronization, recovery guarantees and scan frequency remain unchanged.

## Risk assessment

The [performance/correctness guidance](../AGENTS.md#performance-and-correctness) supports eliminating allocation and copying where validation needs only bounded input. The important boundary is the lifetime of those bytes. Scan-local scratch and mapping leases must remain valid until the validation and reference comparisons finish. A span backed by a temporary file-read vector must never escape that vector's lifetime. Recovery still needs owned command payloads across later reads and writes.

Malformed input must reject at the same boundary with the same error code, path and message. Shared checks prevent validation-only and owning paths from accepting different formats. Committed-prefix validation must still inspect every command even though it does not execute it. A corrupt command late in historical data must reject before a new mutation changes the Session.

Focused private format tests and committed-history corruption tests qualify these boundaries, alongside existing small/large/small scratch, partial-read, rollback and interrupted-recovery tests. The seventeen primer programs retain the same public APIs, examples and ownership rules; this internal change requires no lesson update.

## Matched measurements

Use the exact preserved 0.4.27 executables and a separate merged-base checkout, then strict 0.4.28 builds with the same compiler and flags. Benchmark commands use matching default execution requests; Git/worktree setup remains separate. Complete builds/tests before timing and retain all workload outcomes.

Before/after enabled matrices provide allocation, native-call, scan and recovery counters. The primary ordinary timing study fixes twenty adjacent alternating pairs per H=0/32/128 before launch; H=128 operation plots are selected before measurement because repeated construction grows with retained history. All eleven matched normal controls use three pairs. Report ranges and unfavorable observations rather than only improved medians.

The optional `profile-cachegrind-compare` PR label builds immutable base/head on one Linux job with identical Release flags, tools and the fixed cache/branch model. Each version supplies nine profiles: sixteen complete cycles, three repetitions at each history. Profile events are separate work evidence, not native latency or retired hardware instructions. Preserve source/executable hashes, raw reports, order logs and reproducible figures in the [step results](../benchmarks/results/milestone-4-manifest-validation/README.md).

## Qualification

Strict local Release diagnostics OFF/ON each pass **830/830 tests**, in 16.82/19.15 seconds. Five focused tests add independent golden command/manifest cases, exact error parity for malformed input and historical-command rejection before any mutation writes, followed by repair. Existing scratch-reuse, crash-recovery, public ownership tests, demos and all seventeen primer programs remain covered. All 336 native benchmark invocations complete their sixteen-message workloads. Source CI passes **830/830** on Linux x86_64 (9.31 seconds), Linux ARM64 (13.56 seconds) and macOS ARM64 (25.03 seconds). All eighteen matched Cachegrind profiles pass. Final documentation/data commits receive ordinary CI before the PR is marked ready.

## Native observations

Twenty adjacent normal-build pairs per H on Apple M4 show median complete-cycle throughput changes of **+1.89%/+4.83%/+9.42%** at H=0/32/128. H means messages already sent and acknowledged before each sixteen-message window. At H=128, ID/send/receive/acknowledge medians of per-run p50 values fall **18.57%/5.98%/7.92%/8.02%**. ID issuance also benefits because it validates the committed journal prefix. The small H=0 timing effect is noisy, including a slower after run; full ranges and all controls are retained.

All **603** matched serial Counts sample dictionaries at H=0/32/128 retain identical nonallocation counters. First-cycle H=128 allocation requests fall from **1144→133** for ID issuance, **4420→3409** for send, **3958→2940** for receive and **3846→2828** for acknowledgment. Each operation retains its one prefix scan and 319/319/321/321 visited records respectively. Existing native I/O, gate, recovery, topology and Publication-validation work remains. These are successful C++ allocation requests and requested bytes within the measured operation, not live memory usage.

## Profile observations

The [same-job Linux comparison](https://github.com/smangano/angaria/actions/runs/46024481479) profiles immutable base/head with GCC 13.3.0 and Valgrind 3.22.0, diagnostics OFF and the existing fixed cold cache/branch model. At H=0/32/128, instruction events per completed cycle fall **4.61%/6.53%/8.07%** and modeled data-write references fall **5.05%/7.16%/9.26%**. The H=128 before/after values are 11,541,340.0625→10,609,457.0625 instruction events and 1,808,802.6875→1,641,222.0625 data-write references per cycle. Each profile completes sixteen cycles; each version has three repetitions per history. Model rates, ordinary latency and hardware-retired instructions are distinct measurements; the report retains the full event totals and any unfavorable changes.

Complete prefix scans, repeated topology/Publication validation, remaining allocations and native mapping/atomic-adapter work remain separate costs. This step reduces construction inside each validation pass; it does not change the algorithmic dependence on retained history.
