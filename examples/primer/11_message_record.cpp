// Put an exact schema and publication identity beside a binary payload.
// This constructs portable records in memory; it does not publish to a Queue.
#include "example_support.h"
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

  PreparedQuote prepare_quote(const std::filesystem::path& directory)
  {
    using namespace angaria;
    TaskDescriptor setup;
    setup._name = "Setup";
    auto created = primer::value(Session::create(
        primer::session_options(directory, "Portable message records"), setup));
    auto registry = created._creator.registry();

    FieldDescriptor price;
    price._id = static_cast<FieldId>(0xee000001);
    price._name = "PriceTicks";
    price._base = EFieldBase::BASE_INT64;
    price._size = 8;
    primer::value(registry.register_field(price));
    FieldDescriptor quantity;
    quantity._id = static_cast<FieldId>(0xee000002);
    quantity._name = "Quantity";
    quantity._base = EFieldBase::BASE_UINT64;
    quantity._size = 8;
    primer::value(registry.register_field(quantity));

    MessageDescriptor quote;
    quote._schema = {static_cast<MessageType>(1), 1};
    quote._name = "Quote";
    quote._fields = {price._id, quantity._id};
    const auto schema = primer::value(registry.register_message(quote));
    auto codec = primer::value(BinaryPayloadCodec::from_registry(
        created._session.registry(), schema, 16));

    MessageHeader header;
    header._schema = schema;
    header._format = PayloadFormat::Binary;
    // Issuing an identity persists a producer sequence. It does not accept or
    // send a publication; the record codec itself checks only identity shape.
    header._publication = primer::value(created._creator.next_publication_id());
    // A fixed caller-supplied timestamp makes this lesson's output repeatable.
    header._publicationTime = UtcTimestamp{std::chrono::nanoseconds{42}};
    header._flags = 0x80000001u;
    header._payloadBytes = codec.payload_size();
    return PreparedQuote{std::move(codec), header, created._session.identity()};
    // All Session, Registry, and Task handles close here. The codec owns its
    // definitions, and the header owns values rather than borrowed handles.
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
    using namespace angaria;
    const auto prepared = prepare_quote(argv[1]);
    const auto payload = primer::value(prepared._codec.encode({
        std::int64_t{-2}, std::uint64_t{7}}));
    constexpr std::uint64_t record_limit = 96;
    MessageRecord retained;
    {
      const auto bytes = primer::value(MessageRecordCodec::encode(
          prepared._header, payload, record_limit));
      // Header decoding checks the announced size under the complete record
      // budget. Full decoding then verifies that all payload bytes are present.
      const auto header = primer::value(MessageRecordCodec::decode_header(
          std::span{bytes}.first(MessageRecordCodec::header_bytes), record_limit));
      const auto borrowed = primer::value(MessageRecordCodec::decode_view(bytes, record_limit));
      // Schema IDs are local to a Session. Use the independently retained
      // source identity, rather than trusting the Session ID inside the record.
      const auto values = primer::value(MessageRecordCodec::decode_binary(
          borrowed, prepared._codec, prepared._schemaSession));
      std::cout << "Message record bytes: " << bytes.size() << '\n'
                << "Header bytes: " << MessageRecordCodec::header_bytes
                << " Payload bytes: " << header._payloadBytes << '\n'
                << "Schema: Quote type=" << static_cast<unsigned>(header._schema._type)
                << " version=" << header._schema._version << '\n'
                << "Producer: Task=" << static_cast<unsigned>(header._publication._producer._task)
                << " incarnation=" << header._publication._producer._incarnation
                << " sequence=" << header._publication._sequence << '\n'
                << "Publication time (demo): " << header._publicationTime.time_since_epoch().count() << " ns\n"
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
      if (!schema_error || schema_error->_code != ErrorCode::SchemaConflict)
      {
        throw std::runtime_error("A different exact schema must fail with SchemaConflict");
      }
      const auto too_small = MessageRecordCodec::encode(prepared._header, payload, record_limit - 1);
      const auto* limit_error = std::get_if<Error>(&too_small);
      if (!limit_error || limit_error->_code != ErrorCode::CapacityExceeded)
      {
        throw std::runtime_error("The record budget must include both header and payload");
      }
      std::cout << "Wrong schema version: " << primer::error_name(schema_error->_code) << '\n'
                << "Record cap 95: " << primer::error_name(limit_error->_code) << '\n';
      retained = primer::value(MessageRecordCodec::decode(bytes, record_limit));
      // borrowed._payload refers to bytes. It cannot be used after this scope.
    }
    const auto own_view = retained.view();
    const auto raw_message = own_view.message();
    const auto values = primer::value(MessageRecordCodec::decode_binary(
        own_view, prepared._codec, prepared._schemaSession));
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
