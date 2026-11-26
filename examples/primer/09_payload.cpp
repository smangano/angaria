// Turn registered scalar Fields into portable bytes, then decode those bytes.
// These are local payload operations; no Queue or publication is involved.
#include "angaria/runtime.h"
#include "angaria/payload.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <span>
#include <utility>
#include <variant>
#include <vector>

namespace
{
  struct QuoteCodecs
  {
    angaria::BinaryPayloadCodec _versionOne;
    angaria::BinaryPayloadCodec _versionTwo;
  };

  angaria::Result<QuoteCodecs> configure_quotes(const std::filesystem::path& directory)
  {
    using namespace angaria;
    TaskDescriptor setup;
    setup._name = "Setup";
    SessionOptions options;
    options._name = "Binary quote payloads";
    options._directory = directory;
    options._maxLogBytes = 1024 * 1024;
    options._limits = CapacityLimits::empty().tasks(8).queues(8)
        .source_sets(4).publishing_sets(4).memberships_per_set(4)
        .fields(16).message_schemas(8);
    options._defaults._ringCapacity = 8;
    auto create_result = Session::create(
        options, setup);
    if (const auto* error = std::get_if<angaria::Error>(&create_result))
    {
      return *error;
    }
    auto created = std::get<angaria::CreatedSession>(std::move(create_result));
    auto registry = created._creator.registry();

    FieldDescriptor price;
    price._id = static_cast<FieldId>(0xee000001);
    price._name = "PriceTicks";
    price._base = EFieldBase::BASE_INT64;
    price._size = 8;
    auto register_field_result = registry.register_field(price);
    if (const auto* error = std::get_if<angaria::Error>(&register_field_result))
    {
      return *error;
    }
    FieldDescriptor quantity;
    quantity._id = static_cast<FieldId>(0xee000002);
    quantity._name = "Quantity";
    quantity._base = EFieldBase::BASE_UINT64;
    quantity._size = 8;
    auto register_field_result_2 = registry.register_field(quantity);
    if (const auto* error = std::get_if<angaria::Error>(&register_field_result_2))
    {
      return *error;
    }
    FieldDescriptor instrument;
    instrument._id = static_cast<FieldId>(0xee000003);
    instrument._name = "Instrument";
    instrument._base = EFieldBase::BASE_UINT32;
    instrument._size = 4;
    auto register_field_result_3 = registry.register_field(instrument);
    if (const auto* error = std::get_if<angaria::Error>(&register_field_result_3))
    {
      return *error;
    }

    MessageDescriptor quote;
    quote._schema = {static_cast<MessageType>(1), 1};
    quote._name = "Quote";
    quote._fields = {price._id, quantity._id};
    auto register_message_result = registry.register_message(quote);
    if (const auto* error = std::get_if<angaria::Error>(&register_message_result))
    {
      return *error;
    }
    const auto first_schema = std::get<angaria::SchemaId>(std::move(register_message_result));
    constexpr std::uint64_t payload_limit = 64;
    auto from_registry_result = BinaryPayloadCodec::from_registry(
        created._session.registry(), first_schema, payload_limit);
    if (const auto* error = std::get_if<angaria::Error>(&from_registry_result))
    {
      return *error;
    }
    auto first = std::get<angaria::BinaryPayloadCodec>(std::move(from_registry_result));

    // A new immutable version adds a Field. The earlier codec keeps version 1
    // and its original two-Field layout after version 2 enters the Registry.
    quote._schema._version = 2;
    quote._fields.push_back(instrument._id);
    auto register_message_result_2 = registry.register_message(quote);
    if (const auto* error = std::get_if<angaria::Error>(&register_message_result_2))
    {
      return *error;
    }
    const auto second_schema = std::get<angaria::SchemaId>(std::move(register_message_result_2));
    auto from_registry_result_2 = BinaryPayloadCodec::from_registry(
        created._session.registry(), second_schema, payload_limit);
    if (const auto* error = std::get_if<angaria::Error>(&from_registry_result_2))
    {
      return *error;
    }
    auto second = std::get<angaria::BinaryPayloadCodec>(std::move(from_registry_result_2));
    return QuoteCodecs{std::move(first), std::move(second)};
    // Session and Task handles close here. Each codec owns its definitions.
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
    auto configured = configure_quotes(argv[1]);
    if (const auto* error = std::get_if<angaria::Error>(&configured))
    {
      if (error->_code == angaria::ErrorCode::Conflict)
      {
        std::cerr << "Conflict: ";
      }
      std::cerr << "Angaria error " << static_cast<int>(error->_code)
                << " at " << error->_path << ": " << error->_message << '\n';
      return 1;
    }
    const auto codecs = std::get<QuoteCodecs>(std::move(configured));
    // ScalarValue keeps the exact C++ type. A uint64_t value must match a
    // uint64 Field; the codec does not silently convert another integer type.
    const std::array<ScalarValue, 2> first_values{std::int64_t{-2}, std::uint64_t{7}};
    auto encode_result = codecs._versionOne.encode(first_values);
    if (const auto* error = std::get_if<angaria::Error>(&encode_result))
    {
      std::cerr << "Angaria error " << static_cast<int>(error->_code)
                << " at " << error->_path << ": " << error->_message << '\n';
      return 1;
    }
    const auto first_payload = std::get<std::vector<std::byte>>(std::move(encode_result));
    auto decode_result = codecs._versionOne.decode(first_payload);
    if (const auto* error = std::get_if<angaria::Error>(&decode_result))
    {
      std::cerr << "Angaria error " << static_cast<int>(error->_code)
                << " at " << error->_path << ": " << error->_message << '\n';
      return 1;
    }
    const auto first_decoded = std::get<std::vector<angaria::PayloadValue>>(std::move(decode_result));
    std::cout << "Quote v1 bytes: " << first_payload.size() << '\n'
              << "Quote v1 hex: ";
    // This is ordinary stream formatting, not another Angaria operation.
    for (std::size_t index = 0; index < first_payload.size(); ++index)
    {
      if (index != 0)
      {
        std::cout << ' ';
      }
      std::cout << std::hex << std::setfill('0') << std::setw(2)
                << std::to_integer<unsigned>(first_payload[index]);
    }
    std::cout << std::dec << '\n';
    std::cout << "Decoded v1: PriceTicks=" << std::get<std::int64_t>(first_decoded[0])
              << " Quantity=" << std::get<std::uint64_t>(first_decoded[1]) << '\n';

    const std::array<ScalarValue, 3> second_values{
        std::int64_t{-2}, std::uint64_t{7}, std::uint32_t{42}};
    auto encode_result_2 = codecs._versionTwo.encode(second_values);
    if (const auto* error = std::get_if<angaria::Error>(&encode_result_2))
    {
      std::cerr << "Angaria error " << static_cast<int>(error->_code)
                << " at " << error->_path << ": " << error->_message << '\n';
      return 1;
    }
    const auto second_payload = std::get<std::vector<std::byte>>(std::move(encode_result_2));
    auto decode_result_2 = codecs._versionTwo.decode(second_payload);
    if (const auto* error = std::get_if<angaria::Error>(&decode_result_2))
    {
      std::cerr << "Angaria error " << static_cast<int>(error->_code)
                << " at " << error->_path << ": " << error->_message << '\n';
      return 1;
    }
    const auto second_decoded = std::get<std::vector<angaria::PayloadValue>>(std::move(decode_result_2));
    std::cout << "Quote v2 bytes: " << second_payload.size() << '\n'
              << "Decoded v2: PriceTicks=" << std::get<std::int64_t>(second_decoded[0])
              << " Quantity=" << std::get<std::uint64_t>(second_decoded[1])
              << " Instrument=" << std::get<std::uint32_t>(second_decoded[2]) << '\n'
              << "Retained codec version: " << codecs._versionOne.schema()._version << '\n'
              << "Codecs remain usable after Session handles close.\n"
              << "Payload encoding only: no messages were sent.\n";
    return 0;
  }
  catch (const std::exception& error)
  {
    std::cerr << argv[1] << ": " << error.what() << '\n';
    return 1;
  }
}
