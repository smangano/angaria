# Direct recorded publication

Framework **0.4.39** supplies producer-owned publication through public `Task` and `QueueWriter` handles. Complete all definitions/activation before first opening or ID issuance; that operation seals setup. Every outgoing writer for a Task shares one producer log, ID issuer and producer mutex, with all registered exact Binary schemas cached once at opening.

1. Attach the producer Task and open its named Queue writer.
2. Prepare encoded bytes and a `MessageView` with schema, format, payload byte count and flags.
3. Issue `Task::next_publication_id()`, then call `writer.send(id, view)`.
4. Check acceptance or a typed error; use `publication_status(id)` to resolve that exact ID.

The owning overload returns `PublicationOutcome`. The buffered overload returns `PublicationReport` and borrows caller-supplied `DestinationOutcome` storage; it avoids allocating a result vector on the hot path. A too-small output buffer rejects before recording.

| Result | Meaning |
| --- | --- |
| Accepted destination | Immutable replayable message accepted; `_queueSequence` names its Queue position. |
| Rejected destination, `QueueFull` reason | Known unaccepted: no record appended, no destination sequence; retry after credit returns. |
| `LogFull` | Producer byte/publication reservation exhausted. Receiving/acknowledging does not reclaim it. |
| `Conflict` | An accepted ID is being reused with changed meaningful input or destination. |
| `OutcomeUnknown` / uncertain I/O | Retain the ID and resolve its recorded outcome rather than assuming rejection. |

A Task accepts increasing IDs across its Queues. A later acceptance retires earlier still-unaccepted IDs. Process threads coordinate a multi-call ID/send/retry sequence if they need that ordering; independent Thread Tasks have separate issuers. Accepted retries return the original destination sequence even after Task replacement.

The runtime establishes authoritative publication identity, publication time and destination metadata. It uses the input schema/format/size/flags and bytes, preserving the caller's input object. `Message` and `MessageView` expose consistent headers and copies retain exactly what was observed.

Acceptance and processing completion differ. For an acknowledged Source, compare the destination sequence with `writer.acknowledged_sequence()`, a committed processing lower bound that may lag until interrupted consumer recovery. The query is unsupported in acknowledgment-free mode. Send itself does not automatically block for consumer processing.

See [lesson 14](../examples/primer/14_direct_send.cpp), [public admission](public-runtime-admission.md), [receive](native-receive.md), and [recorded performance qualification](public-runtime-performance.md). The Session administrative gate is absent from established issuance/send/status operations.
