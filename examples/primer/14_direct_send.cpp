// Publish into a Queue while its consumer is offline, then inspect the recorded
// acceptance. Receiving and acknowledging arrive in the next primer lesson.
#include "angaria/runtime.h"
#include "angaria/payload.h"

#include <cstdint>
#include <exception>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <utility>
#include <variant>

namespace
{
  using namespace angaria;

  void require_accepted(const PublicationOutcome& outcome)
  {
    if (outcome._status != AcceptanceStatus::Accepted || outcome._destinations.size() != 1 ||
        outcome._destinations.front()._status != DestinationStatus::Accepted)
    {
      throw std::runtime_error("Expected acceptance into the configured Queue");
    }
  }

  Result<std::pair<Session, PublicationId>> send_quote(const std::filesystem::path& directory)
  {
    TaskDescriptor feed;
    feed._name = "Feed";
    SessionOptions options;
    options._name = "First native send";
    options._directory = directory;
    options._maxLogBytes = 1024 * 1024;
    options._limits = CapacityLimits::empty().tasks(8).queues(8)
        .source_sets(4).publishing_sets(4).memberships_per_set(4)
        .fields(16).message_schemas(8);
    options._defaults._ringCapacity = 8;
    auto create_result = Session::create(
        options, feed);
    if (const auto* error = std::get_if<Error>(&create_result))
    {
      return *error;
    }
    auto created = std::get<0>(std::move(create_result));
    auto registry = created._creator.registry();
    TaskDescriptor strategy;
    strategy._name = "Strategy";
    auto register_task_result = registry.register_task(strategy);
    if (const auto* error = std::get_if<Error>(&register_task_result))
    {
      return *error;
    }
    const auto strategy_id = std::get<0>(std::move(register_task_result));

    SourceQueueSetDescriptor inputs;
    inputs._name = "StrategyInputs";
    inputs._owner = strategy_id;
    auto register_source_set_result = registry.register_source_set(inputs);
    if (const auto* error = std::get_if<Error>(&register_source_set_result))
    {
      return *error;
    }
    const auto source_id = std::get<0>(std::move(register_source_set_result));
    QueueDescriptor queue;
    queue._name = "FeedToStrategy";
    queue._producer = created._creator.claim()._task;
    queue._sourceSet = source_id;
    queue._ringCapacity = 2;
    auto register_queue_result = registry.register_queue(queue);
    if (const auto* error = std::get_if<Error>(&register_queue_result))
    {
      return *error;
    }
    const auto queue_id = std::get<0>(std::move(register_queue_result));
    auto activate_source_set_result = registry.activate_source_set(source_id);
    if (const auto* error = std::get_if<Error>(&activate_source_set_result))
    {
      return *error;
    }

    auto activate_queue_result = registry.activate_queue(queue_id);
    if (const auto* error = std::get_if<Error>(&activate_queue_result))
    {
      return *error;
    }

    FieldDescriptor price;
    price._id = static_cast<FieldId>(0xee000001);
    price._name = "PriceTicks";
    price._base = EFieldBase::BASE_INT64;
    price._size = 8;
    auto register_field_result = registry.register_field(price);
    if (const auto* error = std::get_if<Error>(&register_field_result))
    {
      return *error;
    }

    MessageDescriptor quote;
    quote._schema = {static_cast<MessageType>(1), 1};
    quote._name = "Quote";
    quote._fields = {price._id};
    auto register_message_result = registry.register_message(quote);
    if (const auto* error = std::get_if<Error>(&register_message_result))
    {
      return *error;
    }
    const auto schema = std::get<0>(std::move(register_message_result));
    auto from_registry_result = BinaryPayloadCodec::from_registry(registry, schema, 64);
    if (const auto* error = std::get_if<Error>(&from_registry_result))
    {
      return *error;
    }
    const auto codec = std::get<0>(std::move(from_registry_result));
    auto encode_result = codec.encode({std::int64_t{12345}});
    if (const auto* error = std::get_if<Error>(&encode_result))
    {
      return *error;
    }
    auto bytes = std::get<0>(std::move(encode_result));

    // A MessageView borrows bytes. Keep this vector alive while send reads it.
    // Prepared input has no publication ID/time; the explicit ID and runtime
    // clocks establish the stored header. Sending leaves our header unchanged.
    MessageHeader header;
    header._schema = schema;
    header._payloadBytes = bytes.size();
    const MessageView message{header, bytes};
    auto open_writer_result = created._creator.open_writer("FeedToStrategy");
    if (const auto* error = std::get_if<Error>(&open_writer_result))
    {
      return *error;
    }
    auto writer = std::get<0>(std::move(open_writer_result));
    auto next_publication_id_result = created._creator.next_publication_id();
    if (const auto* error = std::get_if<Error>(&next_publication_id_result))
    {
      return *error;
    }
    const auto publication = std::get<0>(std::move(next_publication_id_result));
    auto send_result = writer.send(publication, message);
    if (const auto* error = std::get_if<Error>(&send_result))
    {
      if (error->_code != ErrorCode::OutcomeUnknown)
      {
        return *error;
      }
      // An uncertain call can already have committed. Query the SAME issued
      // identity; issuing a replacement ID could create another delivery.
      auto recovered_result = created._creator.publication_status(publication);
      if (const auto* recovery_error = std::get_if<Error>(&recovered_result))
      {
        std::cerr << "Keep publication Task=" << static_cast<unsigned>(publication._producer._task)
                  << " incarnation=" << publication._producer._incarnation
                  << " sequence=" << publication._sequence << " for later lookup.\n";
        return *recovery_error;
      }
      send_result = std::get<PublicationOutcome>(std::move(recovered_result));
    }
    const auto accepted = std::get<0>(std::move(send_result));
    require_accepted(accepted);
    std::cout << "Accepted quote while Strategy is offline.\n";

    // Repeating the same ID and bytes returns the first acceptance without
    // creating a second delivery. Use a new issued ID for a different message.
    auto send_result_2 = writer.send(publication, message);
    if (const auto* error = std::get_if<Error>(&send_result_2))
    {
      return *error;
    }
    require_accepted(std::get<0>(std::move(send_result_2)));
    std::cout << "Identical repeat: accepted once.\n";
    bytes.front() ^= std::byte{1};
    const auto conflict = writer.send(publication, message);
    const auto* error = std::get_if<Error>(&conflict);
    if (!error)
    {
      return Error{ErrorCode::InvalidState, "Expected Conflict for different bytes under the same ID", "lesson.repeat"};
    }
    if (error->_code != ErrorCode::Conflict)
    {
      return *error;
    }
    std::cout << "Different bytes with the same ID: Conflict.\n";
    if (message._header != header)
    {
      throw std::runtime_error("Sending changed the caller's MessageHeader");
    }

    // An issued but unused ID has a provably absent acceptance. Lookup reports
    // Rejected with no destinations; issuing an ID does not send anything.
    auto next_publication_id_result_2 = created._creator.next_publication_id();
    if (const auto* error = std::get_if<Error>(&next_publication_id_result_2))
    {
      return *error;
    }
    const auto unused = std::get<0>(std::move(next_publication_id_result_2));
    auto publication_status_result = created._creator.publication_status(unused);
    if (const auto* error = std::get_if<Error>(&publication_status_result))
    {
      return *error;
    }
    const auto absent = std::get<0>(std::move(publication_status_result));
    if (absent._status != AcceptanceStatus::Rejected || !absent._destinations.empty())
    {
      throw std::runtime_error("An unused ID unexpectedly has an acceptance");
    }
    std::cout << "Unused issued ID: rejected, no destinations.\n";
    return std::pair<Session, PublicationId>{std::move(created._session), publication};
    // The producer, writer, and actor-bound Registry close together here.
  }
}

int main(int argc, char** argv)
{
  using namespace angaria;
  if (argc != 2)
  {
    std::cerr << "Usage: " << argv[0] << " <new-session-directory>\n";
    return 1;
  }
  try
  {
    auto send_quote_result = send_quote(argv[1]);
    if (const auto* error = std::get_if<Error>(&send_quote_result))
    {
      if (error->_code == ErrorCode::Conflict)
      {
        std::cerr << "Conflict: ";
      }
      std::cerr << "Error " << static_cast<unsigned>(error->_code) << " at "
                << error->_path << ": " << error->_message << '\n';
      return 1;
    }
    auto [session, publication] = std::get<0>(std::move(send_quote_result));
    auto attach_task_result = session.attach_task("Feed");
    if (const auto* error = std::get_if<Error>(&attach_task_result))
    {
      if (error->_code == ErrorCode::Conflict)
      {
        std::cerr << "Conflict: ";
      }
      std::cerr << "Error " << static_cast<unsigned>(error->_code) << " at "
                << error->_path << ": " << error->_message << '\n';
      return 1;
    }
    auto replacement = std::get<0>(std::move(attach_task_result));
    auto publication_status_result_2 = replacement.publication_status(publication);
    if (const auto* error = std::get_if<Error>(&publication_status_result_2))
    {
      if (error->_code == ErrorCode::Conflict)
      {
        std::cerr << "Conflict: ";
      }
      std::cerr << "Error " << static_cast<unsigned>(error->_code) << " at "
                << error->_path << ": " << error->_message << '\n';
      return 1;
    }
    require_accepted(std::get<0>(std::move(publication_status_result_2)));
    std::cout << "Replacement Feed incarnation: " << replacement.claim()._incarnation << '\n'
              << "Earlier incarnation's acceptance remains queryable.\n"
              << "Acceptance records Queue entry; consumer processing is a later step.\n";
    return 0;
  }
  catch (const std::exception& error)
  {
    std::cerr << argv[1] << ": " << error.what() << '\n';
    return 1;
  }
}
