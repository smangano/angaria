// Describe logical values in TOML, then encode them with their exact meanings.
// No Queue is needed for these local payload and validation operations.
#include "example_support.h"
#include "example_formatting.h"
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
#include <variant>
#include <vector>

namespace
{
  angaria::BinaryPayloadCodec configure_orders(const std::filesystem::path& configuration)
  {
    auto created = primer::value(angaria::Session::create_from_file(configuration, "Setup"));
    auto registry = created._session.registry();
    const auto schema = primer::value(registry.find_message("OrderSettings", 1));
    constexpr std::uint64_t payload_limit = 64;
    return primer::value(angaria::BinaryPayloadCodec::from_registry(registry, schema, payload_limit));
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
    const auto codec = configure_orders(argv[1]);
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
    const auto payload = primer::value(codec.encode({
        true, BitFieldValue{0x8001}, EnumValue{std::uint8_t{1}},
        FixedAsciiValue{"IBM   "}, std::int8_t{-128}}));
    const auto decoded = primer::value(codec.decode(payload));
    std::cout << "OrderSettings payload bytes: " << payload.size() << '\n'
              << "OrderSettings hex: ";
    primer::print_hex(payload);
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
    if (!error || error->_code != ErrorCode::InvalidArgument || output != original)
    {
      throw std::runtime_error("An undeclared enum must fail without changing the output buffer");
    }
    std::cout << "Rejected undeclared enum: " << primer::error_name(error->_code) << '\n'
              << "Caller buffer unchanged: yes\n";
    values[2] = EnumValue{std::uint8_t{1}};
    primer::value(codec.encode_into(values, output));
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
