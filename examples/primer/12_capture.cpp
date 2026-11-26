// An application writes four portable records. A separate generic inspector
// learns their names, versions, types, and enum labels from the retained Registry.
#include "example_support.h"
#include "example_formatting.h"
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

  SchemaId register_schema(Registry& registry, unsigned type, std::uint32_t version,
                             std::string name, std::vector<FieldId> fields)
  {
    MessageDescriptor descriptor;
    descriptor._schema = {static_cast<MessageType>(type), version};
    descriptor._name = std::move(name);
    descriptor._fields = std::move(fields);
    return primer::value(registry.register_message(descriptor));
  }

  void append(MessageCaptureWriter& writer, Task& producer, const BinaryPayloadCodec& codec,
                 std::initializer_list<PayloadValue> values, std::uint32_t flags = 0)
  {
    const auto payload = primer::value(codec.encode(values));
    MessageHeader header;
    header._schema = codec.schema();
    header._publication = primer::value(producer.next_publication_id());
    header._publishedAt = UtcTimestamp{std::chrono::nanoseconds{
        static_cast<std::int64_t>(100 + header._publication._sequence)}};
    header._flags = flags;
    header._payloadBytes = payload.size();
    primer::value(writer.append(header, payload));
  }

  struct CaptureSummary
  {
    SessionId _session;
    std::uint64_t _records{};
    std::uint64_t _bytes{};
  };

  CaptureSummary create_capture(const std::filesystem::path& workspace)
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
    auto created = primer::value(Session::create(
        primer::session_options(workspace / "session", "Capture and inspect"), setup));
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
      primer::value(registry.register_field(descriptor));
    }
    const auto first = register_schema(registry, 1, 1, "Quote", {price._id, quantity._id});
    // Both versions occupy sixteen payload bytes. Looking up the newest
    // version for an older record would silently reverse its interpretation.
    const auto second = register_schema(registry, 1, 2, "Quote", {quantity._id, price._id});
    const auto order = register_schema(registry, 2, 1, "Order",
        {side._id, quantity._id, instrument._id, flags._id, enabled._id, adjustment._id});
    const auto heartbeat = register_schema(registry, 3, 1, "Heartbeat", {});
    const auto codec = [&](SchemaId schema)
    {
      return primer::value(BinaryPayloadCodec::from_registry(created._session.registry(), schema, 128));
    };

    std::ofstream output(workspace / "capture.angcap", std::ios::binary);
    if (!output)
    {
      throw std::runtime_error("IoError at capture: Cannot create capture in the new workspace");
    }
    auto writer = primer::value(MessageCaptureWriter::create(output, 1024, 4096));
    append(writer, created._creator, codec(first), {std::int64_t{-2}, std::uint64_t{7}}, 0x80000001u);
    append(writer, created._creator, codec(second), {std::uint64_t{8}, std::int64_t{-3}});
    append(writer, created._creator, codec(order), {
        EnumValue{std::uint8_t{1}}, std::uint64_t{9}, FixedAsciiValue{std::string{"IBM\0 \n", 6}},
        BitFieldValue{0x8001}, true, std::int8_t{-128}});
    append(writer, created._creator, codec(heartbeat), {});
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
  if (argc != 2)
  {
    std::cerr << "Usage: " << argv[0] << " <new-workspace-directory>\n";
    return 1;
  }
  try
  {
    const auto captured = create_capture(argv[1]);
    std::cout << "Source Session: " << primer::session_text(captured._session) << '\n'
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
