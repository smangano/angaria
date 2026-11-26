// Turn registered scalar Fields into portable bytes, then decode those bytes.
// These are local payload operations; no Queue or publication is involved.
#include "example_support.h"
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

namespace
{
  struct QuoteCodecs
  {
    angaria::BinaryPayloadCodec _versionOne;
    angaria::BinaryPayloadCodec _versionTwo;
  };

  QuoteCodecs configure_quotes(const std::filesystem::path& directory)
  {
    using namespace angaria;
    TaskDescriptor setup;
    setup._name = "Setup";
    auto created = primer::value(Session::create(
        primer::session_options(directory, "Binary quote payloads"), setup));
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
    FieldDescriptor instrument;
    instrument._id = static_cast<FieldId>(0xee000003);
    instrument._name = "Instrument";
    instrument._base = EFieldBase::BASE_UINT32;
    instrument._size = 4;
    primer::value(registry.register_field(instrument));

    MessageDescriptor quote;
    quote._schema = {static_cast<MessageType>(1), 1};
    quote._name = "Quote";
    quote._fields = {price._id, quantity._id};
    const auto first_schema = primer::value(registry.register_message(quote));
    constexpr std::uint64_t payload_limit = 64;
    auto first = primer::value(BinaryPayloadCodec::from_registry(
        created._session.registry(), first_schema, payload_limit));

    // A new immutable version adds a Field. The earlier codec keeps version 1
    // and its original two-Field layout after version 2 enters the Registry.
    quote._schema._version = 2;
    quote._fields.push_back(instrument._id);
    const auto second_schema = primer::value(registry.register_message(quote));
    auto second = primer::value(BinaryPayloadCodec::from_registry(
        created._session.registry(), second_schema, payload_limit));
    return QuoteCodecs{std::move(first), std::move(second)};
    // Session and Task handles close here. Each codec owns its definitions.
  }

  void print_hex(std::span<const std::byte> bytes)
  {
    for (std::size_t index = 0; index < bytes.size(); ++index)
    {
      if (index != 0)
      {
        std::cout << ' ';
      }
      std::cout << std::hex << std::setfill('0') << std::setw(2)
                << std::to_integer<unsigned>(bytes[index]);
    }
    std::cout << std::dec << std::setfill(' ') << '\n';
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
    const auto codecs = configure_quotes(argv[1]);
    // ScalarValue keeps the exact C++ type. A uint64_t value must match a
    // uint64 Field; the codec does not silently convert another integer type.
    const std::array<ScalarValue, 2> first_values{std::int64_t{-2}, std::uint64_t{7}};
    const auto first_payload = primer::value(codecs._versionOne.encode(first_values));
    const auto first_decoded = primer::value(codecs._versionOne.decode(first_payload));
    std::cout << "Quote v1 bytes: " << first_payload.size() << '\n'
              << "Quote v1 hex: ";
    print_hex(first_payload);
    std::cout << "Decoded v1: PriceTicks=" << std::get<std::int64_t>(first_decoded[0])
              << " Quantity=" << std::get<std::uint64_t>(first_decoded[1]) << '\n';

    const std::array<ScalarValue, 3> second_values{
        std::int64_t{-2}, std::uint64_t{7}, std::uint32_t{42}};
    const auto second_payload = primer::value(codecs._versionTwo.encode(second_values));
    const auto second_decoded = primer::value(codecs._versionTwo.decode(second_payload));
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
