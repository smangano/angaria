// Run a messaging lesson first, then pass its existing Session directory.
// This program replays accepted messages into application inspection code; it
// does not resend them or change the live consumers' acknowledgments.
#include "angaria/log.h"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <system_error>
#include <iostream>
#include <optional>
#include <utility>
#include <variant>

int main(int argc, char** argv)
{
  using namespace angaria;
  if (argc < 2 || argc > 3)
  {
    std::cerr << "Usage: " << argv[0] << " SESSION_DIRECTORY [CAPTURE_FILE]\n";
    return 1;
  }

  // Attaching uses the ordinary public Session API. Archive capture requires
  // an admitted Session and creates no Task or live processing obligation.
  auto attached = Session::attach(argv[1]);
  if (const auto* error = std::get_if<Error>(&attached))
  {
    std::cerr << error->_path << ": " << error->_message << '\n';
    return 1;
  }
  auto session = std::get<Session>(std::move(attached));
  ArchiveLimits limits;
  limits._maxRecordBytes = 1048576;
  limits._maxRecords = 1048576;
  limits._maxArchiveBytes = 1073741824;
  auto captured = LogArchive::capture_live(session, limits);
  if (const auto* error = std::get_if<Error>(&captured))
  {
    std::cerr << error->_path << ": " << error->_message << '\n';
    return 1;
  }
  auto archive = std::get<LogArchive>(std::move(captured));
  auto catalog_result = archive.catalog();
  if (const auto* error = std::get_if<Error>(&catalog_result))
  {
    std::cerr << error->_path << ": " << error->_message << '\n';
    return 1;
  }
  auto catalog = std::get<RegistrySnapshot>(std::move(catalog_result));
  auto cursor_result = archive.publications();
  if (const auto* error = std::get_if<Error>(&cursor_result))
  {
    std::cerr << error->_path << ": " << error->_message << '\n';
    return 1;
  }
  auto cursor = std::get<LogCursor>(std::move(cursor_result));
  std::uint64_t count{};
  std::optional<PublicationId> first_publication;
  std::cout << "Message replay preserves producer and Queue order; it is not a consumer execution trace.\n";
  for (;;)
  {
    auto next_result = cursor.read_next();
    if (const auto* error = std::get_if<Error>(&next_result))
    {
      std::cerr << error->_path << ": " << error->_message << '\n';
      return 1;
    }
    auto record = std::get<std::optional<LogRecord>>(std::move(next_result));
    if (!record)
    {
      break; // End of the captured cut, even if producers have sent more.
    }
    const auto& header = record->_message._header;
    if (!first_publication)
    {
      first_publication = header._publication;
    }
    // Resolve the recorded exact schema from this captured catalog. A larger
    // replay application can cache one codec per SchemaId outside this loop.
    auto codec_result = BinaryPayloadCodec::from_snapshot(catalog, header._schema, limits._maxRecordBytes);
    if (const auto* error = std::get_if<Error>(&codec_result))
    {
      std::cerr << error->_path << ": " << error->_message << '\n';
      return 1;
    }
    auto codec = std::get<BinaryPayloadCodec>(std::move(codec_result));
    auto decoded_result = record->_message.decode(codec);
    if (const auto* error = std::get_if<Error>(&decoded_result))
    {
      std::cerr << error->_path << ": " << error->_message << '\n';
      return 1;
    }
    const auto& values = std::get<std::vector<PayloadValue>>(decoded_result);
    std::cout << "Publication producer=" << static_cast<std::uint16_t>(header._publication._producer._task)
              << " incarnation=" << header._publication._producer._incarnation
              << " sequence=" << header._publication._sequence
              << " schema=" << static_cast<std::uint16_t>(header._schema._type) << '.' << header._schema._version
              << " payload_bytes=" << header._payloadBytes << " decoded_fields=" << values.size() << '\n';
    for (const auto& destination : record->_destinations)
    {
      std::cout << "  Accepted Queue=" << static_cast<std::uint16_t>(destination._queue)
                << " sequence=" << destination._queueSequence << '\n';
    }
    ++count;
  }
  std::cout << "Replayed publications: " << count << '\n';

  // Rewind returns to the same frozen input. It neither captures newer traffic
  // nor asks a consumer to redeliver or acknowledge anything.
  auto rewound = cursor.rewind();
  if (const auto* error = std::get_if<Error>(&rewound))
  {
    std::cerr << error->_path << ": " << error->_message << '\n';
    return 1;
  }
  auto first_result = cursor.read_next();
  if (const auto* error = std::get_if<Error>(&first_result))
  {
    std::cerr << error->_path << ": " << error->_message << '\n';
    return 1;
  }
  const auto& repeated = std::get<std::optional<LogRecord>>(first_result);
  if (first_publication && (!repeated || repeated->_message._header._publication != *first_publication))
  {
    std::cerr << "Rewind did not preserve the first recorded publication\n";
    return 1;
  }
  std::cout << "Rewind preserved the captured input\n";

  if (argc == 3)
  {
    // This writes ordinary flat MessageCapture records to the chosen file.
    // Keep the matching catalog for decoding: this file is not a standalone
    // catalog bundle and includes no processing receipts or execution trace.
    // Require a fresh destination so an accidental Session-file path cannot
    // destroy the catalog or message log. Reject dangling symlinks as well.
    // This teaching program assumes no concurrent creator of that output.
    std::error_code path_error;
    const auto status = std::filesystem::symlink_status(argv[2], path_error);
    if (status.type() != std::filesystem::file_type::not_found ||
        (path_error && path_error != std::errc::no_such_file_or_directory))
    {
      std::cerr << "Capture output must be a new file\n";
      return 1;
    }
    std::ofstream output(argv[2], std::ios::binary | std::ios::trunc);
    if (!output)
    {
      std::cerr << "Cannot open capture output\n";
      return 1;
    }
    auto exported = archive.export_capture(output);
    if (const auto* error = std::get_if<Error>(&exported))
    {
      std::cerr << error->_path << ": " << error->_message << '\n';
      return 1;
    }
    output.close();
    if (!output)
    {
      std::cerr << "Closing capture output failed\n";
      return 1;
    }
    std::cout << "Exported flat message capture\n";
  }
}
