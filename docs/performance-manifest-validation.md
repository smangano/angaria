# Validation-only journal manifest traversal

Milestone 4 step **4.26**, framework **0.4.28**, follows [single-destination validation](performance-single-destination.md). It targets temporary owning vectors and payload copies while validating committed history. Measured effects and final qualification are pending.

## Scope and rationale

A **manifest** describes one committed journal batch: its actor, Registry revisions, ordered record references and redo commands. A **redo command** describes a state update that recovery can apply again. See [the concurrency/recovery contract](concurrency.md) and [the glossary](glossary.md).

Complete history validation currently decodes every manifest into an owning reference vector and command vector, copying command payloads. The scan checks the data and uses references/revisions; it does not retain those command objects. The latest H=128 [Cachegrind profile](../benchmarks/results/milestone-4-single-destination/cachegrind-comparison/after/h128-r1/annotate.txt) attributes 18,065,248 of 184,661,441 instruction events (9.78%) to manifest/command decoding. Allocation code has additional cost across the program. These observations identify avoidable construction; they do not establish a predicted percentage latency improvement or attribute all allocations to these decoders.

The change validates bounded manifest/command bytes synchronously during the scan. Shared parsing retains range, alignment, identity, revision, ordered-reference, opcode, target, payload, reserved-byte and padding checks. Owning decoding remains available for recovery and command application. Both paths share the manifest framing parser, metadata/reference checks and command walker. The private `ManifestView` contains scalar metadata and borrowed little-endian reference bytes; it does not reinterpret them as aligned host integers. Public APIs, stored formats, synchronization, recovery guarantees and scan frequency remain unchanged.

## Risk assessment

The [performance/correctness guidance](../AGENTS.md#performance-and-correctness) supports eliminating allocation and copying where validation needs only bounded input. The important boundary is the lifetime of those bytes. Scan-local scratch and mapping leases must remain valid until the validation and reference comparisons finish. A span backed by a temporary file-read vector must never escape that vector's lifetime. Recovery still needs owned command payloads across later reads and writes.

Malformed input must reject at the same boundary with the same error code, path and message. Shared checks prevent validation-only and owning paths from accepting different formats. Committed-prefix validation must still inspect every command even though it does not execute it. A corrupt command late in historical data must reject before a new mutation changes the Session.

Focused private format tests and committed-history corruption tests qualify these boundaries, alongside existing small/large/small scratch, partial-read, rollback and interrupted-recovery tests. The seventeen primer programs retain the same public APIs, examples and ownership rules; this internal change requires no lesson update.

## Measurement plan

Use the exact preserved 0.4.27 executables and a separate merged-base checkout, then strict 0.4.28 builds with the same compiler and flags. Benchmark commands use matching default execution requests; Git/worktree setup remains separate. Complete builds/tests before timing and retain all workload outcomes.

Before/after enabled matrices provide allocation, native-call, scan and recovery counters. The primary ordinary timing study fixes twenty adjacent alternating pairs per H=0/32/128 before launch; H=128 operation plots are selected before measurement because repeated construction grows with retained history. All eleven matched normal controls use three pairs. Report ranges and unfavorable observations rather than only improved medians.

The optional `profile-cachegrind-compare` PR label builds immutable base/head on one Linux job with identical Release flags, tools and the fixed cache/branch model. Each version supplies nine profiles: sixteen complete cycles, three repetitions at each history. Profile events are separate work evidence, not native latency or retired hardware instructions. Preserve source/executable hashes, raw reports, order logs and reproducible figures in the [step results](../benchmarks/results/milestone-4-manifest-validation/README.md).

## Qualification

Strict local Release diagnostics OFF/ON each pass **830/830 tests**, in 16.82/19.15 seconds. Five focused tests add independent golden command/manifest cases, exact error parity for malformed input and historical-command rejection before any mutation writes, followed by repair. Existing scratch-reuse, crash-recovery, public ownership tests, demos and all seventeen primer programs remain covered. Native measurement and CI results are pending.
