# Resolving the oldest pending delivery directly

Milestone 4 step **4.19**, framework **0.4.21**, continues [receive validation reuse](performance-receive-validation.md). Receive now resolves the Queue's recorded oldest-pending delivery directly instead of scanning Publication metadata again to find it. The complete topology and delivery-history checks still run. Public interfaces, stored formats, protocol versions, message headers and payload lifetimes are unchanged. Integration into `main` remains held for measured improvements and qualification.

## The repeated work

A **delivery link** is the file offset of a destination row inside a committed Publication. A Queue's live state records the link of its oldest nonterminal delivery. [Full delivery validation](native-validation.md) already proves that this root agrees with the complete retained history and receipt progress.

Previously issuance ignored this location until after another search: it decoded Publications from the beginning, compared Queue/sequence identities, and finally checked that the discovered link matched the root. At H=128, the first serial receive decoded 129 Publications during complete validation, then decoded those same 129 again to locate its pending sequence. H counts messages already sent and acknowledged before the measurement window; it does not count messages still pending.

The shared private [delivery-history helper](../src/delivery_history.h) now resolves an exact link for the current native direct-send profile. Direct Publications have one destination immediately following their 64-byte envelope and 128-byte fixed prefix. Subtraction is checked before deriving the Publication offset. The ordinary metadata reader then requires an exact committed Publication boundary and kind, validates canonical bounded metadata without copying payload bytes, and rejects unsupported Publishing acceptance. The helper also checks direct/single-destination shape and exact link, Queue and sequence equality. Issuance retains its fresh Pending receipt check before journal execution.

The existing scanning helper remains for acknowledgment's next-sequence lookup, where the Queue does not yet contain the successor link. Existing pins still return before fresh selection and lookup. Allowed loss still decides whether to commit a gap before issuance; surviving pending deliveries use the same direct resolver.

## Risk assessment

The [performance/correctness guidance](../AGENTS.md#performance-and-correctness) favors removing this duplicate search: the exact pointer has already been validated under the operation's continuously held mutation gate, and the selected record is checked again. No full-history validation or corruption check is removed. Queue, Source, claim and receipt reads remain fresh. There is no cache across operations or mutations and no new trusted-snapshot API.

The resolver deliberately supports the current direct, single-destination native profile. Multi-destination Publishing support must provide row-aware resolution rather than silently reuse first-row arithmetic. Explicit shape/identity checks and the current capability fence reject unsupported records. Neither a wraparound ring entry nor a Queue sequence alone substitutes for the committed Publication boundary. Invalid offsets, wrong kinds, mismatched identities and malformed metadata fail before progress writes.

Unsupported raw writers that bypass the mutation gate and arbitrary memory corruption are outside the existing protocol. This change adds no new tolerance or continuous-detection promise for them. Normal recovered operations still validate all history. Recovery, acknowledgment/release, exact cleanup, deadlines, Source close fences and borrowed view lifetimes keep their existing behavior.

## Qualification and measurement

Private tests compare the exact resolver with the retained-history search at early, middle and late positions, including overwritten ring entries. A read observer verifies bounded reads of only the selected Publication's metadata and no payload copying; the scanning control demonstrates the extra reads it replaces. Invalid links and Queue/sequence mismatches reject without changing the file. Existing public receive, allowed-loss/gap, pin repeat, ordering, close, cleanup, recovery and full-log tests remain applicable.

The unchanged diagnostic workload compares preserved 0.4.20 executables against normal and enabled 0.4.21 builds. Normal builds supply latency comparisons; enabled controls explain allocation and native work. Fresh Sessions, geometry/history/capacity/payload/Field controls, independent process/thread workloads, all outcomes and source/executable hashes are retained. Adjacent alternating serial pairs complement sequential full matrices. Observed ranges are not confidence intervals or production-tail guarantees; CI records correctness and diagnostics without timing thresholds.

Measurements and plots are recorded in the [result directory](../benchmarks/results/milestone-4-delivery-lookup/README.md). One full prefix scan and delivery validation, decoding/allocation, acknowledgment's successor search, native resource checks and idle polling remain costs. This focused optimization does not make receive independent of retained history.
