# Receiving messages and closing Sources

Milestone 4, step 4.7, framework **0.4.9**, implements `SourceQueueSet::receive`, `acknowledge`, `release`, and `close` for core native format **2.2/`0x7`**. A Source is the consumer-owned collection of input Queues. This step supports at most one Active Queue for fresh selection; milestone 5 adds selection among several inputs. [Primer lesson 15](../examples/primer/15_receive.cpp) uses only the public API.

## Receive, process, and finish

Open the Source once after attaching its consumer Task. `receive()` returns one of the `ReceiveResult` alternatives:

| Alternative | Meaning in this step |
| --- | --- |
| `Delivery` | One message is borrowed and has an exact acknowledgment/release token. |
| `ReceiveTimeout` | No delivery was offered during this availability attempt or finite wait. |
| `Error` | An ownership, storage, policy, timeout-argument, or other operation failure. |
| `DeliveryGap` | Declared for the next step's configured allowed loss; core recovery delivery does not emit gaps. |

`Delivery::_message` is a `MessageView`; `_token` identifies its live delivery obligation. Inspect metadata through `_message._header`: schema, format, original publication ID/time, byte count, flags, destination Queue/sequence, consumer claim, enqueue time, and this issuance's dequeue time. Destination fields are present even when a timestamp equals the Unix epoch. The payload contains application bytes alone, without a nested portable-record header.

Successful `acknowledge(token)` records completed processing and makes the next Queue sequence eligible. Successful `release(token)` makes the same message available again without acknowledging it. A subsequent issuance gets a new token and dequeue time. All token components must match; an old token cannot complete a newer delivery. Proven unchanged-state failures, including `StaleDelivery`, preserve the current borrow.

While a message is in flight, repeated receive returns the same token, time, and stable payload mapping. It does not select or issue another message. This remains true if the Source gains another Active Queue or its fresh issuance counter is exhausted. Fresh selection with multiple Active Queues returns `UnsupportedPolicy`; zero Active Queues can time out, and Draft Queues are ignored.

## Borrowed bytes and owning copies

The Source owns a read-only mapping and a persisted **pin**, the matching Issued receipt and Source slot. Together they keep the borrowed bytes available while the application processes them outside the mutation gate. Appending messages and overwriting ring cache entries do not move an existing borrow. The oldest pending message is found in retained history even after its ring entry is overwritten.

Successful acknowledgment/release, valid close, move replacement of the owning Source, and claim termination end the affected view's lifetime. Coordinate application reads with operations that end that lifetime. Move construction transfers the incoming pin and mapping, preserving its views; move assignment first closes or defers the destination's old ownership. Do not concurrently move or destroy a handle while calling it.

To keep a message, call `delivery._message.copy_message(maxPayloadBytes)` before ending its borrow. The returned `Message` copies the **entire MessageHeader unchanged**, owns independent payload bytes, and retains the exact current schema definitions. It owns no acknowledgment obligation or Source pin. `Message::view()` exposes the same header with a span borrowing that owned vector. Copying a MessageView alone does not extend the payload lifetime.

The owning copy remains usable after the Source, Task, and Session close. Current Binary decoding uses the exact retained scalar/basic-logical definitions and a matching `BinaryPayloadCodec`; pool-backed types and the full serializer surface remain later milestones. Raw caller-created header/span views remain valid send inputs but have no inferred decoding context.

## Waiting and bounds

Timeouts are nanoseconds measured against a checked monotonic deadline. Negative or overflowing timeouts return `InvalidArgument`. Zero performs one nonblocking gate/availability attempt; a busy gate can therefore return `ReceiveTimeout` even when a delivery is pending. Positive waits retry with bounded gate acquisition, releasing the gate between checks. They never sleep or run application processing while holding it.

After acquisition and recovery, an existing pin can be returned even if the positive deadline has passed. An expired positive deadline cannot issue a fresh delivery. Recovery and an already started mutation still finish consistently; this is not a hard execution-time guarantee. This step uses Spin waiting; Linux eventfd follows in milestone 7.

The bounded ring caches recent references; it is not a pending-delivery limit. Default recovery delivery follows retained history and loses nothing merely because the ring wraps. Receipt progress uses preallocated FixedState storage, so acknowledgment, release, Source cleanup, and replacement require no append space even at `LogFull`. Acknowledgment does not reclaim physical log bytes. Filesystem errors, process-memory limits, and exhausted counters can still fail an operation.

## Explicit close and deferred cleanup

`close()` is observable and retryable. The handle moves through **Active → Closing → Closed**. Wrong native process/thread ownership rejects before starting close. Beginning a valid close ends its views and prevents receive, acknowledge, or release through that handle; only close retry is available. Empty or already closed handles close successfully without new work.

Successful close releases any exact in-flight issuance to Pending, preserves the Source issuance counter, removes the local exclusive opening, and drops its mapping and Task ownership. Another opening may then receive the unfinished message with a new token. Ending the last Task owner happens after releasing the operation gate, so detachment cannot recursively enter it.

A failed explicit close returns an error and keeps its reservation and Task ownership in Closing. A second Source opening cannot bypass it. Retry first recovers and recognizes cleanup already completed by an interrupted attempt.

Destruction and noexcept move assignment attempt the same cleanup. If cleanup fails, a reservation allocated on open becomes a **DeferredClose** marker containing the old Source/claim/issuance identity. It owns neither a Task nor payload mapping. That Task must reconcile deferred cleanup before new delivery/progress or reopening a Source. Publication status and identical-send lookups remain read-only and usable while cleanup is blocked; a fresh send reconciles before acceptance. Recording the marker requires no allocation; it never clears a newer issuance. If all Task owners end, replacement's existing claim cleanup handles remaining persisted pins.

## Interrupted operations and compatibility

Issuance is committed before any payload view is returned. If mapping or schema binding fails afterward, the runtime retains cleanup identity independently of a returned Delivery and attempts exact release. Failed or uncertain cleanup cannot silently consume the message or expose an unowned span. After `OutcomeUnknown`, stop using an earlier view and settle recovery through receive or close before borrowing again.

These are process-recovery guarantees on the existing coherent local-filesystem profile, without a power-loss durability guarantee. Core format/capabilities, record encoding, and synchronization protocol remain unchanged in this step. Existing 2.0/2.1 Sessions reject receive/acknowledge/release with `UnsupportedPolicy` before delivery work; closing an idle legacy Source remains valid cleanup. Allowed loss remains step 4.8.

[Transport contract](native-transport.md) · [API contract](api.md) · [Milestone plan](milestones/milestone-4.md)
