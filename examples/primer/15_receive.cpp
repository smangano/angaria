// Follow retained Queue order, release and receive again, copy a message for
// later use, and explicitly close a Source with unfinished work.
#include "example_support.h"
#include "angaria/payload.h"

#include <array>
#include <cstdint>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <utility>
#include <variant>

namespace
{
  using namespace angaria;

  Delivery receive_delivery(SourceQueueSet& inputs)
  {
    auto event = inputs.receive();
    if (const auto* error = std::get_if<Error>(&event))
    {
      throw std::runtime_error(std::string{primer::error_name(error->_code)} + ": " + error->_message);
    }
    if (auto* delivery = std::get_if<Delivery>(&event))
    {
      return std::move(*delivery);
    }
    throw std::runtime_error("Expected an available delivery");
  }

  struct SavedQuote
  {
    Message _message;
    BinaryPayloadCodec _codec;
  };

  SavedQuote run_lesson(const std::filesystem::path& directory)
  {
    TaskDescriptor feed;
    feed._name = "Feed";
    auto created = primer::value(Session::create(
        primer::session_options(directory, "Receive and keep a Quote"), feed));
    auto registry = created._creator.registry();
    TaskDescriptor strategy;
    strategy._name = "Strategy";
    const auto strategy_id = primer::value(registry.register_task(strategy));
    SourceQueueSetDescriptor input;
    input._name = "StrategyInputs";
    input._owner = strategy_id;
    const auto source_id = primer::value(registry.register_source_set(input));
    QueueDescriptor queue;
    queue._name = "FeedToStrategy";
    queue._producer = created._creator.claim()._task;
    queue._sourceSet = source_id;
    queue._ringCapacity = 2;
    const auto queue_id = primer::value(registry.register_queue(queue));
    primer::value(registry.activate_source_set(source_id));
    primer::value(registry.activate_queue(queue_id));

    FieldDescriptor price;
    price._id = FieldId{0xee000001};
    price._name = "PriceTicks";
    price._base = EFieldBase::BASE_INT64;
    price._size = 8;
    primer::value(registry.register_field(price));
    MessageDescriptor quote;
    quote._schema = {MessageType{1}, 1};
    quote._name = "Quote";
    quote._fields = {price._id};
    const auto schema = primer::value(registry.register_message(quote));
    auto codec = primer::value(BinaryPayloadCodec::from_registry(registry, schema, 64));
    auto writer = primer::value(created._creator.open_writer("FeedToStrategy"));
    PublicationId first_publication;
    for (const auto value : std::array<std::int64_t, 3>{12345, 12346, 12347})
    {
      const auto bytes = primer::value(codec.encode({value}));
      MessageHeader header;
      header._schema = schema;
      header._payloadBytes = bytes.size();
      const auto publication = primer::value(created._creator.next_publication_id());
      const auto outcome = primer::value(writer.send(publication, MessageView{header, bytes}));
      if (outcome._status != AcceptanceStatus::Accepted)
      {
        throw std::runtime_error("Expected Queue acceptance");
      }
      if (first_publication == PublicationId{})
      {
        first_publication = publication;
      }
    }

    // Three messages exceed the two-slot ring. Retained history still delivers
    // sequence 1 first; replacing a cache entry never acknowledges a message.
    auto consumer = primer::value(created._session.attach_task("Strategy"));
    auto inputs = primer::value(consumer.open_source_set("StrategyInputs"));
    const auto first = receive_delivery(inputs);
    const auto original_header = first._message._header;
    const auto original_token = first._token;
    if (original_header._publication != first_publication || original_header._schema != schema ||
        original_header._queue != queue_id || original_header._queueSequence != 1 ||
        original_header._consumer != consumer.claim() || !original_header._enqueuedAt ||
        !original_header._dequeuedAt || original_header._publishedAt != *original_header._enqueuedAt)
    {
      throw std::runtime_error("Received header is missing the expected publication/delivery context");
    }
    std::cout << "Recovered oldest Queue sequence after ring wrap: 1\n";

    // Copy while the borrow is valid. The copy keeps every header field and
    // owns independent bytes and schema definitions; it owns no delivery token.
    auto saved = primer::value(first._message.copy_message(64));
    primer::value(inputs.release(original_token));
    // first's payload view has expired. Keep using only our owning saved copy.
    const auto repeated = receive_delivery(inputs);
    if (repeated._token == original_token || repeated._message._header._publication != first_publication)
    {
      throw std::runtime_error("Release did not create a new issuance of the same message");
    }
    primer::value(inputs.acknowledge(repeated._token));
    std::cout << "Release redelivered the same publication with a new token.\n";

    const auto unfinished = receive_delivery(inputs);
    const auto unfinished_token = unfinished._token;
    primer::value(inputs.close());
    // Explicit close releases sequence 2 without acknowledging it. A new
    // opening gets a different token and can finish the delivery normally.
    auto reopened = primer::value(consumer.open_source_set("StrategyInputs"));
    const auto redelivered = receive_delivery(reopened);
    if (redelivered._token == unfinished_token || redelivered._message._header._queueSequence != 2)
    {
      throw std::runtime_error("Closing did not preserve the unfinished message for redelivery");
    }
    primer::value(reopened.acknowledge(redelivered._token));
    std::cout << "Closing an unacknowledged Source allows redelivery.\n";
    const auto last = receive_delivery(reopened);
    primer::value(reopened.acknowledge(last._token));
    if (!std::holds_alternative<ReceiveTimeout>(reopened.receive()))
    {
      throw std::runtime_error("Expected timeout after all three messages were acknowledged");
    }
    primer::value(reopened.close());
    std::cout << "Final receive: timeout.\n";
    if (saved._header != original_header)
    {
      throw std::runtime_error("The owning copy lost delivery header information");
    }
    return {std::move(saved), std::move(codec)};
    // All Session, Task, and endpoint handles close before main decodes this.
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
    const auto saved = run_lesson(argv[1]);
    const auto values = primer::value(saved._message.decode(saved._codec));
    std::cout << "Saved copy after all handles close: PriceTicks=" << std::get<std::int64_t>(values.front()) << '\n'
              << "Saved header kept every delivery field.\n";
    return 0;
  }
  catch (const std::exception& error)
  {
    std::cerr << argv[1] << ": " << error.what() << '\n';
    return 1;
  }
}
