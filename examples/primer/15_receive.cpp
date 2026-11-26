// Follow retained Queue order, release and receive again, copy a message for
// later use, and explicitly close a Source with unfinished work.
#include "angaria/runtime.h"
#include "angaria/payload.h"

#include <array>
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

  Result<Delivery> receive_delivery(SourceQueueSet& inputs)
  {
    auto event = inputs.receive();
    if (const auto* error = std::get_if<Error>(&event))
    {
      return *error;
    }
    if (auto* delivery = std::get_if<Delivery>(&event))
    {
      return std::move(*delivery);
    }
    return Error{ErrorCode::InvalidState, "Expected an available delivery", "lesson.receive"};
  }

  struct SavedQuote
  {
    Message _message;
    BinaryPayloadCodec _codec;
  };

  Result<SavedQuote> run_lesson(const std::filesystem::path& directory)
  {
    TaskDescriptor feed;
    feed._name = "Feed";
    SessionOptions options;
    options._name = "Receive and keep a Quote";
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
    SourceQueueSetDescriptor input;
    input._name = "StrategyInputs";
    input._owner = strategy_id;
    auto register_source_set_result = registry.register_source_set(input);
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
    price._id = FieldId{0xee000001};
    price._name = "PriceTicks";
    price._base = EFieldBase::BASE_INT64;
    price._size = 8;
    auto register_field_result = registry.register_field(price);
    if (const auto* error = std::get_if<Error>(&register_field_result))
    {
      return *error;
    }

    MessageDescriptor quote;
    quote._schema = {MessageType{1}, 1};
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
    auto codec = std::get<0>(std::move(from_registry_result));
    auto open_writer_result = created._creator.open_writer("FeedToStrategy");
    if (const auto* error = std::get_if<Error>(&open_writer_result))
    {
      return *error;
    }
    auto writer = std::get<0>(std::move(open_writer_result));
    PublicationId first_publication;
    for (const auto value : std::array<std::int64_t, 3>{12345, 12346, 12347})
    {
      auto encode_result = codec.encode({value});
      if (const auto* error = std::get_if<Error>(&encode_result))
      {
        return *error;
      }
      const auto bytes = std::get<0>(std::move(encode_result));
      MessageHeader header;
      header._schema = schema;
      header._payloadBytes = bytes.size();
      auto next_publication_id_result = created._creator.next_publication_id();
      if (const auto* error = std::get_if<Error>(&next_publication_id_result))
      {
        return *error;
      }
      const auto publication = std::get<0>(std::move(next_publication_id_result));
      auto send_result = writer.send(publication, MessageView{header, bytes});
      if (const auto* error = std::get_if<Error>(&send_result))
      {
        if (error->_code == ErrorCode::OutcomeUnknown)
        {
          // Stop with this identity available for publication_status; do not
          // replace an uncertain send by publishing under a different ID.
          std::cerr << "Keep publication Task=" << static_cast<unsigned>(publication._producer._task)
                    << " incarnation=" << publication._producer._incarnation
                    << " sequence=" << publication._sequence << " for later lookup.\n";
        }
        return *error;
      }
      const auto outcome = std::get<0>(std::move(send_result));
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
    auto attach_task_result = created._session.attach_task("Strategy");
    if (const auto* error = std::get_if<Error>(&attach_task_result))
    {
      return *error;
    }
    auto consumer = std::get<0>(std::move(attach_task_result));
    auto open_source_set_result = consumer.open_source_set("StrategyInputs");
    if (const auto* error = std::get_if<Error>(&open_source_set_result))
    {
      return *error;
    }
    auto inputs = std::get<0>(std::move(open_source_set_result));
    auto receive_delivery_result = receive_delivery(inputs);
    if (const auto* error = std::get_if<Error>(&receive_delivery_result))
    {
      return *error;
    }
    const auto first = std::get<0>(std::move(receive_delivery_result));
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
    auto copy_message_result = first._message.copy_message(64);
    if (const auto* error = std::get_if<Error>(&copy_message_result))
    {
      return *error;
    }
    auto saved = std::get<0>(std::move(copy_message_result));
    auto release_result = inputs.release(original_token);
    if (const auto* error = std::get_if<Error>(&release_result))
    {
      return *error;
    }

    // first's payload view has expired. Keep using only our owning saved copy.
    auto receive_delivery_result_2 = receive_delivery(inputs);
    if (const auto* error = std::get_if<Error>(&receive_delivery_result_2))
    {
      return *error;
    }
    const auto repeated = std::get<0>(std::move(receive_delivery_result_2));
    if (repeated._token == original_token || repeated._message._header._publication != first_publication)
    {
      throw std::runtime_error("Release did not create a new issuance of the same message");
    }
    auto acknowledge_result = inputs.acknowledge(repeated._token);
    if (const auto* error = std::get_if<Error>(&acknowledge_result))
    {
      return *error;
    }

    std::cout << "Release redelivered the same publication with a new token.\n";

    auto receive_delivery_result_3 = receive_delivery(inputs);
    if (const auto* error = std::get_if<Error>(&receive_delivery_result_3))
    {
      return *error;
    }
    const auto unfinished = std::get<0>(std::move(receive_delivery_result_3));
    const auto unfinished_token = unfinished._token;
    auto close_result = inputs.close();
    if (const auto* error = std::get_if<Error>(&close_result))
    {
      return *error;
    }

    // Explicit close releases sequence 2 without acknowledging it. A new
    // opening gets a different token and can finish the delivery normally.
    auto open_source_set_result_2 = consumer.open_source_set("StrategyInputs");
    if (const auto* error = std::get_if<Error>(&open_source_set_result_2))
    {
      return *error;
    }
    auto reopened = std::get<0>(std::move(open_source_set_result_2));
    auto receive_delivery_result_4 = receive_delivery(reopened);
    if (const auto* error = std::get_if<Error>(&receive_delivery_result_4))
    {
      return *error;
    }
    const auto redelivered = std::get<0>(std::move(receive_delivery_result_4));
    if (redelivered._token == unfinished_token || redelivered._message._header._queueSequence != 2)
    {
      throw std::runtime_error("Closing did not preserve the unfinished message for redelivery");
    }
    auto acknowledge_result_2 = reopened.acknowledge(redelivered._token);
    if (const auto* error = std::get_if<Error>(&acknowledge_result_2))
    {
      return *error;
    }

    std::cout << "Closing an unacknowledged Source allows redelivery.\n";
    auto receive_delivery_result_5 = receive_delivery(reopened);
    if (const auto* error = std::get_if<Error>(&receive_delivery_result_5))
    {
      return *error;
    }
    const auto last = std::get<0>(std::move(receive_delivery_result_5));
    auto acknowledge_result_3 = reopened.acknowledge(last._token);
    if (const auto* error = std::get_if<Error>(&acknowledge_result_3))
    {
      return *error;
    }

    const auto final_receive = reopened.receive();
    if (const auto* error = std::get_if<Error>(&final_receive))
    {
      return *error;
    }
    if (!std::holds_alternative<ReceiveTimeout>(final_receive))
    {
      throw std::runtime_error("Expected timeout after all three messages were acknowledged");
    }
    auto close_result_2 = reopened.close();
    if (const auto* error = std::get_if<Error>(&close_result_2))
    {
      return *error;
    }

    std::cout << "Final receive: timeout.\n";
    if (saved._header != original_header)
    {
      throw std::runtime_error("The owning copy lost delivery header information");
    }
    return SavedQuote{std::move(saved), std::move(codec)};
    // All Session, Task, and endpoint handles close before main decodes this.
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
    auto run_lesson_result = run_lesson(argv[1]);
    if (const auto* error = std::get_if<Error>(&run_lesson_result))
    {
      if (error->_code == ErrorCode::Conflict)
      {
        std::cerr << "Conflict: ";
      }
      std::cerr << "Error " << static_cast<unsigned>(error->_code) << " at "
                << error->_path << ": " << error->_message << '\n';
      return 1;
    }
    const auto saved = std::get<0>(std::move(run_lesson_result));
    auto decode_result = saved._message.decode(saved._codec);
    if (const auto* error = std::get_if<Error>(&decode_result))
    {
      if (error->_code == ErrorCode::Conflict)
      {
        std::cerr << "Conflict: ";
      }
      std::cerr << "Error " << static_cast<unsigned>(error->_code) << " at "
                << error->_path << ": " << error->_message << '\n';
      return 1;
    }
    const auto values = std::get<0>(std::move(decode_result));
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
