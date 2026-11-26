// Describe logical values in TOML, then encode them with their exact meanings.
// No Queue is needed for these local payload and validation operations.
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
#include <stdexcept>
#include <utility>
#include <variant>
#include <vector>

namespace
{
  angaria::Result<angaria::BinaryPayloadCodec> configure_orders(const std::filesystem::path& configuration)
  {
    auto create_from_file_result = angaria::Session::create_from_file(configuration, "Setup");
    if (const auto* error = std::get_if<angaria::Error>(&create_from_file_result))
    {
      return *error;
    }
    auto created = std::get<angaria::CreatedSession>(std::move(create_from_file_result));
    auto registry = created._session.registry();
    auto find_message_result = registry.find_message("OrderSettings", 1);
    if (const auto* error = std::get_if<angaria::Error>(&find_message_result))
    {
      return *error;
    }
    const auto schema = std::get<angaria::SchemaId>(std::move(find_message_result));
    constexpr std::uint64_t payload_limit = 64;
    auto from_registry_result = angaria::BinaryPayloadCodec::from_registry(registry, schema, payload_limit);
    if (const auto* error = std::get_if<angaria::Error>(&from_registry_result))
    {
      return *error;
    }
    return std::get<angaria::BinaryPayloadCodec>(std::move(from_registry_result));
    // Every Session, Registry, and Task handle closes here. The returned codec
    // owns the exact schema and its logical metadata, including enum labels.
  }

}

int main(int argc, char** argv)
{
  if (argc != 2)
  {
    std::cerr << "Usage: " << argv[0] << " <logical-fields.toml>\n";
    return 1;
  }
  try
  {
    using namespace angaria;
    auto configured = configure_orders(argv[1]);
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
    const auto codec = std::get<angaria::BinaryPayloadCodec>(std::move(configured));
    if (codec.fields().size() != 5)
    {
      throw std::runtime_error("This lesson needs the five-Field OrderSettings schema from logical-fields.toml");
    }
    std::cout << "Registered logical Fields: " << codec.fields().size() << '\n'
              << "Side labels:";
    for (const auto& entry : codec.fields()[2]._logical._enumerators)
    {
      std::cout << ' ' << std::get<std::uint64_t>(entry._value) << '=' << entry._name;
    }
    std::cout << '\n';

    // These wrappers distinguish a bit field and an enum from an ordinary
    // integer. The enum's uint8_t alternative matches its byte storage base.
    // ASCII padding is supplied explicitly; the codec does not invent it.
    auto encode_result = codec.encode({
        true, BitFieldValue{0x8001}, EnumValue{std::uint8_t{1}},
        FixedAsciiValue{"IBM   "}, std::int8_t{-128}});
    if (const auto* error = std::get_if<angaria::Error>(&encode_result))
    {
      std::cerr << "Angaria error " << static_cast<int>(error->_code)
                << " at " << error->_path << ": " << error->_message << '\n';
      return 1;
    }
    const auto payload = std::get<std::vector<std::byte>>(std::move(encode_result));
    auto decode_result = codec.decode(payload);
    if (const auto* error = std::get_if<angaria::Error>(&decode_result))
    {
      std::cerr << "Angaria error " << static_cast<int>(error->_code)
                << " at " << error->_path << ": " << error->_message << '\n';
      return 1;
    }
    const auto decoded = std::get<std::vector<angaria::PayloadValue>>(std::move(decode_result));
    std::cout << "OrderSettings payload bytes: " << payload.size() << '\n'
              << "OrderSettings hex: ";
    // This is ordinary stream formatting, not another Angaria operation.
    for (std::size_t index = 0; index < payload.size(); ++index)
    {
      if (index != 0)
      {
        std::cout << ' ';
      }
      std::cout << std::hex << std::setfill('0') << std::setw(2)
                << std::to_integer<unsigned>(payload[index]);
    }
    std::cout << std::dec << '\n';
    std::cout << "Decoded: Enabled=" << (std::get<bool>(decoded[0]) ? "true" : "false")
              << " Flags=" << std::get<BitFieldValue>(decoded[1])._value
              << " Side=" << static_cast<unsigned>(std::get<std::uint8_t>(std::get<EnumValue>(decoded[2])._value))
              << " Instrument='" << std::get<FixedAsciiValue>(decoded[3])._text << '\''
              << " Adjustment=" << static_cast<int>(std::get<std::int8_t>(decoded[4])) << '\n';

    // A caller can also supply the complete output buffer. All values must
    // pass validation before encode_into replaces any byte in that buffer.
    std::array<PayloadValue, 5> values{
        true, BitFieldValue{0x8001}, EnumValue{std::uint8_t{3}},
        FixedAsciiValue{"IBM   "}, std::int8_t{-128}};
    std::vector<std::byte> output(payload.size(), std::byte{0xa5});
    const auto original = output;
    const auto rejected = codec.encode_into(values, output);
    const auto* error = std::get_if<Error>(&rejected);
    if (!error)
    {
      std::cerr << "An undeclared enum unexpectedly encoded successfully.\n";
      return 1;
    }
    if (error->_code != ErrorCode::InvalidArgument)
    {
      // Keep an unexpected framework failure intact instead of reporting it
      // as the InvalidArgument that this particular scenario anticipated.
      std::cerr << "Angaria error " << static_cast<int>(error->_code)
                << " at " << error->_path << ": " << error->_message << '\n';
      return 1;
    }
    if (output != original)
    {
      std::cerr << "Rejected enum encoding changed the caller's output buffer.\n";
      return 1;
    }
    std::cout << "Rejected undeclared enum: InvalidArgument\n"
              << "Caller buffer unchanged: yes\n";
    values[2] = EnumValue{std::uint8_t{1}};
    auto encode_into_result = codec.encode_into(values, output);
    if (const auto* error = std::get_if<angaria::Error>(&encode_into_result))
    {
      std::cerr << "Angaria error " << static_cast<int>(error->_code)
                << " at " << error->_path << ": " << error->_message << '\n';
      return 1;
    }
    if (output != payload)
    {
      throw std::runtime_error("Caller-buffer encoding disagrees with owning encoding");
    }
    std::cout << "Caller buffer matches owning payload: yes\n"
              << "Codec remains usable after Session handles close.\n"
              << "Payload encoding only: no messages were sent.\n";
    return 0;
  }
  catch (const std::exception& error)
  {
    std::cerr << argv[1] << ": " << error.what() << '\n';
    return 1;
  }
}
