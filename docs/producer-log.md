# Producer-owned append, recovery and replay

Framework **0.4.34**, milestone step **4.32**, implements step 3 of the [independent messaging plan](recorded-unacknowledged-delivery-plan.md#implementation-steps-for-review). These are private storage primitives, not a new public Session or send/receive mode. The current public runtime and the seventeen [primer programs](primer.md) continue to use their existing protocol. Ring delivery, logical Queue credit, consumer progress, application acknowledgments and public admission follow in later steps.

Step **4.33**, framework **0.4.35**, adds private [recorded point-to-point endpoints](recorded-queue.md). It extends this storage API with cached `queue_sequence()` and explicitly live `poll_next(cursor)`. Live polling consumes one already validated record or validates at most one newly committed record after the cursor's cut is exhausted; independent cursor copies stay frozen. Ordinary `next()` and `capture()` retain their replay-cut behavior. Consumer progress with application acknowledgments and public admission remain later work.

A **producer stream** is one logical Task's append-only message log. Its **committed tail** is the atomic byte offset immediately after its last accepted record. The physical file is reserved in advance; its length says nothing about how many messages have been accepted. Publication records and the data header use the [format-3 codecs](producer-log-format.md).

## Ownership and opening

The checked catalog owner supplies the Session/Task identity, exact catalog revision, native ABI profile, byte/count limits and frozen outgoing Queue identities. `ProducerLogOptions` copies that bounded Queue list. These primitives check the supplied context but do not look up schemas, load TOML, construct topology or validate application payload semantics. Those are upper-layer responsibilities before a prepared append reaches this layer.

`ProducerLog::create` is offline bootstrap with existing parent directories. It exclusively creates a data file and an owner file and physically reserves their full capacity. Linux uses `posix_fallocate`; macOS uses `F_PREALLOCATE` followed by `ftruncate`. There is no sparse-file fallback. Reservation failure rejects setup. Bootstrap must finish before resources are published to other processes; callers discard an incomplete unpublished staging directory after failure. It is not a concurrent Session factory.

`open_writer` takes a nonblocking native lifetime claim on the **actual data inode**, using a separate registered descriptor from the mapping descriptors. A second writer fails with `OwnershipBusy`, including another open in the same process or an alternate owner-file argument. Multiply linked files reject earlier with `IoError`, preserving the existing native single-link rule. A paused owner keeps its claim. No timeout displaces it. Fork handling closes the inherited claim descriptor in the child without unlocking the parent's claim; inherited endpoint calls fail with `WrongOwner`. The owning handle and its normal operations are confined to the opening thread.

Opening validates headers and fixed reservations, maps once, reserves a bounded local index, and validates every record through an acquired committed tail. The owner file is bound to the data file's device/inode identity as well as its logical Session/Task. Renaming a resource does not change that identity; copying or replacing individual files does. External changes, unlink/replacement of live resources and portable relocation are outside this native profile. Session bootstrap must retain the correct resource pair and sealed catalog for the stream's lifetime. There is no per-message path check to defend against those unsupported changes.

The owner file is 4,096 bytes:

| Offset | Bytes | Meaning |
| ---: | ---: | --- |
| 0 | 8 | `ANGAOWNR` magic. |
| 8 / 10 | 2 each | Format major/minor, 3.0. |
| 12 | 4 | Owner file length, 4,096. |
| 16 / 24 | 8 each | Session high/low identity. |
| 32 | 2 | Producer Task identity. |
| 40 | 4 | Native ABI profile. |
| 44 / 46 | 2 each | Synchronization revision 2; atomic width 8. |
| 48 / 56 | 8 each | Data-file device/inode identity. |
| 256 | 8 | Native atomic incarnation counter. |

Other immutable header bytes are zero. The incarnation counter starts at zero and advances before an opened writer can expose an ID. It advances even if the previous owner accepted nothing: an issued but unaccepted ID must never be reissued after restart. Exhaustion rejects opening rather than wrapping.

This refines the design's proposed two-image Task-control structure. This step persists exactly **one authoritative word**, the incarnation, so one qualified atomic control publication suffices. There is no multiword after-state to journal. Future parent/Task lifecycle fields must preserve their own consistency when integrated; this private primitive does not claim to implement them. The existing native atomic qualification and control-publication fence are reused.

## Append and accepted-ID retry

1. `next_publication_id()` issues an owner-local, nonwrapping sequence without writing the log or owner file. Gaps are permitted. New acceptances must use current-incarnation issued IDs in increasing order.
2. `append` resolves an already accepted ID first. Identical meaningful input returns its original reference, record ordinal and Queue sequence without another write, including when the log is full. Changed schema/format/flags, payload, Queue, schema reference or topology revision produces `Conflict`. A new attempt's sampled publication time does not replace an accepted record's time.
3. A new direct append checks identity, Queue membership, payload/header bounds and reserved count/byte capacity. It writes one complete immutable record with one Accepted destination. Record ordinals and Queue sequences continue across owner replacement. Input delivery timestamps, Queue context and prior publication identity do not become the new publication header; this layer constructs that header from the prepared message's schema, format, flags and payload plus the new ID/time.
4. A release-store of the record's end to the committed tail accepts it. All fallible preparation happens first. Reserved trivial index entries and Queue counters are installed without allocation afterward. The record remains immutable and the tail never decreases.

Normal issuance/append does no Session access, shared gate acquisition, filesystem read/stat/growth, mapping creation or heap allocation. It still performs bounded validation and copies the payload. Queue lookup depends on the bounded outgoing Queue list; ordinary new append does not scan retained records. Accepted-ID lookup uses a bounded ordered index and costs logarithmic work in its record count. File pages can still fault; this is not a latency or power-loss-durability guarantee.

Count or byte exhaustion returns `LogFull` before acceptance. A stopped/failed attempt before the tail store leaves an unaccepted suffix which a successor may overwrite. Neither recovery nor rejection truncates the file. A failure after acceptance but before returning a result is `OutcomeUnknown`; actor-owned `lookup(id)` or successor recovery resolves the accepted ID. An absent ID is settled only under this exclusive ownership, not by inspecting another live writer's possible in-flight request.

Recovery scans the committed prefix once, checking complete record bounds, identities, increasing IDs, consecutive ordinals, supported direct destinations and consecutive per-Queue sequences. A malformed accepted prefix fails closed; recovery does not lower the tail or salvage a shorter history. A reader which encounters a malformed newly committed range becomes unusable until reopened. Process termination is qualified; recovery from machine power loss and synchronous disk flushing are not promised.

## Borrowed and owning replay

`ProducerLogReader` uses a read-only mapping and acquire-loads the live tail through the native load-only atomic adapter. It validates the initial prefix at open. `capture()` checks only records appended since its last validation and returns a **replay cut**: a fixed committed boundary and record count bound to this Session, producer and file identity. Later acceptances are absent from that cut. `next(cursor)` walks it in producer order; there is no Session-wide total order or consumer execution trace.

`view(reference)` accepts exact indexed committed boundaries. A structurally valid reference, reserved file bytes or a provisional suffix alone cannot make a record visible. A borrowed `PublicationRecordView` preserves its immutable `MessageHeader` and points into the reader's mapping; keep that reader alive while using it. Iteration does not invalidate earlier views because this profile never overwrites accepted records. Writer views similarly require the writer's lifetime. Handles cannot be moved to another thread for use.

`copy_record(reference, maxPayloadBytes)` returns an independent `OwnedPublicationRecord` containing the complete logical header, payload, destination outcomes, ordinal, catalog revision and schema/topology references. It survives reader destruction and enforces the caller's payload budget. This is an owning **storage record**, not the public `Message`: it retains schema identity, but not a bound Registry decoder. Applications will receive public Message/MessageView replay integration with retained catalog bindings in the later public-admission step. This layer does not render schemas or reproduce consumer processing order.

## Qualification and performance

[Functional tests](../test/test_producer_log.cpp) explain identity issuance, restart, retry/conflict, exhaustion, independent streams, frozen cuts, copying and misuse. The [independent worker](../test/producer_log_worker.cpp) pauses or is killed during partial materialization, after complete bytes but before the tail store, and after the tail store but before return. Readers must hide both precommit states and expose the committed state; replacement must recover that same boundary. These are actual mapped append operations. The private partial-materialization hook deliberately creates a provisional prefix before the complete codec write; it represents interrupted bytes, not a proof for every machine instruction.

The isolated [performance test](../test/test_producer_log_append_performance.cpp) runs in ordinary CI. For 64/256/1,024-byte payloads and initial histories H=0/128/1,024, it measures complete local ID issuance plus prepared append, incremental capture of 64 new records, borrowed replay, and cold writer recovery separately. H is the number of accepted records before the first timing block; subsequent blocks explicitly report their increasing starting counts. Warmup uses a separate stream. Every result is verified, with zero-allocation guards and separate diagnostic filesystem/mapping/gate counters plus positive controls. Reader calls include process/thread ownership checks; counters do not claim all system calls are absent.

Expected direction: established append and per-record replay work should remain independent of old history, while fresh-handle recovery grows with the prefix it must check. Recovery timing uses an already created stream and warm OS cache; it is not a cold-disk test. This step establishes a measured mapped-append baseline; it has no former equivalent implementation. The older byte-codec measurements remain a separate cost study. Timings are block means, not per-message latency percentiles, and hosted wall times have no flaky pass threshold. This is not delivered-message latency/throughput or a nanomsg comparison. Matched transport comparisons resume when the replacement delivery path exists. See [retained results and plots](../benchmarks/results/milestone-4-producer-log-append/README.md).

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON
cmake --build build --parallel 3
./build/angaria_tests --gtest_filter='ProducerLogTest.*'
ctest --test-dir build -R producer_log_append_performance --output-on-failure
```
