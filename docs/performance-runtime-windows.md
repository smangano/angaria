# Reusing bounded runtime metadata mappings

Milestone 4 step **4.16**, framework **0.4.18**, continues the [performance investigation](performance-diagnostics-findings.md) after [local gate handoff](performance-gate-handoff.md). It reuses small mappings while one public operation reads Session metadata. The public API, mapped format, synchronization protocol, validation rules, and payload ownership retain their existing contracts. Integration into `main` remains held for measured improvements and qualification.

## What changes

A **RuntimeAccess** is the private recovered view held under the mutation gate for one operation. It obtains an owning index of validated record envelopes, reads immutable metadata into owning byte vectors, and reads mutable words through the native atomic adapter. Previously each metadata request made another native read or short-lived mapping, repeating file-size and process checks. At H=128, the preceding counted first send still made 324 mappings, 791 native reads, and 1,409 stat attempts.

RuntimeAccess now keeps two independent **64 KiB mapping windows**: one for atomic words and one for immutable copies. A window retains only the last region used by that reader. Separating the readers avoids replacing a history mapping every time an operation inspects mutable state. Large requests proceed through successive windows. No returned vector borrows a window, and mutable values are loaded afresh with `AtomicWord::load_acquire`; retaining a mapping does not retain a previous word value.

The journal's existing scan-local reader shares the focused private window helper. Its range checks, envelope assembly, atomic readiness, and full-prefix validation remain in the journal. There is no mapping or decoded-state cache between operations. Payload views retain their own mappings and pins.

## Risk assessment

The [project guidance](../AGENTS.md#performance-and-correctness) gives performance and correctness equal priority. The supported operation holds the mutation gate throughout recovery, metadata reads, and mutation. Repeating a native call for every metadata slice does not protect against a supported intervening Session writer.

- Operation entry and each new mapping retain their ordinary ownership/resource checks. Each RuntimeAccess read also checks its guard's process/thread owner before reusing a window. A reused window avoids repeated native size checks and mapping construction. The private reader requires its owning operation's valid held guard; it is not a general unlocked file reader.
- Window ends are bounded by the operation's initial recovered file size/committed tail. A retained window also retains its native shrink lease until replacement or operation end. A subsequent append can roll back to that original end while these windows remain live. They cannot cover a provisional append, and their owners are destroyed before the operation releases its guard. Existing payload leases remain independent.
- Each reader retains at most 64 KiB of logical file bytes; journal and payload mappings are separate. A larger mapping can increase cumulative mapped-byte counters even while reducing native call counts; mapped bytes are not resident memory. A refill clears its borrowed span before releasing the mapping. Bounds checks precede requests, including empty ones. Only requested immutable slices are copied, and live synchronization words remain atomic even when a mapping virtually covers adjacent bytes or payload pages.
- Metadata reads now use mapped I/O more widely. Unsupported external truncation after mapping, or an OS mapped-page I/O fault, can fault the process rather than return a `pread` error. Existing readiness/payload mappings already use that native failure model. Repeated `fstat` cannot make arbitrary concurrent truncation safe. This is an explicit tradeoff, with no hardware-failure containment promise.

The general NativeFile adapter retains fresh-size checks, independent limits, alias/shrink leases, and fork rejection. All validation still runs; only its access method changes. Cross-process commits, recovery, and corruption are inspected by a fresh operation. History-dependent validation and idle-polling CPU pressure remain separate costs.

## Qualification and measurements

Private tests qualify copies and word ranges crossing window boundaries, movement of populated readers with their guard, fresh atomic values on a cache hit after a mapped update, bounds/empty reads, and actual append failure followed by rollback and retry with both runtime readers live. Existing metadata-only observation, journal boundary/large-manifest/corruption tests, public ownership/fork checks, recovery, and payload lifetime tests remain relevant. These are storage qualification fixtures, not application setup examples.

The unchanged diagnostic workload compares preserved 0.4.17 executables with fresh normal and enabled builds. Exact input/executable hashes, compiler settings, geometry, complete or budget-stop outcomes, and raw samples accompany the measurements. Normal builds supply performance comparisons; enabled counts and timings explain work. Adjacent alternating serial pairs check ordinary-call cost separately from sequential full matrices. CI records correctness and observations without timing thresholds. Small host studies do not qualify production tail latency.

Measured results and exact provenance are recorded with the PR after qualification.
