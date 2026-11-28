// Independent processes exchange completed capture files while the Registry
// retains exact schema versions. This example does not call Queue delivery APIs.
#include "angaria/runtime.h"
#include "angaria/capture.h"
#include "angaria/inspection.h"

#include <array>
#include <chrono>
#include <cstddef>
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
#include <string_view>
#include <system_error>
#include <utility>
#include <variant>
#include <vector>

namespace
{
  using namespace angaria;

  constexpr auto price_field = static_cast<FieldId>(0xee000001);
  constexpr auto quantity_field = static_cast<FieldId>(0xee000002);

  void reserve_directory(const std::filesystem::path& directory)
  {
    std::error_code error;
    if (!std::filesystem::create_directory(directory, error))
    {
      if (!error || error == std::errc::file_exists)
      {
        throw std::runtime_error("Conflict at workspace: Choose an output directory that does not exist yet");
      }
      throw std::filesystem::filesystem_error("Cannot reserve output directory", directory, error);
    }
  }

  std::filesystem::path capture_path(const std::filesystem::path& workspace, std::string_view phase)
  {
    return workspace / phase / "capture.angcap";
  }

  Result<SchemaId> register_quote(Registry& registry, std::uint32_t version)
  {
    MessageDescriptor descriptor;
    descriptor._schema = {static_cast<MessageType>(1), version};
    descriptor._name = "Quote";
    descriptor._fields = version == 1 ? std::vector{price_field, quantity_field}
                                       : std::vector{quantity_field, price_field};
    auto register_message_result = registry.register_message(descriptor);
    if (const auto* error = std::get_if<Error>(&register_message_result))
    {
      return *error;
    }
    return std::get<0>(std::move(register_message_result));
  }

  OperationResult initialize(const std::filesystem::path& workspace)
  {
    reserve_directory(workspace);
    TaskDescriptor setup;
    setup._name = "Setup";
    SessionOptions options;
    options._name = "Process message exchange";
    options._directory = workspace / "session";
    options._maxLogBytes = 1024 * 1024;
    options._limits = CapacityLimits::empty().tasks(2).fields(2).message_schemas(3);
    options._defaults._ringCapacity = 8;
    auto create_result = Session::create(options, setup);
    if (const auto* error = std::get_if<Error>(&create_result))
    {
      return *error;
    }
    auto created = std::get<0>(std::move(create_result));
    auto registry = created._creator.registry();
    TaskDescriptor feed;
    feed._name = "Feed";
    auto register_task_result = registry.register_task(feed);
    if (const auto* error = std::get_if<Error>(&register_task_result))
    {
      return *error;
    }
    const auto producer = std::get<0>(std::move(register_task_result));

    FieldDescriptor price;
    price._id = price_field;
    price._name = "PriceTicks";
    price._base = EFieldBase::BASE_INT64;
    price._size = 8;
    auto register_field_result = registry.register_field(price);
    if (const auto* error = std::get_if<Error>(&register_field_result))
    {
      return *error;
    }

    FieldDescriptor quantity;
    quantity._id = quantity_field;
    quantity._name = "Quantity";
    quantity._base = EFieldBase::BASE_UINT64;
    quantity._size = 8;
    auto register_field_result_2 = registry.register_field(quantity);
    if (const auto* error = std::get_if<Error>(&register_field_result_2))
    {
      return *error;
    }

    auto register_quote_result = register_quote(registry, 1);
    if (const auto* error = std::get_if<Error>(&register_quote_result))
    {
      return *error;
    }

    MessageDescriptor heartbeat;
    heartbeat._schema = {static_cast<MessageType>(2), 1};
    heartbeat._name = "Heartbeat";
    auto register_message_result_2 = registry.register_message(heartbeat);
    if (const auto* error = std::get_if<Error>(&register_message_result_2))
    {
      return *error;
    }

    auto snapshot_result = registry.snapshot();
    if (const auto* error = std::get_if<Error>(&snapshot_result))
    {
      return *error;
    }
    const auto session_id = created._session.identity();
    std::ostringstream identity;
    identity.imbue(std::locale::classic());
    identity << std::hex << std::setfill('0') << std::setw(16) << session_id._high
             << std::setw(16) << session_id._low;
    std::cout << "SESSION " << identity.str() << '\n'
              << "INITIALIZED revision=" << std::get<0>(std::move(snapshot_result))._revision
              << " feed=" << static_cast<unsigned>(producer) << '\n';
    return std::monostate{};
  }

  OperationResult upgrade(const std::filesystem::path& workspace)
  {
    auto attach_result = Session::attach(workspace / "session");
    if (const auto* error = std::get_if<Error>(&attach_result))
    {
      return *error;
    }
    auto session = std::get<0>(std::move(attach_result));
    auto find_task_result = session.registry().find_task("Setup");
    if (const auto* error = std::get_if<Error>(&find_task_result))
    {
      return *error;
    }
    auto attach_task_result = session.attach_task(std::get<0>(std::move(find_task_result)));
    if (const auto* error = std::get_if<Error>(&attach_task_result))
    {
      return *error;
    }
    auto setup = std::get<0>(std::move(attach_task_result));
    auto registry = setup.registry();
    // Version 2 has the same byte width, but changes Field order. Registration
    // adds a definition; it cannot replace the meaning of an existing version.
    auto register_quote_result_2 = register_quote(registry, 2);
    if (const auto* error = std::get_if<Error>(&register_quote_result_2))
    {
      return *error;
    }

    auto snapshot_result_2 = registry.snapshot();
    if (const auto* error = std::get_if<Error>(&snapshot_result_2))
    {
      return *error;
    }
    std::cout << "UPGRADED version=2 revision=" << std::get<0>(std::move(snapshot_result_2))._revision << '\n';
    return std::monostate{};
  }

  OperationResult append(MessageCaptureWriter& writer, Task& producer, const BinaryPayloadCodec& codec,
                 std::initializer_list<PayloadValue> values)
  {
    auto encode_result = codec.encode(values);
    if (const auto* error = std::get_if<Error>(&encode_result))
    {
      return *error;
    }
    const auto payload = std::get<0>(std::move(encode_result));
    MessageHeader header;
    header._schema = codec.schema();
    // Application-chosen capture-local numbering leaves Registry setup open
    // for this metadata-evolution lesson. These are not issued transport IDs
    // or Queue acceptances; never mix them into producer-log deduplication.
    header._publication = PublicationId{producer.claim(), writer.records_written() + 1};
    // Fixed application-chosen timestamps make the walkthrough reproducible.
    // They do not establish native acceptance or delivery times.
    header._publishedAt = UtcTimestamp{std::chrono::nanoseconds{
        static_cast<std::int64_t>(100 + header._publication._sequence)}};
    header._payloadBytes = payload.size();
    auto append_result = writer.append(header, payload);
    if (const auto* error = std::get_if<Error>(&append_result))
    {
      return *error;
    }

    return std::monostate{};
  }

  void wait_for_signal()
  {
    std::string line;
    if (!std::getline(std::cin, line))
    {
      throw std::runtime_error("Input closed before the continuation signal");
    }
  }

  OperationResult produce(const std::filesystem::path& workspace, std::string_view phase, bool hold)
  {
    const auto version = phase == "new" ? 2u : 1u;
    const auto directory = workspace / phase;
    // This preflight avoids acquiring a new claim for an ordinary repeated
    // command. The later exclusive directory reservation prevents overwrites
    // even if another caller creates the phase directory in the meantime.
    if (std::filesystem::exists(directory))
    {
      throw std::runtime_error("Conflict at workspace: The phase output already exists");
    }
    auto attach_result_2 = Session::attach(workspace / "session");
    if (const auto* error = std::get_if<Error>(&attach_result_2))
    {
      return *error;
    }
    auto session = std::get<0>(std::move(attach_result_2));
    auto registry = session.registry();
    auto find_message_result = registry.find_message("Quote", version);
    if (const auto* error = std::get_if<Error>(&find_message_result))
    {
      return *error;
    }
    auto from_registry_result = BinaryPayloadCodec::from_registry(
        registry, std::get<0>(std::move(find_message_result)), 16);
    if (const auto* error = std::get_if<Error>(&from_registry_result))
    {
      return *error;
    }
    const auto quote = std::get<0>(std::move(from_registry_result));
    auto find_message_result_2 = registry.find_message("Heartbeat", 1);
    if (const auto* error = std::get_if<Error>(&find_message_result_2))
    {
      return *error;
    }
    auto from_registry_result_2 = BinaryPayloadCodec::from_registry(
        registry, std::get<0>(std::move(find_message_result_2)), 0);
    if (const auto* error = std::get_if<Error>(&from_registry_result_2))
    {
      return *error;
    }
    const auto heartbeat = std::get<0>(std::move(from_registry_result_2));
    auto find_task_result_2 = registry.find_task("Feed");
    if (const auto* error = std::get_if<Error>(&find_task_result_2))
    {
      return *error;
    }
    auto attach_task_result_2 = session.attach_task(std::get<0>(std::move(find_task_result_2)));
    if (const auto* error = std::get_if<Error>(&attach_task_result_2))
    {
      return *error;
    }
    auto feed = std::get<0>(std::move(attach_task_result_2));
    reserve_directory(directory);
    std::ofstream output(capture_path(workspace, phase), std::ios::binary);
    if (!output)
    {
      throw std::runtime_error("IoError at capture: Cannot create the capture in its new directory");
    }
    auto create_result_2 = MessageCaptureWriter::create(output, 96, 176);
    if (const auto* error = std::get_if<Error>(&create_result_2))
    {
      return *error;
    }
    auto writer = std::get<0>(std::move(create_result_2));
    if (version == 2)
    {
      auto append_result_2 = append(writer, feed, quote, {std::uint64_t{8}, std::int64_t{-3}});
      if (const auto* error = std::get_if<Error>(&append_result_2))
      {
        return *error;
      }

    }
    else
    {
      auto append_result_3 = append(writer, feed, quote, {
          std::int64_t{phase == "first" ? -2 : -4}, std::uint64_t{phase == "first" ? 7u : 9u}});
      if (const auto* error = std::get_if<Error>(&append_result_3))
      {
        return *error;
      }

    }
    auto append_result_4 = append(writer, feed, heartbeat, {});
    if (const auto* error = std::get_if<Error>(&append_result_4))
    {
      return *error;
    }

    output.flush();
    output.close();
    if (!output)
    {
      throw std::runtime_error("IoError at capture: Cannot finish the capture file");
    }
    std::cout << "PRODUCED phase=" << phase << " version=" << version
              << " producer=" << static_cast<unsigned>(feed.claim()._task)
              << " incarnation=" << feed.claim()._incarnation
              << " records=" << writer.records_written() << " bytes=" << writer.bytes_written() << std::endl;
    if (hold)
    {
      // The test may terminate this owner only after a completed, closed file.
      // Restart qualifies claim replacement, not recovery of a torn capture.
      std::cout << "READY producer" << std::endl;
      wait_for_signal();
      std::cout << "RELEASED producer\n";
    }
    return std::monostate{};
  }

  Result<MessageInspector> retained_catalog(const std::filesystem::path& workspace)
  {
    auto attach_result_3 = Session::attach(workspace / "session");
    if (const auto* error = std::get_if<Error>(&attach_result_3))
    {
      return *error;
    }
    auto session = std::get<0>(std::move(attach_result_3));
    InspectionLimits limits;
    limits._maxRecordBytes = 96;
    limits._maxCatalogBytes = 4096;
    limits._maxTextBytes = 4096;
    auto from_session_result = MessageInspector::from_session(session, limits);
    if (const auto* error = std::get_if<Error>(&from_session_result))
    {
      return *error;
    }
    return std::get<0>(std::move(from_session_result));
    // The Session and Registry snapshot are gone before the caller signals
    // READY. Only owning Field/schema definitions remain in the inspector.
  }

  Result<std::string> inspect_capture(const MessageInspector& inspector, const std::filesystem::path& path)
  {
    std::ifstream input(path, std::ios::binary);
    if (!input)
    {
      throw std::runtime_error("IoError at capture: Cannot open the completed capture");
    }
    auto create_result_3 = MessageCaptureReader::create(input, 96, 176);
    if (const auto* error = std::get_if<Error>(&create_result_3))
    {
      return *error;
    }
    auto reader = std::get<0>(std::move(create_result_3));
    std::string text;
    for (;;)
    {
      auto next = reader.read_next();
      if (const auto* error = std::get_if<Error>(&next))
      {
        return *error;
      }
      auto record = std::get<0>(std::move(next));
      if (!record)
      {
        break;
      }
      auto inspect_result = inspector.inspect(record->view());
      if (const auto* error = std::get_if<Error>(&inspect_result))
      {
        return *error;
      }
      text += std::get<0>(std::move(inspect_result));
    }
    if (reader.records_read() != 2)
    {
      throw std::runtime_error("Expected two complete captured records");
    }
    return text;
  }

  OperationResult retain(const std::filesystem::path& workspace)
  {
    auto retained_catalog_result = retained_catalog(workspace);
    if (const auto* error = std::get_if<Error>(&retained_catalog_result))
    {
      return *error;
    }
    const auto inspector = std::get<0>(std::move(retained_catalog_result));
    std::cout << "READY retained" << std::endl;
    wait_for_signal();
    for (const auto phase : {"first", "restart"})
    {
      auto inspect_capture_result = inspect_capture(inspector, capture_path(workspace, phase));
      if (const auto* error = std::get_if<Error>(&inspect_capture_result))
      {
        return *error;
      }
      const auto text = std::get<0>(std::move(inspect_capture_result));
      std::cout << "RETAINED phase=" << phase << '\n' << text;
    }
    std::ifstream input(capture_path(workspace, "new"), std::ios::binary);
    if (!input)
    {
      throw std::runtime_error("IoError at capture: Cannot open the new-version capture");
    }
    auto create_result_4 = MessageCaptureReader::create(input, 96, 176);
    if (const auto* error = std::get_if<Error>(&create_result_4))
    {
      return *error;
    }
    auto reader = std::get<0>(std::move(create_result_4));
    auto read_next_result = reader.read_next();
    if (const auto* error = std::get_if<Error>(&read_next_result))
    {
      return *error;
    }
    const auto record = std::get<0>(std::move(read_next_result));
    if (!record)
    {
      throw std::runtime_error("Expected a new-version Quote record");
    }
    const auto attempted = inspector.inspect(record->view());
    const auto* error = std::get_if<Error>(&attempted);
    if (!error)
    {
      return Error{ErrorCode::InvalidState,
          "Expected the retained catalog to reject absent Quote v2 with NotFound", "lesson.retained_catalog"};
    }
    if (error->_code != ErrorCode::NotFound || error->_path != "inspection.schema")
    {
      return *error;
    }
    std::cout << "REJECTED phase=new code=NotFound record=0 offset=0\n";
    return std::monostate{};
  }

  void report_failure(std::string_view text)
  {
    // Output-only application formatting: keep paths and errors on one
    // printable line without depending on Angaria implementation headers.
    constexpr std::string_view digits{"0123456789abcdef"};
    for (const auto character : text)
    {
      const auto byte = static_cast<unsigned char>(character);
      if (byte == '\\')
      {
        std::cerr << "\\\\";
      }
      else if (byte >= 0x20 && byte <= 0x7e)
      {
        std::cerr.put(character);
      }
      else
      {
        std::cerr << "\\x" << digits[byte >> 4] << digits[byte & 0xf];
      }
    }
    std::cerr << '\n';
  }

  void usage()
  {
    std::cerr << "Usage: angaria_primer_13_process_exchange init <new-workspace>\n"
              << "       angaria_primer_13_process_exchange produce <workspace> <first|new|restart> [--hold]\n"
              << "       angaria_primer_13_process_exchange upgrade <workspace>\n"
              << "       angaria_primer_13_process_exchange retain <workspace>\n";
  }
}

int main(int argc, char** argv)
{
  using namespace angaria;
  try
  {
    if (argc == 3 && std::string_view{argv[1]} == "init")
    {
      auto initialize_result = initialize(argv[2]);
      if (const auto* error = std::get_if<Error>(&initialize_result))
      {
        if (error->_code == ErrorCode::Conflict)
        {
          std::cerr << "Conflict: ";
        }
        report_failure("Error " + std::to_string(static_cast<unsigned>(error->_code)) +
                       " at " + error->_path + ": " + error->_message);
        return 1;
      }

    }
    else if (argc == 3 && std::string_view{argv[1]} == "upgrade")
    {
      auto upgrade_result = upgrade(argv[2]);
      if (const auto* error = std::get_if<Error>(&upgrade_result))
      {
        if (error->_code == ErrorCode::Conflict)
        {
          std::cerr << "Conflict: ";
        }
        report_failure("Error " + std::to_string(static_cast<unsigned>(error->_code)) +
                       " at " + error->_path + ": " + error->_message);
        return 1;
      }

    }
    else if (argc == 3 && std::string_view{argv[1]} == "retain")
    {
      auto retain_result = retain(argv[2]);
      if (const auto* error = std::get_if<Error>(&retain_result))
      {
        if (error->_code == ErrorCode::Conflict)
        {
          std::cerr << "Conflict: ";
        }
        report_failure("Error " + std::to_string(static_cast<unsigned>(error->_code)) +
                       " at " + error->_path + ": " + error->_message);
        return 1;
      }

    }
    else if ((argc == 4 || argc == 5) && std::string_view{argv[1]} == "produce")
    {
      const std::string_view phase{argv[3]};
      if ((phase != "first" && phase != "new" && phase != "restart") ||
          (argc == 5 && std::string_view{argv[4]} != "--hold"))
      {
        usage();
        return 1;
      }
      auto produce_result = produce(argv[2], phase, argc == 5);
      if (const auto* error = std::get_if<Error>(&produce_result))
      {
        if (error->_code == ErrorCode::Conflict)
        {
          std::cerr << "Conflict: ";
        }
        report_failure("Error " + std::to_string(static_cast<unsigned>(error->_code)) +
                       " at " + error->_path + ": " + error->_message);
        return 1;
      }

    }
    else
    {
      usage();
      return 1;
    }
    return 0;
  }
  catch (const std::exception& error)
  {
    report_failure(error.what());
    return 1;
  }
}
