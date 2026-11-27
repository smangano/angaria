# Reusing validated record information

Milestone 4 step **4.13**, framework **0.4.15**, is the first optimization selected from the [diagnostic findings](performance-diagnostics-findings.md). It removes repeated reads of committed record framing during a public operation. The public API, native format, gate protocol, and recovery behavior are unchanged. Milestone integration into `main` remains held for further measured improvements and qualification.

## What changes

A **record boundary** is the exact starting offset of a committed arena record. Its decoded envelope describes the record's kind, size, generation, and immutable/mutable extents. The journal already walks these records, checks their boundaries and framing, and validates their manifest chain before returning a clean snapshot.

Previously RuntimeAccess discarded those decoded envelopes, then read and decoded the entire prefix again to build a lookup index. It now adopts the journal's owning record copies. Exact-boundary lookup reuses the shared `find_journal_record` helper; references into the middle of a record and references of the wrong kind still reject. The returned metadata does not alias mapped storage.

Mutation preflight also used to scan once to validate its clean snapshot and a second time to obtain the same record ranges. The first pass now supplies those ranges directly to local planning. Preflight still obtains its own fresh snapshot; it does not trust a snapshot from an earlier API call.

For a healthy direct mutation, the expected change is **three to two full-prefix scans**, and **one to zero separate runtime index scans**. Status, identical retry, and empty polling retain their fresh prefix validation but lose the separate index scan. Diagnostic counter/phase names remain available, so old and new reports can be compared without changing their schema.

## Risk assessment

Performance and correctness have equal standing under the [project guidance](../AGENTS.md#performance-and-correctness). This change removes repeated work rather than adding a long-lived cache or additional cache-validity machinery.

- Supported Session mutations are coordinated by the held gate. Re-reading an immutable envelope during the same validation/planning pass adds cost without a new supported state transition to observe. Planning between the reused scan and execution constructs local objects; it does not modify the Session.
- Every ordinary journal snapshot and execute entry still validates freshly. Another completed operation, a stale generation with an unchanged tail, failed preflight, and non-idle recovery retain their existing checks. Arbitrary external writes are not made safe by repeatedly rereading bytes within an operation.
- The private snapshot overload clears its output before work and moves record ranges into it only after complete validation. An error exposes neither old ranges nor a partly validated prefix. The copies own their metadata and remain stable while the operation uses them.
- RuntimeAccess retains two additional integer fields per record because it adopts the existing journal range type. That modest transient metadata cost replaces a second allocated vector and its complete reread. Allocation requests, cumulative requested bytes, and native work will be measured; none is a claim about peak or resident memory.

No documented delivery or corruption-rejection guarantee is weakened in this step. This is a scoped decision, not a rule that all current defensive checks must be preserved forever. Further reductions should be assessed against their measured benefits, supported failure model, implementation complexity, and explicitly accepted risks.

## Qualification and measurements

Two new journal tests demonstrate complete record exports across append/fixed-state/append operations and empty output after late historical corruption. Existing tests cover exact boundaries, forged interior references, wrong kinds, fresh access after appends, metadata inspection without retained-payload reads, stale plans, no-write rejection, process death, recovery, rollback, Source cleanup, and deadlines.

The measurement plan uses the unchanged public diagnostic workload and preserves the exact pre-change executables. Normal Release builds establish latency/throughput differences; enabled builds explain scan, read, mapping, stat, and allocation changes. Fresh Sessions, fixed geometry, matched history/bytes/Fields/capacities, and explicit incomplete outcomes retain the [diagnostic method](performance-diagnostics.md). Timing is never a CI pass/fail threshold.

Measurements and exact source hashes are recorded with the PR once qualification completes. This first change still performs history-dependent validation. It does not resolve the separately measured polling/gate handoff problem or establish bounded latency. The next candidates remain native metadata/mapping work and polling progress, informed by the updated measurements.
