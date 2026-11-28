# Public recorded runtime

Framework **0.4.39**, milestone step **4.37**, implements step 8 of the [independent messaging plan](recorded-unacknowledged-delivery-plan.md#implementation-steps-for-review). Applications now use `Session`, `Task`, `QueueWriter`, `SourceQueueSet`, `Message`, `MessageView`, and `Delivery` over producer-owned recording. Normal ID issuance, send, acceptance lookup, receive, acknowledgment, and release acquire no Session administrative gate.

## Setup and admission

An **admitted connection** is an Active Queue/Source whose complete immutable catalog and native resources have passed the current runtime's checks. Finish registering Tasks, Fields, exact schemas, Queues, and Source memberships, then activate the topology before opening messaging handles or issuing the first publication ID. Metadata activation describes the configured topology; operational admission also checks its executable policy and schema requirements.

The first valid endpoint opening or ID issuance seals the Registry. A **catalog seal** records the final definition revision in the Session; later registration, adoption, assignment, or activation changes return `InvalidState`. Prepare all outgoing Queues and all schema versions together. Invalid/unknown/wrong-owner openings and unsealed status/archive queries do not seal setup or create message resources. File-based creation completes registration and activation before returning; the same admission boundary applies afterward.

Bootstrap occurs under the administrative gate. It reserves the producer logs and connection/progress files before committing the seal through the administrative journal. A failed unpublished bootstrap can be rebuilt; after the seal commits, reopening never deletes or recreates these resources. Independent processes obtain the same catalog from validated native definitions, including exact schema definition references. Applications cannot supply their own snapshot as transport authority.

## Implemented profile

| Facility | Current behavior |
| --- | --- |
| Producer ownership | One Task-owned log and ID issuer shared by every outgoing Queue writer, including duplicate handles. |
| Schemas | All registered supported exact Binary schemas cached at opening; selection uses `MessageHeader::_schema`. |
| Tasks | Process Tasks and Thread Tasks with their live parent Process claims. Handles retain the required claims. |
| Source inputs | One Active input Queue per operational Source; multiple-input selection remains milestone 5. |
| Delivery | `recover_from_log`; immutable accepted messages remain replayable. |
| Processing | `required` acknowledgment by default; explicit `none` returns a `MessageView` without a processing token. |
| Storage/wait | Two-plane native storage and Spin waiting. |
| Later policies | Publishing execution, partial delivery, allowed loss, alternative selection/waits, segmentation, and reclamation remain unsupported by this admitted profile. Typed metadata can still describe later Queue Set features. |

A Task can have many outgoing Queues and send multiple registered schema types or versions. There is no public one-Queue/one-schema restriction. Source input selection and Publishing execution retain their roadmap boundaries; this admission does not pretend they are implemented.

## Sending and acceptance

Call `next_publication_id()`, then `writer.send(id, view)`. The owning overload returns `PublicationOutcome`; the buffered overload writes into caller-owned `DestinationOutcome` storage and avoids result-vector allocation. Both report acceptance, separately from processing completion.

`Accepted` includes an exact destination `_queueSequence`. `QueueFull` is a known rejection in that destination's `_reason`: no message was appended and `_queueSequence` is absent. Retry after receiving returns credit. `LogFull` separately reports exhausted producer recording bytes or publication capacity. Looking up an accepted ID returns its original destination and sequence across producer replacement. An accepted ID with changed meaningful input is a conflict.

IDs accepted by one Task advance in increasing order across its Queues. Accepting a later ID retires earlier still-unaccepted IDs. Multiple threads sharing a Process Task must coordinate an ID/send/retry sequence when they require that ordering; individual operations are internally serialized. Separate Thread Tasks provide independent issuers/logs. An accepted retry continues to resolve its original result.

The Session's `max_log_bytes` bounds administrative Registry/journal storage. `producer_log_bytes` and `producer_log_publications` bound each producing Task's retained messages. Returning credit or acknowledging processing frees neither log bytes nor accepted-record capacity. Tasks without outgoing Queues use a minimal ID-only log rather than reserving an unused full message log.

## Receiving and processing progress

An acknowledged Source returns `Delivery`, with `_message` and an exact `_token`. Repeated receive keeps its pin; `acknowledge(token)` commits processing completion, while `release(token)` permits redelivery with a fresh token. Close releases an outstanding pin. Old tokens fail after release, successful close/reopen, or Task replacement. A failed close keeps the live handle available for retry; recovery of selected progress remains consumer-owned.

An acknowledgment-free Source returns `MessageView`. The next receive ends the preceding borrow, including an empty attempt. Replacement/reopening can deliver an unacknowledged message again. Both receive forms expose the same `MessageHeader`; `copy_message()` owns its bytes and preserves the complete observed header and exact decoding context after handles close.

Transport credit is returned at receipt, independently of processing acknowledgment. A producer can query `QueueWriter::acknowledged_sequence()` and compare its **committed lower bound** with an accepted destination sequence. It can lag after an interrupted consumer until that consumer recovers; it never certifies an uncommitted acknowledgment. The query is unsupported for acknowledgment-free Sources. It is not an automatic producer-blocking round trip per send.

A positive receive timeout spins until a message, error, or its deadline. An empty attempt returns `ReceiveTimeout`; a shared producer stream can require several bounded scans to pass records addressed to other Queues. Negative/unrepresentable timeouts reject explicitly. Event notification waits remain a later milestone.

## Lifecycle and the critical path

Attachment and binding validate native resources, ownership, parent claims, versions, and the sealed catalog. Established operations check their retained local PID/Thread ownership before taking local mutexes; the producer data owner and consumer progress owner retain native lifetime claims. The Task's producer mutex serializes its own issuer/writers, and each Source has its own operation mutex. They do not serialize other Tasks or disjoint connections.

The fixed-resource contract is deliberate: live resource unlink/replacement and online catalog mutation are unsupported. Repeating pathname/inode checks or Session recovery on every message would reintroduce administrative work into communication. Validating at binding and retaining the actual owners avoids that cost without weakening supported Task exclusivity. Inherited handles reject before inherited local locks are entered; a forked child must exec before attaching a fresh Session. A Thread Task retains its parent, and attachment of a replacement checks the real native claims. Stale recorded Task flags after process death are diagnostics, not proof of liveness; public status checks native ownership and parent incarnation.

Process interruption on a running coherent host is the recovery scope. This runtime makes no synchronous disk-flush or power-loss durability promise. Preallocated files still incur mapped-page first-touch faults and background writeback; absence of explicit per-message file calls does not eliminate those costs.

## Replay, export, and inspection

Include `angaria/log.h`. `LogArchive::capture_live(session, limits)` acquires a committed cut for each producer. `publications(filter)` returns a `LogCursor`; `read_next()` returns an owning `LogRecord`, and `rewind()` repeats that captured input. Later appends are outside the cut. A Queue filter preserves its accepted ordering. Producers are enumerated deterministically without inventing a global publication or consumer execution order.

The archive retains exact schema definitions; copied records and their decoding context outlive the cursor and Task handles. `catalog()` supplies the captured definitions. `inspect(stream)` renders accepted-destination context and exact schema-aware Field names/types/values through MessageInspector. ArchiveLimits `_maxTextBytes` defaults to a 16 MiB aggregate output budget including summary/complete record units; capacity rejection leaves a complete prefix, while I/O can leave a partial final unit. Inspector catalog retention uses its separate default 16 MiB bound; `export_capture(stream)` writes the existing flat `MessageCaptureWriter` format. That flat stream needs the matching retained catalog for later schema-aware inspection. It is not a self-contained catalog/progress bundle. Receipt snapshots, portable catalog bundles, and optional exact execution traces remain in the [full interface direction](public-interface.md).

[Primer lesson 18](../examples/primer/18_log_replay.cpp) shows public replay and export after a messaging lesson. `angaria_inspect SESSION_DIRECTORY CAPTURE_FILE` inspects the exported records through the matching current Registry. Ordinary Session attachment may perform administrative recovery; it is not a physically read-only or foreign-ABI opener.

## Verification and measurement

Public teaching tests qualify both modes, shared Task logs, exact schema versions, duplicate writers, Queue credit/processing distinctions, Thread/parent ownership, close/stale tokens, invalid-operation side effects, actual held-gate communication, and fork+exec/SIGKILL replacement. The existing producer-log, descriptor, progress, crash, replay, and connection-independence suites remain current core qualification. Old Session-message-journal/receipt tests and their runtime have been retired together.

The focused public performance test measures ID/send/status/receive/selected-ACK cycles and guards buffered operations against allocation, Session gate acquisition, metadata reconstruction, mapping, native reads/stats, and file growth. Ordinary CI reports its timings without a wall-clock pass threshold. The [public performance comparison](public-runtime-performance.md) uses actual public Session/Task/endpoints in independent processes against nanomsg. Its latency/throughput figures are separate from same-process unit costs and from the prior private-binding baseline.

Native public Sessions use **format 3.0**, required-feature mask **0x7**, and synchronization revision **2**. Use matching programs/libraries and fresh Sessions. Earlier formats, the former Session-gated engine, and the v0 integer demo are not alternate public runtimes. Historical experiments and measurements are reproduced from their pinned revisions.
