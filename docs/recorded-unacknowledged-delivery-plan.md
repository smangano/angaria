# Recorded unacknowledged delivery plan

**Status: proposed design and qualification plan. No new delivery mode, runtime API, mapped layout or benchmark implementation is introduced by this document.** The first implementation step is the [comparison benchmark](nanomsg-comparison-plan.md), not a fast runtime path. No performance improvement is claimed.

The proposed profile combines a replayable message log with delivery that requires no application acknowledgment. It targets local interprocess communication through fixed one-to-one connections, explicit fan-out to multiple consumers, and Source Queue Set fan-in from multiple producers. A fan-out publication goes to every accepted destination; it does not choose one worker from a pool.

## Existing behavior and proposed boundary

The [current transport contract](native-transport.md) implements direct Queue send, retained outcome lookup, and one-Active-Queue Source receive with acknowledgment, release and configured loss. Multiple-Queue Source selection, Publishing Queue Set delivery and notification waiting remain later roadmap work. Public declarations are not evidence that these paths are implemented.

Current operations use the [Session mutation gate and journal](concurrency.md). A runtime access recovers and validates committed history; publication ID issuance is a separate FixedState transaction. Send appends immutable Publication data, destination receipts and a manifest, then applies recorded state updates. Receive establishes an issued receipt and pin; acknowledgment records terminal progress. Existing `release()` returns a delivery to Pending for redelivery. It is not an unacknowledged consume operation.

The proposed profile retains the concepts of producer ownership, Publishing Queue Set destinations and Source Queue Set inputs. It needs a separate protocol and delivery contract. It must not silently change acknowledged delivery, reinterpret `DeliveryPolicy::AllowLoss`, or expose current mutable ring structures without their required gate. Allowed loss still records delivery progress in the existing protocol.

## Required contract

| Dimension | Proposed requirement |
| --- | --- |
| Acceptance | Every accepted message is committed to its replay log before it becomes visible to any consumer or send reports acceptance. |
| Replay evidence | The committed record identifies the producer and incarnation, sequence, exact schema/header/payload, destination snapshot and per-destination acceptance outcome. |
| Fan-out | `AllDestinations` is the default. An explicitly selected `Partial` policy records exactly which destinations accepted and which did not. |
| Destination availability | Freeze whether an offline destination is eligible for acceptance and how connection liveness is established. Available ring credit alone does not establish a live consumer. |
| Application progress | Receive requires no application acknowledgment and records no per-message acknowledgment receipt. Delivery does not certify successful application processing. |
| Ordering | Preserve per-connection order and record a defined Session-wide commit order. Source selection order is a separate concern. |
| Failure scope | Process termination on a running system with coherent local mappings, as in the current native contract. No power-loss durability, synchronous disk-flush or network-filesystem guarantee is implied. |
| Retention | Keep all accepted records for the retained Session/log lifetime. Exhausting the configured bound returns `LogFull`; consumer progress alone cannot authorize deletion. |

An interrupted call may have committed without returning its result. It must not turn a committed acceptance into a known rejection. The eventual API must define how callers discover that outcome and whether a retry is deduplicated. Assigning a publication sequence within the append can remove a separate issuance operation, but does not by itself supply a retry identity after an uncertain result. Existing explicit-ID APIs retain their current contract.

Preserve the common `MessageHeader`, `MessageView` and owning `Message` model. An owning copy must preserve the exact logical header, payload bytes and decoding dependencies beyond the endpoint/borrow lifetime. A mode-specific receive handle carries no application-acknowledgment obligation; any storage or mapping lease it owns is a separate resource contract. Do not reuse `DeliveryToken` in a way that implies the existing acknowledgment obligation or silently changes existing `Delivery` behavior.

## Log authority and connection rings

A proposed connection has one producer and one consumer, with a bounded single-producer/single-consumer (SPSC) ring. Concurrent application threads must not share an endpoint in a way that violates that ownership. Mapping setup and native atomic bindings occur when the connection opens, rather than for every message. The protocol must qualify alignment, release/acquire ordering and supported ABI profiles; it cannot assume ordinary mapped-byte accesses are atomic.

The payload is written once into immutable logged storage. Ring entries carry checked references to committed payloads, with connection identity, sequence and generation sufficient to reject stale or wrong-log references. Each consumer borrows the same immutable payload through its own mapping lease. Copying a reference into several connection rings avoids a payload copy per destination without requiring each consumer to own or free the payload. Reusable payload storage would require a separate reclamation protocol.

The publication sequence is assigned within the append operation. The complete allocation and destination outcome must be established before commit. The normal sequence is:

1. Validate producer/connection ownership, message bounds, payload rules, destination membership and result capacity using checked connection metadata.
2. Check log space and all required connection credits before accepting. Finish any operation that can produce a known rejection before changing published heads.
3. Write the complete immutable record, including destination outcomes and the sequence/order metadata, into reserved log space.
4. Publish the log commit with the required native ordering. This is the acceptance authority.
5. Expose references in the accepted destinations' rings and return the recorded outcome.

Record materialization or a ready-looking ring slot is not sufficient evidence of acceptance. Consumers must rely on the committed record and their recorded destination membership. Interrupted append recovery must distinguish committed records from incomplete reservations before exposing or replaying either.

## Fan-out, capacity and crashes

For fixed connections with one producer owning all outbound rings, an `AllDestinations` capacity check can establish that every destination has credit before the append. Consumers only return credit, and another producer cannot consume it. Under those assumptions, a normal `WouldBlock` result can leave every destination unchanged. Concurrent sends, connection replacement or another writer would invalidate that reasoning and require additional coordination.

This profile proposes flow-controlled rings: do not overwrite an unread entry, and never reclaim logged payload bytes while they are borrowed. That differs from the existing ring cache, whose overwritten entries can be recovered through retained history. A slow or disconnected destination can therefore block the default fan-out path even when other destinations are ready. A bounded wait must retain its deadline across retries; zero-time admission can return `WouldBlock`. Log exhaustion remains a distinct `LogFull` condition.

`Partial` must be explicit. Determine the eligible subset and record each destination's outcome in the committed publication. A destination excluded from that acceptance must not later appear to have accepted it. If every destination rejects, create no accepted publication/log record, preserving the existing zero-acceptance boundary. Recovery can still settle an earlier uncertain operation; this rule does not change uncertain-outcome handling. Retry rules, if offered, need their own recorded identity and outcome contract; they cannot be inferred from a volatile ring state.

Offline-destination admission is a separate policy decision. The existing acknowledged profile can accept while its consumer is offline. A new profile could retain that behavior or require an available connection, but it must define availability, disconnect detection and the resulting timeout/rejection behavior before implementation. A free ring entry is storage capacity, not proof that a peer can receive. The benchmark must report these policies explicitly rather than treating credit availability as equivalent to a socket having an available downstream connection.

Publishing multiple ring heads is not one atomic operation. If the producer dies after log commit, some consumers may already have observed references while others have not. The committed destination list can establish all-destination acceptance, but it does not ensure that all live notifications completed. Strict crash-atomic acceptance depends on that shared log commit predicate, not on the collection of head stores. A postcommit ring-publication delay or failure leaves accepted data not yet exposed; it cannot become a known rejection or `WouldBlock` result.

Before a runtime profile is admitted, select and qualify its restart policy. Rebuilding/catching up connection rings from committed records can restore discoverability, but volatile consumer cursors cannot prove which messages an application already processed. Replay can consequently duplicate delivery. Starting at the live tail instead is a different policy that may skip accepted messages in live delivery, even though they remain available for explicit replay. Neither choice may be presented as acknowledged or exactly-once processing.

## Source fan-in and local ownership

A Source selects among its configured incoming connections; a Publishing Queue Set sends to its configured outbound destinations. These are separate operations. Strict priority, fair selection and waiting policy need defined behavior and qualification rather than being borrowed from declarations of future native functionality. Preserve each input's order; a merge of several inputs does not automatically reproduce commit order.

Receiving a borrowed view can consume its ring entry while a **local borrow lease** keeps the payload mapping usable. Returning ring credit or ending this lease is resource management, not application acknowledgment. The API must define whether a view ends on the next receive invocation, explicit loan release, Source close or connection teardown, including a receive that times out. An owning copy is required when application work outlives that boundary. Copying a span does not extend the lease.

The immutable log remains the storage authority after connection credit returns. Log retirement or deletion must respect outstanding mapping leases, not merely ring cursors. A process-local mapping lease does not protect another process: cross-process reclamation would require a qualified shared lease, epoch or equivalent protocol. For the first profile, retain the append-only log and fail with `LogFull` at its bound. Replay retention includes the exact decoding definitions and destination metadata, not payload bytes alone. Future rotation or archival must preserve that history and define recovery and retention ownership before reclamation is allowed.

## Commit order and optional delivery trace

The current shared append/journal authority provides an order across producer operations. Retain that order in the initial experiment. Partitioning into producer-owned logs must not silently replace it with unrelated local sequence counters. If a later design records a Session-wide commit ordinal, its assignment must have a defined commit linearization point, including concurrent producers, abandoned reservations and crashes. A counter reserved before commit is not automatically commit order. A global sequencer also introduces coordination whose runtime cost must be measured.

The required log supports replay of accepted messages and their recorded destination outcomes. It does not, by itself, reproduce which Source input won a race, the order consumers processed messages, clock observations or external effects. An optional delivery trace could record selection/dequeue events when that stronger replay task is needed. Trace collection has separate allocation, ordering, retention and throughput costs. It must not be enabled silently in an ordinary messaging comparison, and message logging must remain mandatory when the optional trace is disabled.

## Validation and compatibility

The steady-state target is work independent of retained message-history length: no full-prefix scan, full topology reconstruction or receive-progress journal transaction for each message. Validate the complete retained state during attachment/recovery, then retain checked schema, topology, mapping and ownership metadata for the endpoint lifetime. Define how configuration revisions, changed claims and connection generations invalidate those handles. Cache use must not permit a replaced or ended owner to continue publishing.

This is not a claim of constant work for arbitrary messages or topologies. Payload copying/validation depends on payload size and schema; fan-out depends on destination count; Source polling depends on its inputs. Continue checking current message bounds, committed references and slot generations. Removing repeated history validation must not remove necessary admission checks or use unchecked pointers from shared storage.

The new profile requires explicit mapped-layout and synchronization admission. Allocate required capability identifiers only with a reviewed format/protocol proposal. Existing binaries must reject unsupported profiles before writes or recovery, and existing Sessions must not be promoted in place. Keep acknowledged delivery usable under its existing format and guarantees. Update public API, format documentation, inspector/export support and primer examples when an implementation actually introduces public behavior.

## Incremental work and evidence

| Stage | Scope and exit evidence |
| --- | --- |
| 1. Benchmark first | Implement the [comparison specification](nanomsg-comparison-plan.md) in a later focused step. Retain current acknowledged Angaria results and unacknowledged nanomsg IPC results under their actual, different contracts. Validate payloads, destination completeness and workload completion before interpreting timing. |
| 2. Current-log experiment | Keep the existing serialized append/commit authority. Publish only committed log references to experimental SPSC connections and use separate consumer cursors/local leases. Do not reinterpret existing Pending receipts or `release()`. Measure the remaining producer gate, ID issuance and full-prefix costs honestly; this stage does not meet the final history-independent target. |
| 3. Profile and failure qualification | Freeze acceptance, offline admission/liveness, uncertain-result lookup, fan-in selection, restart/catch-up, retention and lease rules. Qualify incomplete append, commit-before-exposure, partial head publication, stale generations, slow/full/offline destinations, reader lifetime and every selected restart policy with independent processes. Demonstrate that every accepted message remains replayable. |
| 4. Producer-owned log evaluation | Evaluate preallocated producer partitions and checked stable endpoint metadata. Preserve the required global commit order explicitly, qualify recovery/retention, and compare with the shared-log experiment before adopting the additional architecture. |
| 5. Public runtime admission | Review the new profile/capabilities and API, preserve the existing acknowledged path, and add implementation documentation, teaching tests, tools and examples. Only measured, qualified behavior becomes a public guarantee. |

Benchmark reporting must lead with end-to-end latency and completed-message throughput, include tails and CPU cost, and retain setup, payload size, fan-out degree, topology, wait policy and offered-load details. Include saturation, slow-reader and bounded-log behavior. Allocation/profile counts can explain a result but cannot substitute for latency/throughput evidence. The [current performance methodology](native-performance.md) remains the baseline for provenance and workload limits; a future comparison must not label semantically different paths as a like-for-like transport speedup.
