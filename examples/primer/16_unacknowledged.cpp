// Receive recorded messages without a processing acknowledgment, preserve an
// owning copy, and see why an unacknowledged replacement may deliver again.
#include "angaria/runtime.h"
#include "angaria/payload.h"

#include <cstdint>
#include <exception>
#include <filesystem>
#include <iostream>
#include <utility>
#include <variant>
#include <vector>

namespace
{
  using namespace angaria;

  Result<std::pair<Message, BinaryPayloadCodec>> run_lesson(const std::filesystem::path& configuration)
  {
    auto creation = Session::create_from_file(configuration, "Feed");
    if (const auto* error = std::get_if<Error>(&creation))
    {
      return *error;
    }
    auto created = std::get<CreatedSession>(std::move(creation));
    auto registry = created._session.registry();
    auto source_identity = registry.find_source_set("ConsumerInputs");
    if (const auto* error = std::get_if<Error>(&source_identity))
    {
      return *error;
    }
    auto source_definition = registry.describe_source_set(std::get<SourceQueueSetId>(source_identity));
    if (const auto* error = std::get_if<Error>(&source_definition))
    {
      return *error;
    }
    if (std::get<SourceQueueSetDescriptor>(source_definition)._acknowledgment != AcknowledgmentMode::None)
    {
      return Error{ErrorCode::InvalidConfiguration, "This lesson requires acknowledgment = none", "source_sets.ConsumerInputs"};
    }
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
    auto codec = std::get<BinaryPayloadCodec>(std::move(codec_result));
    auto writer_result = created._creator.open_writer("Ticks");
    if (const auto* error = std::get_if<Error>(&writer_result))
    {
      return *error;
    }
    auto writer = std::get<QueueWriter>(std::move(writer_result));
    auto encoded = codec.encode({std::int64_t{12345}});
    if (const auto* error = std::get_if<Error>(&encoded))
    {
      return *error;
    }
    const auto bytes = std::get<std::vector<std::byte>>(std::move(encoded));
    auto issued = created._creator.next_publication_id();
    if (const auto* error = std::get_if<Error>(&issued))
    {
      return *error;
    }
    const auto publication = std::get<PublicationId>(issued);
    MessageHeader header;
    header._schema = schema;
    header._payloadBytes = bytes.size();
    auto sent = writer.send(publication, MessageView{header, bytes});
    if (const auto* error = std::get_if<Error>(&sent))
    {
      // An uncertain call can already have committed. Resolve its original ID
      // rather than issuing a second publication for the same application work.
      if (error->_code != ErrorCode::OutcomeUnknown)
      {
        return *error;
      }
      sent = created._creator.publication_status(publication);
      if (const auto* lookup_error = std::get_if<Error>(&sent))
      {
        return *lookup_error;
      }
    }
    const auto accepted = std::get<PublicationOutcome>(std::move(sent));
    if (accepted._status != AcceptanceStatus::Accepted)
    {
      return Error{ErrorCode::InvalidState, "Expected the first Tick to fit its Queue", "lesson.send"};
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
    auto inputs = std::get<SourceQueueSet>(std::move(opening));
    auto event = inputs.receive();
    if (const auto* error = std::get_if<Error>(&event))
    {
      return *error;
    }
    const auto* received = std::get_if<MessageView>(&event);
    if (!received || received->_header._publication != publication || received->_header._queueSequence != 1)
    {
      return Error{ErrorCode::InvalidState, "Expected an unacknowledged MessageView for Queue sequence 1", "lesson.receive"};
    }
    const auto observed_header = received->_header;
    auto copying = received->copy_message(8);
    if (const auto* error = std::get_if<Error>(&copying))
    {
      return *error;
    }
    auto saved = std::get<Message>(std::move(copying));
    std::cout << "Configured acknowledgment: none; accepted messages remain recorded.\n"
              << "Unacknowledged receive: Queue sequence 1.\n"
              << "A MessageView has no acknowledgment token.\n";
    auto closing = inputs.close();
    if (const auto* error = std::get_if<Error>(&closing))
    {
      return *error;
    }
    // The old borrow is now invalid. saved owns independent bytes, the exact
    // schema context and the complete observed MessageHeader.
    if (saved._header != observed_header || saved._payload != bytes)
    {
      return Error{ErrorCode::InvalidState, "The owning copy changed its header or payload", "lesson.copy"};
    }
    std::cout << "Owning copy preserves every header field.\n";
    auto reopening = consumer.open_source_set("ConsumerInputs");
    if (const auto* error = std::get_if<Error>(&reopening))
    {
      return *error;
    }
    auto replacement = std::get<SourceQueueSet>(std::move(reopening));
    auto replay = replacement.receive();
    if (const auto* error = std::get_if<Error>(&replay))
    {
      return *error;
    }
    const auto* repeated = std::get_if<MessageView>(&replay);
    if (!repeated || repeated->_header._publication != publication || repeated->_header._queueSequence != 1)
    {
      return Error{ErrorCode::InvalidState, "Unacknowledged replacement lost recorded replay", "lesson.replay"};
    }
    auto closed = replacement.close();
    if (const auto* error = std::get_if<Error>(&closed))
    {
      return *error;
    }
    std::cout << "Reopening can replay the same unacknowledged publication.\n";
    return std::pair<Message, BinaryPayloadCodec>{std::move(saved), std::move(codec)};
    // Every Session, Task and endpoint closes before main uses the saved copy.
  }
}

int main(int argc, char** argv)
{
  using namespace angaria;
  if (argc != 2)
  {
    std::cerr << "Usage: " << argv[0] << " <unacknowledged.toml>\n";
    return 1;
  }
  try
  {
    auto result = run_lesson(argv[1]);
    if (const auto* error = std::get_if<Error>(&result))
    {
      if (error->_code == ErrorCode::Conflict)
      {
        std::cerr << "Conflict: ";
      }
      std::cerr << error->_path << ": " << error->_message << " (code " << static_cast<unsigned>(error->_code) << ")\n";
      return 1;
    }
    auto [saved, codec] = std::get<0>(std::move(result));
    auto decoded = saved.decode(codec);
    if (const auto* error = std::get_if<Error>(&decoded))
    {
      std::cerr << error->_path << ": " << error->_message << '\n';
      return 1;
    }
    const auto values = std::get<0>(std::move(decoded));
    std::cout << "Saved copy after all handles close: PriceTicks=" << std::get<std::int64_t>(values.front()) << '\n';
    return 0;
  }
  catch (const std::exception& error)
  {
    std::cerr << argv[1] << ": " << error.what() << '\n';
    return 1;
  }
}
