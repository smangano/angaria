// Test-only fixture producer. Public Session/Registry APIs register valid names
// containing controls and UTF-8. Capture framing accepts deliberately invalid
// logical payload bytes so the CLI's error-output boundary can be qualified.
#include "result_helpers.h"
#include "angaria/capture.h"
#include "angaria/runtime.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>
#include <variant>

namespace
{
  using namespace angaria;


  SchemaId register_message(Registry& registry, unsigned type, const char* name, FieldId field = {})
  {
    MessageDescriptor descriptor;
    descriptor._schema = {static_cast<MessageType>(type), 1};
    descriptor._name = name;
    if (field != FieldId{})
    {
      descriptor._fields.push_back(field);
    }
    return angaria::test_support::require_value(registry.register_message(descriptor));
  }

  void create_fixture(const std::filesystem::path& directory)
  {
    if (!std::filesystem::create_directory(directory))
    {
      throw std::runtime_error("Fixture directory must not exist yet");
    }
    SessionOptions options;
    options._name = "Capture diagnostic fixture";
    options._directory = directory / "session";
    options._maxLogBytes = 1048576;
    options._limits = CapacityLimits::empty().tasks(1).fields(3).message_schemas(4);
    TaskDescriptor creator;
    creator._name = "Setup";
    auto created = angaria::test_support::require_value(Session::create(options, creator));
    auto registry = created._creator.registry();
    const auto control = register_message(registry, 1, "Control");

    const auto write = [&](const char* file, SchemaId schema, std::byte invalid)
    {
      std::ofstream output(directory / file, std::ios::binary);
      auto writer = angaria::test_support::require_value(MessageCaptureWriter::create(output, 81, 161));
      MessageHeader header;
      header._schema = control;
      header._publication = angaria::test_support::require_value(created._creator.next_publication_id());
      angaria::test_support::require_value(writer.append(header, {}));
      header._schema = schema;
      header._publication = angaria::test_support::require_value(created._creator.next_publication_id());
      header._payloadBytes = 1;
      angaria::test_support::require_value(writer.append(header, std::array{invalid}));
      output.close();
      if (!output)
      {
        throw std::runtime_error("Cannot write diagnostic fixture capture");
      }
    };

    const std::array<const char*, 3> names{"Boolean", "Enumeration", "Ascii"};
    const std::array<const char*, 3> files{"boolean.angcap", "enum.angcap", "ascii.angcap"};
    const std::array<FieldLogicalKind, 3> kinds{
        FieldLogicalKind::Boolean, FieldLogicalKind::Enumeration, FieldLogicalKind::FixedAscii};
    for (unsigned index = 0; index < names.size(); ++index)
    {
      FieldDescriptor descriptor;
      descriptor._id = static_cast<FieldId>(0xee000001u + index);
      descriptor._name = std::string{names[index]} + "\\\n\r\x1b[31m\xc3\xa9";
      descriptor._base = index == 2 ? EFieldBase::BASE_CHARS : EFieldBase::BASE_BYTE;
      descriptor._size = 1;
      descriptor._logical._kind = kinds[index];
      if (index == 1)
      {
        descriptor._logical._enumerators = {{"Only", std::uint64_t{1}}};
      }
      const auto field = angaria::test_support::require_value(registry.register_field(descriptor));
      const auto schema = register_message(registry, index + 2, names[index], field);
      write(files[index], schema, index == 2 ? std::byte{0x80} : std::byte{2});
    }
  }
}

int main(int argc, char** argv)
{
  try
  {
    if (argc != 2)
    {
      throw std::runtime_error("Expected a new fixture directory");
    }
    create_fixture(argv[1]);
    return 0;
  }
  catch (const std::exception& error)
  {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
