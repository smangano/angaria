# Reusing one operation's validated history

Milestone 4 step **4.17**, framework **0.4.19**, continues the [performance investigation](performance-diagnostics-findings.md) after [runtime mapping reuse](performance-runtime-windows.md). It removes a second full committed-prefix scan from the first execution attempt of a private gate-held runtime operation. General journal entry points and later attempts still validate freshly. Public interfaces, stored formats, native locking, and delivery/recovery rules retain their existing versions. Integration into `main` remains held for measured improvements and qualification.

## What changes

The **committed prefix** is the retained record history ending at the authoritative allocation tail. A private **RuntimeAccess** recovers the Session and validates that history when an operation begins. It holds the mutation gate until the operation ends, retaining owning record envelopes for exact-boundary lookup. Previously journal preflight scanned the same history again before preparing the first mutation, despite that continuously held gate. The preceding H=128 first send counted two scans and 638 record visits.

RuntimeAccess now offers its initial validated snapshot and record ranges to a friend-only journal path on its first `execute` attempt. The snapshot used for this decision is private and separate from the working snapshot that progress code can update. Eligibility is consumed before owner checks, plan checks, allocation, recovery or execution; an invalid plan or failed append does not restore it. Moving an access carries its owned guard, journal and record ranges; a moved-from journal rejects execution.

The journal still checks guard/controller ownership, stable native resources, Idle control, current canonical roots and physical file length. If recovery ran, or current roots/length differ, it uses the normal fresh-validation path before classifying a stale plan. A matching first attempt reuses the already validated framing. Plan identity, generation exhaustion, size budgets, new record contents, predecessor references, redo targets, actor/domain rules and accepted materialized bytes retain their checks. Error settlement and process-death recovery still validate freshly.

The general `MutationJournal::execute`, `snapshot` and `recover` methods retain their fresh paths. Later execution attempts on the same RuntimeAccess also use generic execution. No reusable token is accepted from a caller, and there is no cache shared between operations or gate acquisitions.

## Risk assessment

The [project guidance](../AGENTS.md#performance-and-correctness) gives performance and correctness equal priority. Under the supported protocol, other processes and threads cannot alter committed state while this operation continuously owns the mutation gate. The duplicate scan adds history-dependent work without a supported intervening writer. Execution already trusted preflight's owning ranges during materialization/apply; this change extends that trust interval back to the same operation's entry validation.

- This changes validation frequency on a narrowly trusted internal path. The first attempt relies on facts established at operation entry rather than re-reading every historical envelope before commit. A raw file write or same-root corruption injected between those points may therefore escape that second scan. Private callers must keep the same guard, journal, Session and file binding, and must not alter committed metadata directly. Qualification fixtures that deliberately change storage use generic fresh APIs or consume the one-use path before doing so.
- External writers bypassing coordination, arbitrary memory corruption, and hardware faults during an operation are not made safe by either design. A second scan cannot guarantee their absence. The accepted tradeoff is to avoid this repeated precaution during a short, gate-held operation while retaining complete validation at each new operation and on generic journal calls.
- Matching roots alone do not establish reusable validation. Only RuntimeAccess supplies the private snapshot and owning ranges created by its entry validation, under its still-held guard. The comparison includes operation generation and physical size; Registry revision or tail alone would miss FixedState progress. Any non-Idle recovery disables reuse even if rollback restores the original roots.
- Eligibility ends before the first attempt, including errors and exceptions. Later retries, multiple FixedState progress operations, changed state, and uncertain-outcome repair cannot reuse it. The implementation shares preflight and execution machinery rather than duplicating transaction or recovery logic.
- Reuse still copies owning ranges needed by journal execution; it is not an allocation-free path. No mapped span or payload view changes owner or lifetime. Existing atomic loads/fences, class preference at the gate, rollback limits, and corruption rejection on fresh entry remain in force.

Application code uses the same API. The guarantee is coordinated mutation and validated operation entry, not continuous detection of unsupported file tampering inside an operation. This trust boundary and the changed validation frequency are deliberate performance choices.

## Qualification and measurements

Private tests cover moved operation state, repeated execution and stale plans, first-attempt rejection consuming eligibility, unchanged-root corruption on fresh paths, and real append-growth failure followed by rollback and retry. Existing generic journal corruption, recovery, exhaustion, native ownership/fork, public delivery, and payload lifetime tests remain relevant. These are storage qualification fixtures, not application examples.

The unchanged diagnostic workload compares preserved 0.4.18 executables with fresh normal and enabled builds. Matched geometry, history/capacity/payload/Field controls, separate thread/process cases, complete or budget-stop outcomes, raw operation samples, and source/executable hashes are retained. Normal builds provide the timing comparisons; enabled counts/timings explain work. Adjacent alternating pairs check ordinary serial cost separately from sequential full matrices. No CI timing threshold or production-tail guarantee is introduced.

Measured results and exact provenance are recorded with the PR after qualification.
