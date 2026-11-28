// An application writes four portable records. A separate generic inspector
// learns their names, versions, types, and enum labels from the retained Registry.
#include "angaria/runtime.h"
#include "angaria/capture.h"

#include <chrono>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <initializer_list>
#include <iostream>
#include <locale>
#include <sstream>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>
#include <variant>
#include <vector>

namespace
{
  using namespace angaria;

  FieldDescriptor field(std::uint32_t id, std::string name, EFieldBase base, std::uint32_t size)
  {
    FieldDescriptor descriptor;
    descriptor._id = static_cast<FieldId>(id);
    descriptor._name = std::move(name);
    descriptor._base = base;
    descriptor._size = size;
    return descriptor;
  }

  Result<SchemaId> register_schema(Registry& registry, unsigned type, std::uint32_t version,
                             std::string name, std::vector<FieldId> fields)
  {
    MessageDescriptor descriptor;
    descriptor._schema = {static_cast<MessageType>(type), version};
    descriptor._name = std::move(name);
    descriptor._fields = std::move(fields);
    auto register_message_result = registry.register_message(descriptor);
    if (const auto* error = std::get_if<Error>(&register_message_result))
    {
      return *error;
    }
    return std::get<0>(std::move(register_message_result));
  }

  OperationResult append(MessageCaptureWriter& writer, Task& producer, const BinaryPayloadCodec& codec,
                 std::initializer_list<PayloadValue> values, std::uint32_t flags = 0)
  {
    auto encode_result = codec.encode(values);
    if (const auto* error = std::get_if<Error>(&encode_result))
    {
      return *error;
    }
    const auto payload = std::get<0>(std::move(encode_result));
    MessageHeader header;
    header._schema = codec.schema();
    // This application numbers records within this portable capture. These
    // identities are not issued transport IDs or accepted Queue publications;
    // do not combine them with the producer log's deduplication namespace.
    header._publication = PublicationId{producer.claim(), writer.records_written() + 1};
    header._publishedAt = UtcTimestamp{std::chrono::nanoseconds{
        static_cast<std::int64_t>(100 + header._publication._sequence)}};
    header._flags = flags;
    header._payloadBytes = payload.size();
    auto append_result = writer.append(header, payload);
    if (const auto* error = std::get_if<Error>(&append_result))
    {
      return *error;
    }

    return std::monostate{};
  }

  struct CaptureSummary
  {
    SessionId _session;
    std::uint64_t _records{};
    std::uint64_t _bytes{};
  };

  Result<CaptureSummary> create_capture(const std::filesystem::path& workspace)
  {
    std::error_code error;
    if (!std::filesystem::create_directory(workspace, error))
    {
      if (!error || error == std::errc::file_exists)
      {
        throw std::runtime_error("Conflict at workspace: Choose a directory that does not exist yet");
      }
      throw std::filesystem::filesystem_error("Cannot create capture workspace", workspace, error);
    }
    TaskDescriptor setup;
    setup._name = "Setup";
    SessionOptions options;
    options._name = "Capture and inspect";
    options._directory = workspace / "session";
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

    auto price = field(0xee000001, "PriceTicks", EFieldBase::BASE_INT64, 8);
    auto quantity = field(0xee000002, "Quantity", EFieldBase::BASE_UINT64, 8);
    auto side = field(0xee000003, "Side", EFieldBase::BASE_BYTE, 1);
    side._logical._kind = FieldLogicalKind::Enumeration;
    side._logical._enumerators = {{"Sell", std::uint64_t{2}}, {"Buy", std::uint64_t{1}}};
    auto instrument = field(0xee000004, "Instrument", EFieldBase::BASE_CHARS, 6);
    instrument._logical._kind = FieldLogicalKind::FixedAscii;
    auto flags = field(0xee000005, "Flags", EFieldBase::BASE_UINT16, 2);
    flags._logical._kind = FieldLogicalKind::Bits;
    auto enabled = field(0xee000006, "Enabled", EFieldBase::BASE_BYTE, 1);
    enabled._logical._kind = FieldLogicalKind::Boolean;
    auto adjustment = field(0xee000007, "Adjustment", EFieldBase::BASE_INT8, 1);
    for (const auto& descriptor : {price, quantity, side, instrument, flags, enabled, adjustment})
    {
      auto register_field_result = registry.register_field(descriptor);
      if (const auto* error = std::get_if<Error>(&register_field_result))
      {
        return *error;
      }

    }
    auto register_schema_result = register_schema(registry, 1, 1, "Quote", {price._id, quantity._id});
    if (const auto* error = std::get_if<Error>(&register_schema_result))
    {
      return *error;
    }
    const auto first = std::get<0>(std::move(register_schema_result));
    // Both versions occupy sixteen payload bytes. Looking up the newest
    // version for an older record would silently reverse its interpretation.
    auto register_schema_result_2 = register_schema(registry, 1, 2, "Quote", {quantity._id, price._id});
    if (const auto* error = std::get_if<Error>(&register_schema_result_2))
    {
      return *error;
    }
    const auto second = std::get<0>(std::move(register_schema_result_2));
    auto register_schema_result_3 = register_schema(registry, 2, 1, "Order",
        {side._id, quantity._id, instrument._id, flags._id, enabled._id, adjustment._id});
    if (const auto* error = std::get_if<Error>(&register_schema_result_3))
    {
      return *error;
    }
    const auto order = std::get<0>(std::move(register_schema_result_3));
    auto register_schema_result_4 = register_schema(registry, 3, 1, "Heartbeat", {});
    if (const auto* error = std::get_if<Error>(&register_schema_result_4))
    {
      return *error;
    }
    const auto heartbeat = std::get<0>(std::move(register_schema_result_4));

    auto from_registry_result = BinaryPayloadCodec::from_registry(created._session.registry(), first, 128);
    if (const auto* error = std::get_if<Error>(&from_registry_result))
    {
      return *error;
    }
    auto from_registry_result_2 = BinaryPayloadCodec::from_registry(created._session.registry(), second, 128);
    if (const auto* error = std::get_if<Error>(&from_registry_result_2))
    {
      return *error;
    }
    auto from_registry_result_3 = BinaryPayloadCodec::from_registry(created._session.registry(), order, 128);
    if (const auto* error = std::get_if<Error>(&from_registry_result_3))
    {
      return *error;
    }
    auto from_registry_result_4 = BinaryPayloadCodec::from_registry(created._session.registry(), heartbeat, 128);
    if (const auto* error = std::get_if<Error>(&from_registry_result_4))
    {
      return *error;
    }

    std::ofstream output(workspace / "capture.angcap", std::ios::binary);
    if (!output)
    {
      throw std::runtime_error("IoError at capture: Cannot create capture in the new workspace");
    }
    auto create_result_2 = MessageCaptureWriter::create(output, 1024, 4096);
    if (const auto* error = std::get_if<Error>(&create_result_2))
    {
      return *error;
    }
    auto writer = std::get<0>(std::move(create_result_2));
    auto append_result_2 = append(writer, created._creator, std::get<0>(from_registry_result), {std::int64_t{-2}, std::uint64_t{7}}, 0x80000001u);
    if (const auto* error = std::get_if<Error>(&append_result_2))
    {
      return *error;
    }

    auto append_result_3 = append(writer, created._creator, std::get<0>(from_registry_result_2), {std::uint64_t{8}, std::int64_t{-3}});
    if (const auto* error = std::get_if<Error>(&append_result_3))
    {
      return *error;
    }

    auto append_result_4 = append(writer, created._creator, std::get<0>(from_registry_result_3), {
        EnumValue{std::uint8_t{1}}, std::uint64_t{9}, FixedAsciiValue{std::string{"IBM\0 \n", 6}},
        BitFieldValue{0x8001}, true, std::int8_t{-128}});
    if (const auto* error = std::get_if<Error>(&append_result_4))
    {
      return *error;
    }

    auto append_result_5 = append(writer, created._creator, std::get<0>(from_registry_result_4), {});
    if (const auto* error = std::get_if<Error>(&append_result_5))
    {
      return *error;
    }

    output.flush();
    output.close();
    if (!output)
    {
      throw std::runtime_error("IoError at capture: Cannot finish the capture file");
    }
    return CaptureSummary{created._session.identity(), writer.records_written(), writer.bytes_written()};
    // The producer's handles close before main prints this summary. The next
    // command starts a generic inspector using only the retained Session.
  }
}

int main(int argc, char** argv)
{
  using namespace angaria;
  if (argc != 2)
  {
    std::cerr << "Usage: " << argv[0] << " <new-workspace-directory>\n";
    return 1;
  }
  try
  {
    auto create_capture_result = create_capture(argv[1]);
    if (const auto* error = std::get_if<Error>(&create_capture_result))
    {
      if (error->_code == ErrorCode::Conflict)
      {
        std::cerr << "Conflict: ";
      }
      std::cerr << "Error " << static_cast<unsigned>(error->_code) << " at "
                << error->_path << ": " << error->_message << '\n';
      return 1;
    }
    const auto captured = std::get<0>(std::move(create_capture_result));
    // SessionId is an opaque pair of words; this local stream only formats
    // that public value for the separately launched inspection command.
    std::ostringstream identity;
    identity.imbue(std::locale::classic());
    identity << std::hex << std::setfill('0') << std::setw(16) << captured._session._high
             << std::setw(16) << captured._session._low;
    std::cout << "Source Session: " << identity.str() << '\n'
              << "Captured records: " << captured._records << '\n'
              << "Capture bytes: " << captured._bytes << '\n'
              << "Retained schemas: Quote v1, Quote v2, Order v1, Heartbeat v1\n"
              << "Capture encoding only: no messages were sent.\n";
    return 0;
  }
  catch (const std::exception& error)
  {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
