# Receiving recorded messages

The current public runtime is **0.4.39**, native format **3.0 / 0x7**, synchronization **2**. Open a `SourceQueueSet` after configuring and activating its one input Queue and exact schemas. First operational opening seals the complete catalog. A Task may own several Sources; each has its own consumer state. Multi-input selection remains milestone 5.

| `ReceiveResult` alternative | Meaning |
| --- | --- |
| `Delivery` | Default acknowledged mode: a `MessageView` plus its exact processing token. |
| `MessageView` | Explicit acknowledgment-free mode: borrowed header/bytes, no token obligation. |
| `ReceiveTimeout` | This attempt/deadline offered no message. |
| `Error` | Ownership, policy, storage, or timeout-argument failure. |
| `DeliveryGap` | Reserved public result for later configured loss; the admitted profile does not emit it. |

Use TOML `acknowledgment = "required"` (default) or `"none"` on a Source or its default policy. [Lesson 15](../examples/primer/15_receive.cpp) demonstrates acknowledged processing; [lesson 16](../examples/primer/16_unacknowledged.cpp) demonstrates acknowledgment-free delivery. Both use direct public APIs and explicit error handling.

In acknowledged mode, repeat receive retains the current pin. `acknowledge(token)` commits processing completion; `release(token)` makes it available again with a fresh token. Successful close releases an outstanding pin, so replacement can redeliver it. Stale tokens reject. A failed close leaves the handle available for retry; its own progress file recovers selected operations.

Transport credit is returned at receipt, independently of acknowledgment. Producers can compare an accepted destination's `_queueSequence` with `QueueWriter::acknowledged_sequence()`, a committed processing lower bound that can lag during consumer interruption. Acknowledgment-free Sources do not offer that processing watermark.

Both modes expose the same `MessageHeader`. `MessageView::copy_message(limit)` preserves the complete observed header, owns its payload and retains exact decoding definitions. The copy outlives the borrowed view, Source, Task, and Session handles. In acknowledgment-free mode the next receive ends the previous borrow; reopening/replacement can deliver again. Neither credit nor a copied message certifies application processing.

An empty zero-timeout poll performs no Session transaction. A positive timeout spins using its monotonic deadline; negative/unrepresentable values reject. A shared producer log may contain other Queues' records, passed in bounded scan batches. See [admission and lifecycles](public-runtime-admission.md) for setup, Process/Thread ownership, fork+exec, fixed resources, close, and supported policies.

The [public performance guide](public-runtime-performance.md) reports actual independent-process delivery/ACK latency and completed throughput. The prior Session-gated receive engine and configured-loss implementation are reproduced only from pinned historical revisions; current allowed-loss requests return `UnsupportedPolicy`.
