# Milestone 4 completion audit

**Integration hold:** the step-4.11 baseline below exposed unacceptable performance. Step [4.12 diagnostics](../performance-diagnostics-findings.md), framework **0.4.14**, records the causes and next fix candidates. Integration into `main` requires reviewed measured improvements and final qualification. This document retains the original step-4.11 scope audit.

Step **4.11**, framework **0.4.13**, completes the measurement and audit work for review on `milestone-4-11`, targeting `milestone-4`. It does not integrate the milestone into `main`; that requires the separate milestone integration PR after this step is reviewed and merged. [The review plan](milestone-4.md) records the eleven development steps and intervening focused PRs.

## Implemented scope and evidence

| Step | Result | Evidence |
| --- | --- | --- |
| 4.1 | Defined two-plane transport, compatibility, timestamps, receipt/ring state, view lifetime, and cleanup. | [Transport contract](../native-transport.md), [mapped layouts](../mapped-format.md). |
| 4.2 | Checked local codecs for Publications, destinations, receipts, and full-width ring generations. | [Publication codec tests](../../test/test_publication_metadata.cpp), [delivery state tests](../../test/test_delivery_state.cpp), [framing tests](../../test/test_delivery_framing.cpp). |
| 4.3 | Capability admission before recovery, followed by validation of acceptance/history, exact schemas, topology, Queue chains, and receipt pins. | [Validation contract](../native-validation.md), [history tests](../../test/test_delivery_validation.cpp), [compatibility tests](../../test/test_delivery_compatibility.cpp). |
| 4.4 | Complete append preflight and immutable payload mappings safe across growth and rollback. | [Storage contract](../native-storage.md#committed-payload-storage-and-append-planning), [append planning](../../test/test_append_layout.cpp), [mapping](../../test/test_mapped_payload.cpp), [rollback](../../test/test_payload_rollback.cpp). |
| 4.5 | Reserved issuance, acknowledgment/release, and publication-aware claim cleanup with repeatable replay. | [Progress contract](../native-progress.md), [progress](../../test/test_delivery_progress.cpp), [cleanup](../../test/test_delivery_claim_cleanup.cpp), [recovery](../../test/test_delivery_progress_recovery.cpp). |
| 4.6 | Direct Queue send, issued-ID/repeat/conflict rules, retained status lookup, and shared public message/header model. | [Direct publication](../direct-publication.md), [public tests](../../test/test_publication_api.cpp), [message tests](../../test/test_message.cpp), [lesson 14](../../examples/primer/14_direct_send.cpp). |
| 4.7 | One-input Source receive, stable borrows, exact tokens, owning copies, deadlines, and explicit/deferred close. | [Receive guide](../native-receive.md), [public Source](../../test/test_source_runtime.cpp), [lifecycle](../../test/test_source_lifecycle.cpp), [deadlines](../../test/test_source_deadlines.cpp), [lesson 15](../../examples/primer/15_receive.cpp). |
| 4.8 | Configured allowed loss, protected Issued messages, compact replay, and visible gaps. | [Public loss tests](../../test/test_allowed_loss.cpp), [configuration](../../test/test_loss_configuration.cpp), [native recovery](../../test/test_loss_recovery.cpp), [lesson 16](../../examples/primer/16_allowed_loss.cpp). |
| 4.9 | Combined process death/recovery, full logs, counter boundaries, and actual file-growth refusal. | [Qualification matrix](../native-qualification.md), [recovery](../../test/test_transport_recovery.cpp), [exhaustion](../../test/test_transport_exhaustion.cpp), [I/O](../../test/test_transport_io.cpp). |
| 4.10 | Independent Feed → Strategy → Audit application with exact schemas, backlog, provenance, replacement, and acknowledgment/redelivery. | [Walkthrough](../native-pipeline.md), [lesson 17](../../examples/primer/17_native_pipeline.cpp), [coordinator](../../test/native_pipeline.py). |
| 4.11 | Reproducible public latency/throughput, physical append growth, and instrumented allocation/commit gate stages; full scope/primer audit. | [Measurement guide](../native-performance.md), [benchmark](../../benchmarks/native_transport.cpp), [runner](../../benchmarks/run_native.py), this audit. |

The [test reading guide](../../test/README.md) distinguishes ordinary public usage from private corruption, boundary fixtures, and process-death injection. Journal cuts and synthetic exhausted counters qualify failure behavior; they are not application setup APIs. Passing tests qualify those controlled cases, rather than arbitrary corruption, storage failure, or power loss.

## Public API boundary

Applications can create/attach Sessions, claim registered Process/Thread Tasks, register/configure and inspect the Registry, encode supported Binary Fields, write/read portable captures, send directly through QueueWriter, query acceptance, and receive through a Source with at most one Active Queue for fresh selection. Default delivery recovers retained history after ring overwrite. Configured AllowLoss reports skipped pending ranges while protecting already Issued messages. Message payloads remain immutable and acknowledgment is per destination.

`Message` owns payload bytes and decoding context. A received `MessageView` borrows payload bytes while retaining decoding metadata; `Delivery` combines that view with its exact token. Both message types expose the same MessageHeader. A copy preserves publication and optional delivery context, not just payload bytes. Portable encoding 1 remains an 80-byte publication header and rejects delivery-only optional fields it cannot represent. It is not an export format for a complete native delivery.

Task replacement changes incarnation. Identical sends under the current claim preserve accepted identity, and uncertain results must be resolved using the original ID. A replacement can inspect earlier publication IDs but cannot send using the earlier claim. An old delivery token cannot acknowledge a replacement issuance. Separate send and acknowledgment commits do not make application side effects exactly once.

An existing in-flight delivery can still be repeated or completed after membership grows. Fresh selection with multiple Active Queues rejects until milestone 5. Registry support for both Queue Set kinds does not imply operational Publishing fan-out: `PublishingQueueSet::publish` and `retry` are declarations without runtime definitions. Selection/ResetBudgets, richer diagnostics/capability-query objects, and other methods marked planned in the [public interface status table](../public-interface.md) remain unavailable.

## Compatibility review

Framework version **0.4.13** does not change a mapped format, required capability bit, portable encoding, or synchronization protocol.

| Native Session profile | Supported behavior at this milestone |
| --- | --- |
| 2.0 / `0x1` | Retained scalar Registry/codec operations; native send/receive rejects without applying delivery work. |
| 2.1 / `0x3` | Retained scalar/logical Registry/codec operations; native send/receive rejects without applying delivery work. |
| 2.2 / `0x7` | Core native direct delivery and recovery; allowed-loss configuration/registration rejects. |
| 2.2 / `0xf` | Current factory profile: core native delivery plus allowed loss. |
| Unsupported format/required capability | Reject before journal recovery changes the Session or coordination bytes. |

There is no in-place upgrade. Existing supported Sessions retain their profile. Native attachment and inspection retain normal recovery requirements, so attaching a supported Session can finish its pending journal; inspection is not a promise of zero writes. [Compatibility tests](../../test/test_delivery_compatibility.cpp) leave real Prepared/Committed work pending under an incompatible header and verify admission rejects before writes. [Public publication tests](../../test/test_publication_api.cpp) and [loss configuration tests](../../test/test_loss_configuration.cpp) cover retained legacy/core behavior.

Exact retained schema versions determine interpretation even when byte widths match. Ordinary capture encoding, native mapped format, configuration schema, and framework version remain separate compatibility domains.

## Progress and recovery review

The Session gate serializes mutation, using process-local coordination plus a crash-released native lock. A terminated owner releases ownership; the next owner repairs or aborts its journal before exposing recovered state. A paused live owner can stall others. Lock-free native atomic words do not make send, receive, acknowledgment, allocation, or cleanup lock-free.

Reserved acknowledgment, release, loss, and claim cleanup require **no appended storage** and can operate at LogFull. This does not mean zero heap allocation, zero mappings, no history scans, no I/O, or bounded elapsed time. Guard construction and metadata snapshots use process memory. These operations still need valid state, I/O, and counter headroom; an exhausted Session operation generation can reject a mutation that appends no bytes. Acknowledgment/loss do not reclaim physical history.

Borrowed payload reads need no mutation gate after issuance establishes their pin. Their mapping/address remains stable across growth and later uncommitted rollback. Acknowledgment, release, close, or replacement ends the delivery borrow; applications must stop reading it, including after uncertain completion, and copy before finishing if independent lifetime is needed. A preallocated deferred-close marker records cleanup obligations without itself allocating; processing that obligation still enters the ordinary gated runtime.

Zero-time receive and finite waits use nonblocking gate attempts and checked monotonic deadlines. They do not impose a hard bound on recovery or an already started operation, nor guarantee that a competing producer acquires the gate before a receive deadline. The concurrent-publication test accepts that legitimate timeout and verifies subsequent exact delivery instead of assuming fair scheduling within two seconds. The [concurrency contract](../concurrency.md#supported-failure-and-progress-scope) and [receive guide](../native-receive.md) state the exact scope. There is no starvation-free, wait-free, hard-real-time, or power-loss durability guarantee.

## Primer and documentation review

All seventeen lessons were reviewed against their compiled sources, split application roles, explicit TOML, commands, and CTest registrations. They use implemented public Angaria APIs, visible configuration, and explicit Result/Error checks. No private hooks or generic primer result/configuration wrappers are presented as application code. Topology-only lessons do not call future fan-out methods; capture lessons distinguish framing from native acceptance. All lesson targets build with `BUILD_TESTING=OFF`.

The [primer](../primer.md) now explains performance results, no-append progress, the portable/full-header distinction, and the implemented recovery/loss paths. README legacy sections explicitly identify v0's separate API and format while preserving existing anchors. Native transport/progress/storage/validation/layout and the public interface review distinguish implemented behavior from future protocol requirements. The roadmap and milestone plan now point here rather than leaving step 4.11 as future work.

## Integration gate and remaining roadmap

Before this step is ready to merge, build/test the exact PR head on automatic Linux x86_64, Linux ARM64, and macOS ARM64, including the measurement correctness smoke. The PR records exact commits, counts, and run links. macOS x86_64 remains request-only; it is not silently added to the automatic matrix. Timing values are recorded experiments, never CI pass/fail thresholds. Milestone integration repeats qualification on its own final commit.

| Milestone | Remaining application-facing work |
| --- | --- |
| 5 | Multiple-input Source selection: strict priority/round robin, proportional budgets, weighted random, and replacement. |
| 6 | Publishing fan-out, all-destination/partial acceptance and retry, complete five-process trading simulation. |
| 7 | Additional waiting/synchronization policies, including Linux eventfd. |
| 8 | Segmented storage, allocation policies, reclamation, and restart. |
| 9 | Advanced Fields/shared pools and serialization formats. |
| 10 | Generalized native export/inspection/replay, optional execution traces, configuration converters, release tooling and 1.0.0 qualification. |

The measured baseline informs later optimization without expanding this milestone's guarantees. The [roadmap](../../ROADMAP.md) remains the authority for subsequent scope.
