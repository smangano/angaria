// Put an exact schema and publication identity beside a binary payload.
// This constructs portable records in memory; it does not publish to a Queue.
#include "angaria/runtime.h"
#include "angaria/message_record.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <iostream>
#include <span>
#include <stdexcept>
#include <utility>
#include <variant>

namespace
{
  struct PreparedQuote
  {
    angaria::BinaryPayloadCodec _codec;
    angaria::MessageHeader _header;
    angaria::SessionId _schemaSession;
  };

  angaria::Result<PreparedQuote> prepare_quote(const std::filesystem::path& directory)
  {
    using namespace angaria;
    TaskDescriptor setup;
    setup._name = "Setup";
    SessionOptions options;
    options._name = "Portable message records";
    options._directory = directory;
    options._maxLogBytes = 1024 * 1024;
    options._limits = CapacityLimits::empty().tasks(8).queues(8)
        .source_sets(4).publishing_sets(4).memberships_per_set(4)
        .fields(16).message_schemas(8);
    options._defaults._ringCapacity = 8;
    auto create_result = Session::create(
        options, setup);
    if (const auto* error = std::get_if<Error>(&create_result))
    {
      return *error;
    }
    auto created = std::get<0>(std::move(create_result));
    auto registry = created._creator.registry();

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

    FieldDescriptor quantity;
    quantity._id = static_cast<FieldId>(0xee000002);
    quantity._name = "Quantity";
    quantity._base = EFieldBase::BASE_UINT64;
    quantity._size = 8;
    auto register_field_result_2 = registry.register_field(quantity);
    if (const auto* error = std::get_if<Error>(&register_field_result_2))
    {
      return *error;
    }

    MessageDescriptor quote;
    quote._schema = {static_cast<MessageType>(1), 1};
    quote._name = "Quote";
    quote._fields = {price._id, quantity._id};
    auto register_message_result = registry.register_message(quote);
    if (const auto* error = std::get_if<Error>(&register_message_result))
    {
      return *error;
    }
    const auto schema = std::get<0>(std::move(register_message_result));
    auto from_registry_result = BinaryPayloadCodec::from_registry(
        created._session.registry(), schema, 16);
    if (const auto* error = std::get_if<Error>(&from_registry_result))
    {
      return *error;
    }
    auto codec = std::get<0>(std::move(from_registry_result));

    MessageHeader header;
    header._schema = schema;
    header._format = PayloadFormat::Binary;
    // Issuing an identity persists a producer sequence. It does not accept or
    // send a publication; the record codec itself checks only identity shape.
    auto next_publication_id_result = created._creator.next_publication_id();
    if (const auto* error = std::get_if<Error>(&next_publication_id_result))
    {
      return *error;
    }
    header._publication = std::get<0>(std::move(next_publication_id_result));
    // A fixed caller-supplied timestamp makes this lesson's output repeatable.
    header._publishedAt = UtcTimestamp{std::chrono::nanoseconds{42}};
    header._flags = 0x80000001u;
    header._payloadBytes = codec.payload_size();
    return PreparedQuote{std::move(codec), header, created._session.identity()};
    // All Session, Registry, and Task handles close here. The codec owns its
    // definitions, and the header owns values rather than borrowed handles.
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
    auto prepare_quote_result = prepare_quote(argv[1]);
    if (const auto* error = std::get_if<Error>(&prepare_quote_result))
    {
      if (error->_code == ErrorCode::Conflict)
      {
        std::cerr << "Conflict: ";
      }
      std::cerr << "Error " << static_cast<unsigned>(error->_code) << " at "
                << error->_path << ": " << error->_message << '\n';
      return 1;
    }
    const auto prepared = std::get<0>(std::move(prepare_quote_result));
    auto encode_result = prepared._codec.encode({
        std::int64_t{-2}, std::uint64_t{7}});
    if (const auto* error = std::get_if<Error>(&encode_result))
    {
      if (error->_code == ErrorCode::Conflict)
      {
        std::cerr << "Conflict: ";
      }
      std::cerr << "Error " << static_cast<unsigned>(error->_code) << " at "
                << error->_path << ": " << error->_message << '\n';
      return 1;
    }
    const auto payload = std::get<0>(std::move(encode_result));
    constexpr std::uint64_t record_limit = 96;
    MessageRecord retained;
    {
      auto encode_result_2 = MessageRecordCodec::encode(
          prepared._header, payload, record_limit);
      if (const auto* error = std::get_if<Error>(&encode_result_2))
      {
        if (error->_code == ErrorCode::Conflict)
        {
          std::cerr << "Conflict: ";
        }
        std::cerr << "Error " << static_cast<unsigned>(error->_code) << " at "
                << error->_path << ": " << error->_message << '\n';
        return 1;
      }
      const auto bytes = std::get<0>(std::move(encode_result_2));
      // Header decoding checks the announced size under the complete record
      // budget. Full decoding then verifies that all payload bytes are present.
      auto decode_header_result = MessageRecordCodec::decode_header(
          std::span{bytes}.first(MessageRecordCodec::header_bytes), record_limit);
      if (const auto* error = std::get_if<Error>(&decode_header_result))
      {
        if (error->_code == ErrorCode::Conflict)
        {
          std::cerr << "Conflict: ";
        }
        std::cerr << "Error " << static_cast<unsigned>(error->_code) << " at "
                << error->_path << ": " << error->_message << '\n';
        return 1;
      }
      const auto header = std::get<0>(std::move(decode_header_result));
      auto decode_view_result = MessageRecordCodec::decode_view(bytes, record_limit);
      if (const auto* error = std::get_if<Error>(&decode_view_result))
      {
        if (error->_code == ErrorCode::Conflict)
        {
          std::cerr << "Conflict: ";
        }
        std::cerr << "Error " << static_cast<unsigned>(error->_code) << " at "
                << error->_path << ": " << error->_message << '\n';
        return 1;
      }
      const auto borrowed = std::get<0>(std::move(decode_view_result));
      // Schema IDs are local to a Session. Use the independently retained
      // source identity, rather than trusting the Session ID inside the record.
      auto decode_binary_result = MessageRecordCodec::decode_binary(
          borrowed, prepared._codec, prepared._schemaSession);
      if (const auto* error = std::get_if<Error>(&decode_binary_result))
      {
        if (error->_code == ErrorCode::Conflict)
        {
          std::cerr << "Conflict: ";
        }
        std::cerr << "Error " << static_cast<unsigned>(error->_code) << " at "
                << error->_path << ": " << error->_message << '\n';
        return 1;
      }
      const auto values = std::get<0>(std::move(decode_binary_result));
      std::cout << "Message record bytes: " << bytes.size() << '\n'
                << "Header bytes: " << MessageRecordCodec::header_bytes
                << " Payload bytes: " << header._payloadBytes << '\n'
                << "Schema: Quote type=" << static_cast<unsigned>(header._schema._type)
                << " version=" << header._schema._version << '\n'
                << "Producer: Task=" << static_cast<unsigned>(header._publication._producer._task)
                << " incarnation=" << header._publication._producer._incarnation
                << " sequence=" << header._publication._sequence << '\n'
                << "Publication time (demo): " << header._publishedAt.time_since_epoch().count() << " ns\n"
                << "User flags: " << header._flags << '\n'
                << "Decoded Quote: PriceTicks=" << std::get<std::int64_t>(values[0])
                << " Quantity=" << std::get<std::uint64_t>(values[1]) << '\n';

      // A same-sized payload cannot establish its own schema version. This
      // deliberately altered view must fail before the v1 codec decodes it.
      auto wrong_version = borrowed;
      wrong_version._header._schema._version = 2;
      const auto mismatch = MessageRecordCodec::decode_binary(
          wrong_version, prepared._codec, prepared._schemaSession);
      const auto* schema_error = std::get_if<Error>(&mismatch);
      if (!schema_error)
      {
        std::cerr << "Expected SchemaConflict for a different exact schema.\n";
        return 1;
      }
      if (schema_error->_code != ErrorCode::SchemaConflict)
      {
        std::cerr << "Error " << static_cast<unsigned>(schema_error->_code) << " at "
                  << schema_error->_path << ": " << schema_error->_message << '\n';
        return 1;
      }
      const auto too_small = MessageRecordCodec::encode(prepared._header, payload, record_limit - 1);
      const auto* limit_error = std::get_if<Error>(&too_small);
      if (!limit_error)
      {
        std::cerr << "Expected CapacityExceeded when the record budget is too small.\n";
        return 1;
      }
      if (limit_error->_code != ErrorCode::CapacityExceeded)
      {
        std::cerr << "Error " << static_cast<unsigned>(limit_error->_code) << " at "
                  << limit_error->_path << ": " << limit_error->_message << '\n';
        return 1;
      }
      std::cout << "Wrong schema version: SchemaConflict\n"
                << "Record cap 95: CapacityExceeded\n";
      auto decode_result = MessageRecordCodec::decode(bytes, record_limit);
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
      retained = std::get<0>(std::move(decode_result));
      // borrowed._payload refers to bytes. It cannot be used after this scope.
    }
    const auto own_view = retained.view();
    const auto raw_message = own_view.message();
    auto decode_binary_result_2 = MessageRecordCodec::decode_binary(
        own_view, prepared._codec, prepared._schemaSession);
    if (const auto* error = std::get_if<Error>(&decode_binary_result_2))
    {
      if (error->_code == ErrorCode::Conflict)
      {
        std::cerr << "Conflict: ";
      }
      std::cerr << "Error " << static_cast<unsigned>(error->_code) << " at "
                << error->_path << ": " << error->_message << '\n';
      return 1;
    }
    const auto values = std::get<0>(std::move(decode_binary_result_2));
    if (std::get<std::int64_t>(values[0]) != -2 || std::get<std::uint64_t>(values[1]) != 7)
    {
      throw std::runtime_error("The owning record must retain its payload after input bytes close");
    }
    std::cout << "Borrowed MessageView payload bytes: " << raw_message._payload.size() << '\n'
              << "Owning record survives input buffer release: yes\n"
              << "Record and payload codecs remain usable after Session handles close.\n"
              << "Record encoding only: no messages were sent.\n";
    return 0;
  }
  catch (const std::exception& error)
  {
    std::cerr << argv[1] << ": " << error.what() << '\n';
    return 1;
  }
}
