# Native transport failure and exhaustion qualification

Milestone 4, step 4.9, framework **0.4.11**, combines the failure cases introduced alongside native send, receive, and allowed loss. The public contract remains [two-plane mapped transport](native-transport.md). A **qualification test** checks that an implementation meets a stated guarantee under a controlled failure or boundary condition.

The tests establish real Sessions and message history through public APIs. Private hooks select exact journal stores, inspect persisted state, or place an otherwise unreachable counter near its maximum. These hooks are test infrastructure. The native multi-process primer application follows in step 4.10; these workers are not application examples to copy.

## Evidence by guarantee

| Condition | Evidence | Expected behavior |
| --- | --- | --- |
| Producer dies before or after acceptance while a consumer holds a message | [Combined process tests](../test/test_transport_recovery.cpp) and [worker](../test/native_transport_worker.cpp) | Prepared publication rolls back; committed publication is repaired once. An existing consumer pin keeps the same message, token, header, and payload address across ring wrap and producer replacement. |
| Consumer dies during acknowledgment; producer is the next participant | [Combined process tests](../test/test_transport_recovery.cpp) | The producer's operation first completes the committed acknowledgment, then admits its own publication. Completed work stays terminal and later delivery follows Queue order. |
| Recovery itself dies before finishing | [Combined process tests](../test/test_transport_recovery.cpp), [receipt/claim crash cuts](../test/test_delivery_progress_recovery.cpp), and [loss crash cuts](../test/test_loss_recovery.cpp) | Reapplying absolute changes preserves the captured identity and timestamp, without another acceptance, acknowledgment, or gap. |
| Wrapped ring and replacement under either delivery policy | [Combined process tests](../test/test_transport_recovery.cpp) and [allowed-loss tests](../test/test_allowed_loss.cpp) | Recovery delivery preserves the entire pending backlog. Allowed loss reports an eligible stale Pending prefix while an Issued pin stays protected until it ends. Old tokens cannot finish a replacement's issuance. |
| Exactly full log with delivery and claim progress | [Combined process tests](../test/test_transport_recovery.cpp), [loss progress](../test/test_allowed_loss.cpp), and [claim cleanup](../test/test_delivery_claim_cleanup.cpp) | Reserved progress and replacement require no append space. Retained acceptance remains queryable; fresh publication returns `LogFull`. Neither acknowledgment nor loss reclaims history. |
| Final publication identity, Source issuance, and operation generation | [Counter and rejection tests](../test/test_transport_exhaustion.cpp) | The final representable increment succeeds. Counters never wrap; subsequent operations requiring another value reject without changing state. The distinctions between these counters are described below. |
| Exhausted Task attachment counters and Registry mutation limits | [Session counters](../test/test_runtime_session.cpp) and [Task registration](../test/test_task_registration.cpp) | Incarnation/restart overflow rejects without replacing the claim or changing bytes. Exact registration remains usable when a fresh Registry mutation has no capacity or operation generation. |
| OS refuses physical file growth | [Native I/O tests](../test/test_transport_io.cpp) and [worker](../test/native_transport_io_worker.cpp) | Proven precommit rollback leaves no new accepted delivery and preserves earlier acceptance and the live consumer borrow. Restoring the OS limit permits retry with the original identity. |
| Invalid request, malformed reference, or unsupported profile | [Public admission](../test/test_publication_api.cpp), [loss redo](../test/test_loss_redo.cpp), and [capability tests](../test/test_delivery_compatibility.cpp) | Rejection preserves the prior accepted state. Exact record boundaries, kinds, owners, schemas, and required capabilities remain mandatory. |

Earlier tests remain part of the evidence. The [journal suite](../test/test_mutation_journal.cpp) covers framing, append rollback, mixed roots, and interrupted recovery. [Publication fault tests](../test/test_publication_runtime.cpp) cover each Queue/ring write; [progress crash cuts](../test/test_delivery_progress_recovery.cpp) cover each receipt/Queue/Source/claim write. [Deadline](../test/test_source_deadlines.cpp) and [lifecycle](../test/test_source_lifecycle.cpp) suites qualify finite waits, unreturned issuance, and deferred cleanup. The [test reading guide](../test/README.md) distinguishes public examples from private fixtures.

## Native process boundaries

The combined tests execute producer, consumer, and recovery workers as independent programs. A pipe handshake identifies the exact completed operation or selected fault checkpoint before the parent sends `SIGKILL`. No arbitrary sleep stands in for readiness. Reads, process waits, and worker lifetimes have finite test deadlines.

The [shared process owner](../test/native_process.h) owns each worker and its coordination pipes. [Process support](../test/process_support.h) handles bounded reports, continuation, termination, and `SIGPIPE` safely. These native report words are local test coordination; Angaria still carries messages through its mapped Session file.

An application normally cannot pause journal replay at a particular store. The worker's private hook makes that point observable, while setup, delivery, acceptance lookup, and final replacement use the implemented APIs. These tests qualify process death on a running coherent local filesystem. They do not establish power-loss durability, network-filesystem support, starvation freedom, or a hard execution-time bound.

## Counters have different lifetimes

- A publication sequence belongs to one producer Task incarnation. `next_publication_id()` reserves it separately from acceptance. A new incarnation starts its own sequence, while retained older identities remain queryable.
- A Source issuance identifies a particular borrow within a consumer incarnation. Repeated receive returns the existing pin even when no fresh issuance number remains. Acknowledgment/release can finish it if the operation-generation counter still has room. Consumer replacement creates a new issuance context and fences old tokens.
- The operation generation is Session-wide and advances for every committed mutation, including reserved progress. Exhausting it blocks a fresh mutation even when no append bytes are required. Status lookup and an unchanged repeat of a recorded publication do not require a fresh generation after recovery.
- Queue sequences describe complete accepted Queue history. A runtime fixture cannot honestly set a Queue root to `UINT64_MAX` while retaining only a few publications. [Ring/receipt codec tests](../test/test_delivery_state.cpp) and [publication codecs](../test/test_publication_metadata.cpp) cover full-width representation; runtime qualification preserves contiguous real history rather than claiming an end-to-end run through every Queue sequence.

Counter fixtures place selected private words at a valid boundary and then exercise the final transition through public calls. This is distinct from an application-supported counter-reset operation. A configuration count limit, append-byte limit, OS file limit, and counter exhaustion are separate failure conditions.

## Real I/O failure and synthetic checkpoints

The native I/O worker temporarily lowers its own `RLIMIT_FSIZE`, the operating system's per-process file-size limit, to the current Session file length. It temporarily ignores `SIGXFSZ`, the signal for exceeding that limit, within the child so attempted growth returns the OS error instead of terminating the worker. The Session's configured append budget still has room. This exercises an actual failing file-growth call rather than constructing an invalid Session or manufacturing `LogFull`.

The worker restores its saved OS limit and signal disposition before retrying. The parent's limits and signal handling remain independent. This qualification covers refused growth and its precommit rollback; it does not simulate every disk, mapping, torn-write, or power-loss failure. Synthetic `JournalFaultInjector` checkpoints separately cover failures after preparation and after commit, including `OutcomeUnknown` when repair cannot establish a final outcome immediately.

## What unchanged rejection means

Where a request is rejected before acceptance, tests compare the Session bytes or exact native snapshot from before that attempt. Identity issuance is a separate operation and occurs before taking that baseline. Recovering an older interrupted operation may legitimately change state before a new request is rejected; those changes belong to the older operation.

An accepted publication identity remains accepted after consumer acknowledgment or loss. An identical repeat does not create another Queue sequence. A changed repeat is a conflict; a fresh request at a full log is `LogFull`. A gap is a committed terminal change, not a timeout or publication rejection. An unreturned committed gap is not emitted again after replacement.

## Run the qualification

Build and run all tests using the [README instructions](../README.md#build-and-test). To focus on the compositions added in this step:

```sh
ctest --test-dir build -R 'Transport|PinnedProducerCrashTest' --output-on-failure
```

The full suite remains required because these compositions reuse the same loaders, journal, claims, mappings, and receipt validation as ordinary Registry and primer operations. Automatic CI builds and tests Linux x86_64, Linux ARM64, and macOS ARM64. Intel Mac qualification remains [request-only](native-storage.md#request-macos-x86_64-qualification). Each PR records results for its exact commit; step 4.11 adds performance measurements and the final milestone audit.
