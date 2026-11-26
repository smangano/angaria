// Publish into a Queue while its consumer is offline, then inspect the durable
// acceptance. Receiving and acknowledging arrive in the next primer lesson.
#include "example_support.h"
#include "angaria/payload.h"

#include <cstdint>
#include <exception>
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

  std::pair<Session, PublicationId> send_quote(const std::filesystem::path& directory)
  {
    TaskDescriptor feed;
    feed._name = "Feed";
    auto created = primer::value(Session::create(
        primer::session_options(directory, "First native send"), feed));
    auto registry = created._creator.registry();
    TaskDescriptor strategy;
    strategy._name = "Strategy";
    const auto strategy_id = primer::value(registry.register_task(strategy));

    SourceQueueSetDescriptor inputs;
    inputs._name = "StrategyInputs";
    inputs._owner = strategy_id;
    const auto source_id = primer::value(registry.register_source_set(inputs));
    QueueDescriptor queue;
    queue._name = "FeedToStrategy";
    queue._producer = created._creator.claim()._task;
    queue._sourceSet = source_id;
    queue._ringCapacity = 2;
    const auto queue_id = primer::value(registry.register_queue(queue));
    primer::value(registry.activate_source_set(source_id));
    primer::value(registry.activate_queue(queue_id));

    FieldDescriptor price;
    price._id = static_cast<FieldId>(0xee000001);
    price._name = "PriceTicks";
    price._base = EFieldBase::BASE_INT64;
    price._size = 8;
    primer::value(registry.register_field(price));
    MessageDescriptor quote;
    quote._schema = {static_cast<MessageType>(1), 1};
    quote._name = "Quote";
    quote._fields = {price._id};
    const auto schema = primer::value(registry.register_message(quote));
    const auto codec = primer::value(BinaryPayloadCodec::from_registry(registry, schema, 64));
    auto bytes = primer::value(codec.encode({std::int64_t{12345}}));

    // A MessageView borrows bytes. Keep this vector alive while send reads it.
    // Prepared input has no publication ID/time; the explicit ID and runtime
    // clocks establish the stored header. Sending leaves our header unchanged.
    MessageHeader header;
    header._schema = schema;
    header._payloadBytes = bytes.size();
    const MessageView message{header, bytes};
    auto writer = primer::value(created._creator.open_writer("FeedToStrategy"));
    const auto publication = primer::value(created._creator.next_publication_id());
    const auto accepted = primer::value(writer.send(publication, message));
    require_accepted(accepted);
    std::cout << "Accepted quote while Strategy is offline.\n";

    // Repeating the same ID and bytes returns the first acceptance without
    // creating a second delivery. Use a new issued ID for a different message.
    require_accepted(primer::value(writer.send(publication, message)));
    std::cout << "Identical repeat: accepted once.\n";
    bytes.front() ^= std::byte{1};
    const auto conflict = writer.send(publication, message);
    const auto* error = std::get_if<Error>(&conflict);
    if (!error || error->_code != ErrorCode::Conflict)
    {
      throw std::runtime_error("Reusing the ID for different bytes must conflict");
    }
    std::cout << "Different bytes with the same ID: Conflict.\n";
    if (message._header != header)
    {
      throw std::runtime_error("Sending changed the caller's MessageHeader");
    }

    // An issued but unused ID has a provably absent acceptance. Lookup reports
    // Rejected with no destinations; issuing an ID does not send anything.
    const auto unused = primer::value(created._creator.next_publication_id());
    const auto absent = primer::value(created._creator.publication_status(unused));
    if (absent._status != AcceptanceStatus::Rejected || !absent._destinations.empty())
    {
      throw std::runtime_error("An unused ID unexpectedly has an acceptance");
    }
    std::cout << "Unused issued ID: rejected, no destinations.\n";
    return {std::move(created._session), publication};
    // The producer, writer, and actor-bound Registry close together here.
  }
}

int main(int argc, char** argv)
{
  if (argc != 2)
  {
    std::cerr << "Usage: " << argv[0] << " <new-session-directory>\n";
    return 1;
  }
  try
  {
    auto [session, publication] = send_quote(argv[1]);
    auto replacement = primer::value(session.attach_task("Feed"));
    require_accepted(primer::value(replacement.publication_status(publication)));
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
