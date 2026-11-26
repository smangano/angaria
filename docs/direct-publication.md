# Direct Queue publication

Milestone 4, step 4.6, framework **0.4.8**, implements `QueueWriter::send` and `Task::publication_status`. Public Session factories now create core native format **2.2/`0x7`**. The [fourteenth primer program](../examples/primer/14_direct_send.cpp) demonstrates the public path. Receiving, acknowledgment, and explicit Source close follow in step 4.7; Publishing fan-out and retry follow in milestone 6.

## Ordinary application use

Open the output once, prepare an exact schema's payload, and issue an ID before sending:

```cpp
auto writer_result = feed.open_writer("FeedToStrategy");
if (const auto* error = std::get_if<Error>(&writer_result))
{
  std::cerr << error->_path << ": " << error->_message << '\n';
  return 1;
}
auto writer = std::get<QueueWriter>(std::move(writer_result));
MessageHeader header;
header._schema = codec.schema();
header._payloadBytes = bytes.size();
MessageView message{header, bytes};
auto id_result = feed.next_publication_id();
if (const auto* error = std::get_if<Error>(&id_result))
{
  std::cerr << error->_path << ": " << error->_message << '\n';
  return 1;
}
const auto id = std::get<PublicationId>(id_result);
auto result = writer.send(id, message);
if (const auto* error = std::get_if<Error>(&result))
{
  if (error->_code == ErrorCode::OutcomeUnknown)
  {
    // Preserve this ID for recovery lookup instead of issuing another one.
    std::cerr << "OutcomeUnknown for sequence " << id._sequence << '\n';
  }
  std::cerr << error->_path << ": " << error->_message << '\n';
  return 1;
}
auto outcome = std::get<PublicationOutcome>(std::move(result));
```

This main-body fragment checks each public Result before extracting its value, as in the [primer](primer.md). `message` borrows `bytes` for the call; successful acceptance stores independent bytes. The header's schema, format, byte count, and flags describe the input. The explicit ID and runtime clock establish publication identity/time. Historical publication/destination fields in an input header never override the new request, and send leaves the caller's header unchanged.

The ordinary overload returns `Result<PublicationOutcome>`. The outcome owns its destination vector and has `AcceptanceStatus::Accepted`, `Partial`, or `Rejected`; direct Queue acceptance has one accepted destination. Operational failures use the outer `Error`, including `OutcomeUnknown`. No automatic second ID is issued after uncertainty.

The buffer-taking overload returns `PublicationReport` and requires space for one `DestinationOutcome`. It borrows that buffer; check `_error` before `_status`. An insufficient buffer returns `OutputBufferTooSmall` with `_requiredCapacity = 1`, without creating an acceptance. The same sizing error during lookup says nothing about whether the original send committed.

Name overloads for Task attachment and endpoint opening provide the shorter setup path approved in the [public interface review](public-interface.md). Typed-ID overloads retain their existing behavior.

## Identity, validation, and retries

Fresh sends require an already-issued positive ID from the current live producer claim and an Active Queue owned by that Task. Binary admission checks the exact registered schema, exact byte length, Boolean wire values, declared enum numbers, and fixed ASCII. `BinaryPayloadCodec::validate` performs this check without constructing decoded application values. Ordinary char bytes and all supported IEEE floating-point bit patterns remain valid. Known Json/Xml/Csv/Text payloads are opaque; no serializer or syntax validation is implied.

An identical repeat under the same current-claim ID returns the original acceptance without appending another Publication or advancing Queue order. Endpoint, schema, format, flags, payload length, and bytes must match; otherwise it returns `Conflict`. Existing acceptance is resolved before fresh allocation and counter checks, so repeating it still succeeds when the append log or operation counter is exhausted.

`Task::publication_status(id)` recovers interrupted work before reporting its outcome. The same logical Task can query a retained earlier incarnation's ID. A current unissued ID is invalid, while provable absence of an issued ID reports `Rejected` with no destinations. Milestone 4 never reclaims publication history; retirement fences and `HistoryUnavailable` are later milestone-8 work. An earlier incarnation's ID cannot create a fresh send.

## Atomic acceptance and bounded failures

One existing journal transaction commits the immutable Publication, its Pending receipt, Queue order/links, and ring entry. The whole allocation is sized before copying a large payload: record framing, destination metadata, receipt, manifest, redo, and padding all count against the log ceiling. Known rejection creates none of these resources. Recovery of previously interrupted work can legitimately repair the Session before a later request rejects.

A full ring is an overwriteable cache, not a pending-message limit. Sending while the consumer is offline is supported. Accepted history remains after producer detachment/replacement, ring overwrite, or consumer progress. Acknowledgment in the next step will end a delivery obligation without reclaiming append space.

Precommit failures with proven rollback create no delivery. Once committed, repair either establishes the accepted result or returns `OutcomeUnknown`. Retain the original ID and query it; an uncertain result is never presented as known rejection. These are process-recovery guarantees, without a new power-loss durability guarantee.

Legacy **2.0/`0x1`** and **2.1/`0x3`** Sessions remain attachable for their existing Registry, claim, and codec behavior. Direct send and status return `UnsupportedPolicy` before new delivery work. No in-place profile upgrade is provided. Allowed-loss format **2.2/`0xf`** remains rejected before recovery until step 4.8.

[Native transport contract](native-transport.md) · [Primer](primer.md#14-send-directly-and-look-up-acceptance) · [Milestone plan](milestones/milestone-4.md)
