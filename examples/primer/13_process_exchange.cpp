// Independent processes exchange completed capture files while the Registry
// retains exact schema versions. This example does not call Queue delivery APIs.
#include "example_support.h"
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

  std::string session_text(SessionId session)
  {
    std::ostringstream output;
    output.imbue(std::locale::classic());
    output << std::hex << std::setfill('0') << std::setw(16) << session._high
           << std::setw(16) << session._low;
    return output.str();
  }

  SchemaId register_quote(Registry& registry, std::uint32_t version)
  {
    MessageDescriptor descriptor;
    descriptor._schema = {static_cast<MessageType>(1), version};
    descriptor._name = "Quote";
    descriptor._fields = version == 1 ? std::vector{price_field, quantity_field}
                                       : std::vector{quantity_field, price_field};
    return primer::value(registry.register_message(descriptor));
  }

  void initialize(const std::filesystem::path& workspace)
  {
    reserve_directory(workspace);
    TaskDescriptor setup;
    setup._name = "Setup";
    auto options = primer::session_options(workspace / "session", "Process message exchange");
    options._limits = CapacityLimits::empty().tasks(2).fields(2).message_schemas(3);
    auto created = primer::value(Session::create(options, setup));
    auto registry = created._creator.registry();
    TaskDescriptor feed;
    feed._name = "Feed";
    const auto producer = primer::value(registry.register_task(feed));

    FieldDescriptor price;
    price._id = price_field;
    price._name = "PriceTicks";
    price._base = EFieldBase::BASE_INT64;
    price._size = 8;
    primer::value(registry.register_field(price));
    FieldDescriptor quantity;
    quantity._id = quantity_field;
    quantity._name = "Quantity";
    quantity._base = EFieldBase::BASE_UINT64;
    quantity._size = 8;
    primer::value(registry.register_field(quantity));
    register_quote(registry, 1);
    MessageDescriptor heartbeat;
    heartbeat._schema = {static_cast<MessageType>(2), 1};
    heartbeat._name = "Heartbeat";
    primer::value(registry.register_message(heartbeat));
    std::cout << "SESSION " << session_text(created._session.identity()) << '\n'
              << "INITIALIZED revision=" << primer::value(registry.snapshot())._revision
              << " feed=" << static_cast<unsigned>(producer) << '\n';
  }

  void upgrade(const std::filesystem::path& workspace)
  {
    auto session = primer::value(Session::attach(workspace / "session"));
    auto setup = primer::value(session.attach_task(primer::value(session.registry().find_task("Setup"))));
    auto registry = setup.registry();
    // Version 2 has the same byte width, but changes Field order. Registration
    // adds a definition; it cannot replace the meaning of an existing version.
    register_quote(registry, 2);
    std::cout << "UPGRADED version=2 revision=" << primer::value(registry.snapshot())._revision << '\n';
  }

  void append(MessageCaptureWriter& writer, Task& producer, const BinaryPayloadCodec& codec,
                 std::initializer_list<PayloadValue> values)
  {
    const auto payload = primer::value(codec.encode(values));
    MessageHeader header;
    header._schema = codec.schema();
    header._publication = primer::value(producer.next_publication_id());
    // Fixed application-chosen timestamps make the walkthrough reproducible.
    // They do not establish native acceptance or delivery times.
    header._publicationTime = UtcTimestamp{std::chrono::nanoseconds{
        static_cast<std::int64_t>(100 + header._publication._sequence)}};
    header._payloadBytes = payload.size();
    primer::value(writer.append(header, payload));
  }

  void wait_for_signal()
  {
    std::string line;
    if (!std::getline(std::cin, line))
    {
      throw std::runtime_error("Input closed before the continuation signal");
    }
  }

  void produce(const std::filesystem::path& workspace, std::string_view phase, bool hold)
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
    auto session = primer::value(Session::attach(workspace / "session"));
    auto registry = session.registry();
    const auto quote = primer::value(BinaryPayloadCodec::from_registry(
        registry, primer::value(registry.find_message("Quote", version)), 16));
    const auto heartbeat = primer::value(BinaryPayloadCodec::from_registry(
        registry, primer::value(registry.find_message("Heartbeat", 1)), 0));
    auto feed = primer::value(session.attach_task(primer::value(registry.find_task("Feed"))));
    reserve_directory(directory);
    std::ofstream output(capture_path(workspace, phase), std::ios::binary);
    if (!output)
    {
      throw std::runtime_error("IoError at capture: Cannot create the capture in its new directory");
    }
    auto writer = primer::value(MessageCaptureWriter::create(output, 96, 176));
    if (version == 2)
    {
      append(writer, feed, quote, {std::uint64_t{8}, std::int64_t{-3}});
    }
    else
    {
      append(writer, feed, quote, {
          std::int64_t{phase == "first" ? -2 : -4}, std::uint64_t{phase == "first" ? 7u : 9u}});
    }
    append(writer, feed, heartbeat, {});
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
  }

  MessageInspector retained_catalog(const std::filesystem::path& workspace)
  {
    auto session = primer::value(Session::attach(workspace / "session"));
    InspectionLimits limits;
    limits._maxRecordBytes = 96;
    limits._maxCatalogBytes = 4096;
    limits._maxTextBytes = 4096;
    return primer::value(MessageInspector::from_session(session, limits));
    // The Session and Registry snapshot are gone before the caller signals
    // READY. Only owning Field/schema definitions remain in the inspector.
  }

  std::string inspect_capture(const MessageInspector& inspector, const std::filesystem::path& path)
  {
    std::ifstream input(path, std::ios::binary);
    if (!input)
    {
      throw std::runtime_error("IoError at capture: Cannot open the completed capture");
    }
    auto reader = primer::value(MessageCaptureReader::create(input, 96, 176));
    std::string text;
    while (auto record = primer::value(reader.read_next()))
    {
      text += primer::value(inspector.inspect(record->view()));
    }
    if (reader.records_read() != 2)
    {
      throw std::runtime_error("Expected two complete captured records");
    }
    return text;
  }

  void retain(const std::filesystem::path& workspace)
  {
    const auto inspector = retained_catalog(workspace);
    std::cout << "READY retained" << std::endl;
    wait_for_signal();
    for (const auto phase : {"first", "restart"})
    {
      const auto text = inspect_capture(inspector, capture_path(workspace, phase));
      std::cout << "RETAINED phase=" << phase << '\n' << text;
    }
    std::ifstream input(capture_path(workspace, "new"), std::ios::binary);
    if (!input)
    {
      throw std::runtime_error("IoError at capture: Cannot open the new-version capture");
    }
    auto reader = primer::value(MessageCaptureReader::create(input, 96, 176));
    const auto record = primer::value(reader.read_next());
    if (!record)
    {
      throw std::runtime_error("Expected a new-version Quote record");
    }
    const auto attempted = inspector.inspect(record->view());
    const auto* error = std::get_if<Error>(&attempted);
    if (!error || error->_code != ErrorCode::NotFound || error->_path != "inspection.schema")
    {
      throw std::runtime_error("The retained catalog must reject absent Quote v2 with NotFound");
    }
    std::cout << "REJECTED phase=new code=NotFound record=0 offset=0\n";
  }

  void report_failure(std::string_view text)
  {
    // Filesystem failures can contain caller pathnames. Keep diagnostics on
    // one ASCII line, just as the generic inspection tool does.
    constexpr std::string_view digits{"0123456789abcdef"};
    std::array<char, 1024> buffer{};
    std::size_t used{};
    for (const auto character : text)
    {
      if (buffer.size() - used < 4)
      {
        std::cerr.write(buffer.data(), static_cast<std::streamsize>(used));
        used = 0;
      }
      const auto byte = static_cast<unsigned char>(character);
      if (byte == '\\')
      {
        buffer[used++] = '\\';
        buffer[used++] = '\\';
      }
      else if (byte >= 0x20 && byte <= 0x7e)
      {
        buffer[used++] = character;
      }
      else
      {
        buffer[used++] = '\\';
        buffer[used++] = 'x';
        buffer[used++] = digits[byte >> 4];
        buffer[used++] = digits[byte & 0x0f];
      }
    }
    std::cerr.write(buffer.data(), static_cast<std::streamsize>(used));
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
  try
  {
    if (argc == 3 && std::string_view{argv[1]} == "init")
    {
      initialize(argv[2]);
    }
    else if (argc == 3 && std::string_view{argv[1]} == "upgrade")
    {
      upgrade(argv[2]);
    }
    else if (argc == 3 && std::string_view{argv[1]} == "retain")
    {
      retain(argv[2]);
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
      produce(argv[2], phase, argc == 5);
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
