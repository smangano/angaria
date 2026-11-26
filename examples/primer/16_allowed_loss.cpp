// Configure a Queue for stale-data loss, inspect gaps, and show that overwrite
// cannot revoke a message that the consumer already borrowed.
#include "angaria/runtime.h"
#include "angaria/payload.h"

#include <cstdint>
#include <cstddef>
#include <exception>
#include <iostream>
#include <utility>
#include <variant>
#include <vector>

namespace
{
  using namespace angaria;

  OperationResult send_tick(Task& feed, QueueWriter& writer,
                              const BinaryPayloadCodec& codec, std::int64_t value)
  {
    auto encoded = codec.encode({value});
    if (const auto* error = std::get_if<Error>(&encoded))
    {
      return *error;
    }
    const auto bytes = std::get<std::vector<std::byte>>(std::move(encoded));
    auto issued = feed.next_publication_id();
    if (const auto* error = std::get_if<Error>(&issued))
    {
      return *error;
    }
    const auto id = std::get<PublicationId>(issued);
    MessageHeader header;
    header._schema = codec.schema();
    header._payloadBytes = bytes.size();
    auto sent = writer.send(id, MessageView{header, bytes});
    if (const auto* error = std::get_if<Error>(&sent))
    {
      if (error->_code != ErrorCode::OutcomeUnknown)
      {
        return *error;
      }
      // Resolve the original request; an uncertain result must not cause a
      // second publication under a replacement identity.
      auto recovered = feed.publication_status(id);
      if (const auto* recovery_error = std::get_if<Error>(&recovered))
      {
        std::cerr << "Keep publication Task=" << static_cast<unsigned>(id._producer._task)
                  << " incarnation=" << id._producer._incarnation
                  << " sequence=" << id._sequence << " for later lookup.\n";
        return *recovery_error;
      }
      sent = std::get<PublicationOutcome>(std::move(recovered));
    }
    if (std::get<PublicationOutcome>(sent)._status != AcceptanceStatus::Accepted)
    {
      return Error{ErrorCode::InvalidState, "Tick was not accepted", "lesson.send"};
    }
    return std::monostate{};
  }

  // These scenario-specific functions check ReceiveResult directly. They do
  // not replace Angaria's generic result handling or hide configuration.
  Result<DeliveryGap> receive_gap(SourceQueueSet& source)
  {
    auto event = source.receive();
    if (const auto* error = std::get_if<Error>(&event))
    {
      return *error;
    }
    if (const auto* gap = std::get_if<DeliveryGap>(&event))
    {
      return *gap;
    }
    return Error{ErrorCode::InvalidState, "Expected the overwritten pending prefix to be reported", "lesson.gap"};
  }

  Result<Delivery> receive_tick(SourceQueueSet& source)
  {
    auto event = source.receive();
    if (const auto* error = std::get_if<Error>(&event))
    {
      return *error;
    }
    if (auto* delivery = std::get_if<Delivery>(&event))
    {
      return std::move(*delivery);
    }
    return Error{ErrorCode::InvalidState, "Expected a retained tick", "lesson.receive"};
  }

  OperationResult run_lesson(const char* configuration)
  {
    auto creation = Session::create_from_file(configuration, "Feed");
    if (const auto* error = std::get_if<Error>(&creation))
    {
      return *error;
    }
    auto created = std::get<CreatedSession>(std::move(creation));
    auto options_result = created._session.options();
    if (const auto* error = std::get_if<Error>(&options_result))
    {
      return *error;
    }
    const auto options = std::get<SessionOptions>(std::move(options_result));
    if (options._defaults._delivery != DeliveryPolicy::RecoverFromLog)
    {
      return Error{ErrorCode::InvalidConfiguration, "This lesson keeps the Session default as recovery", "defaults.queue.delivery"};
    }
    auto registry = created._session.registry();
    auto schema_result = registry.find_message("Tick", 1);
    if (const auto* error = std::get_if<Error>(&schema_result))
    {
      return *error;
    }
    const auto schema = std::get<SchemaId>(schema_result);
    auto codec_result = BinaryPayloadCodec::from_registry(registry, schema, 8);
    if (const auto* error = std::get_if<Error>(&codec_result))
    {
      return *error;
    }
    const auto codec = std::get<BinaryPayloadCodec>(std::move(codec_result));
    auto writer_result = created._creator.open_writer("Ticks");
    if (const auto* error = std::get_if<Error>(&writer_result))
    {
      return *error;
    }
    auto writer = std::get<QueueWriter>(std::move(writer_result));
    auto queue_result = registry.describe_queue(writer.id());
    if (const auto* error = std::get_if<Error>(&queue_result))
    {
      return *error;
    }
    const auto queue = std::get<QueueDescriptor>(std::move(queue_result));
    if (queue._delivery != DeliveryPolicy::AllowLoss || queue._ringCapacity != 2)
    {
      return Error{ErrorCode::InvalidConfiguration, "This lesson requires allow_loss and ring capacity 2", "queues.Ticks"};
    }
    std::cout << "Configured delivery: allow_loss (ring capacity 2).\n";

    for (std::int64_t tick = 1; tick <= 3; ++tick)
    {
      auto sent = send_tick(created._creator, writer, codec, tick);
      if (const auto* error = std::get_if<Error>(&sent))
      {
        return *error;
      }
    }
    auto attachment = created._session.attach_task("Consumer");
    if (const auto* error = std::get_if<Error>(&attachment))
    {
      return *error;
    }
    auto consumer = std::get<Task>(std::move(attachment));
    auto opening = consumer.open_source_set("ConsumerInputs");
    if (const auto* error = std::get_if<Error>(&opening))
    {
      return *error;
    }
    auto source = std::get<SourceQueueSet>(std::move(opening));
    auto first_gap_result = receive_gap(source);
    if (const auto* error = std::get_if<Error>(&first_gap_result))
    {
      return *error;
    }
    const auto first_gap = std::get<DeliveryGap>(first_gap_result);
    if (first_gap._firstSequence != 1 || first_gap._lastSequence != 1)
    {
      return Error{ErrorCode::InvalidState, "Unexpected initial gap", "lesson.gap"};
    }
    std::cout << "Gap: 1..1\n";
    auto first_tick_result = receive_tick(source);
    if (const auto* error = std::get_if<Error>(&first_tick_result))
    {
      return *error;
    }
    const auto first_tick = std::get<Delivery>(std::move(first_tick_result));
    if (first_tick._message._header._queueSequence != 2)
    {
      return Error{ErrorCode::InvalidState, "Expected Queue sequence 2", "lesson.receive"};
    }
    std::cout << "Received after gap: 2\n";
    for (std::int64_t tick = 4; tick <= 5; ++tick)
    {
      auto sent = send_tick(created._creator, writer, codec, tick);
      if (const auto* error = std::get_if<Error>(&sent))
      {
        return *error;
      }
    }
    auto repeated_result = receive_tick(source);
    if (const auto* error = std::get_if<Error>(&repeated_result))
    {
      return *error;
    }
    const auto repeated = std::get<Delivery>(std::move(repeated_result));
    if (repeated._token != first_tick._token ||
        repeated._message._payload.data() != first_tick._message._payload.data())
    {
      return Error{ErrorCode::InvalidState, "Overwrite changed the in-flight borrow", "lesson.pin"};
    }
    std::cout << "In-flight sequence 2 survives ring overwrite.\n";
    auto released = source.release(repeated._token);
    if (const auto* error = std::get_if<Error>(&released))
    {
      return *error;
    }
    // Both old payload views have expired. After release, sequence 2 is once
    // again Pending and the current overwrite boundary may make it stale.
    auto second_gap_result = receive_gap(source);
    if (const auto* error = std::get_if<Error>(&second_gap_result))
    {
      return *error;
    }
    const auto second_gap = std::get<DeliveryGap>(second_gap_result);
    if (second_gap._firstSequence != 2 || second_gap._lastSequence != 3)
    {
      return Error{ErrorCode::InvalidState, "Unexpected gap after release", "lesson.gap"};
    }
    std::cout << "Gap after release: 2..3\n";
    for (std::uint64_t sequence = 4; sequence <= 5; ++sequence)
    {
      auto delivery_result = receive_tick(source);
      if (const auto* error = std::get_if<Error>(&delivery_result))
      {
        return *error;
      }
      const auto delivery = std::get<Delivery>(std::move(delivery_result));
      if (delivery._message._header._queueSequence != sequence)
      {
        return Error{ErrorCode::InvalidState, "Retained Queue order changed", "lesson.receive"};
      }
      auto acknowledged = source.acknowledge(delivery._token);
      if (const auto* error = std::get_if<Error>(&acknowledged))
      {
        return *error;
      }
    }
    std::cout << "Received retained sequences: 4,5\n";
    auto final_event = source.receive();
    if (const auto* error = std::get_if<Error>(&final_event))
    {
      return *error;
    }
    if (!std::holds_alternative<ReceiveTimeout>(final_event))
    {
      return Error{ErrorCode::InvalidState, "Expected timeout after the retained ticks", "lesson.receive"};
    }
    auto closed = source.close();
    if (const auto* error = std::get_if<Error>(&closed))
    {
      return *error;
    }
    std::cout << "Final receive: timeout.\n"
              << "Default delivery remains recover_from_log.\n";
    return std::monostate{};
  }
}

int main(int argc, char** argv)
{
  if (argc != 2)
  {
    std::cerr << "Usage: " << argv[0] << " <allowed-loss.toml>\n";
    return 1;
  }
  try
  {
    const auto result = run_lesson(argv[1]);
    if (const auto* error = std::get_if<angaria::Error>(&result))
    {
      if (error->_code == angaria::ErrorCode::Conflict)
      {
        std::cerr << "Conflict: ";
      }
      std::cerr << error->_path << ": " << error->_message
                << " (code " << static_cast<int>(error->_code) << ")\n";
      return 1;
    }
    return 0;
  }
  catch (const std::exception& error)
  {
    std::cerr << argv[1] << ": " << error.what() << '\n';
    return 1;
  }
}
