#include "angaria/capture.h"
#include "angaria/detail/error_support.h"
#include "angaria/detail/text_escaping.h"
#include "angaria/inspection.h"
#include "angaria/runtime.h"

#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <new>
#include <set>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <variant>

namespace
{
  using namespace angaria;
  using angaria::detail::error_name;

  struct ToolFailure
  {
    Error _error;
  };

  using angaria::detail::write_diagnostic_text;

  [[noreturn]] void fail(ErrorCode code, std::string path, std::string message)
  {
    throw ToolFailure{Error{code, std::move(message), std::move(path)}};
  }

  void usage(std::ostream& output)
  {
    output << "Usage: angaria_inspect <session-directory> <capture-file> [limits]\n"
           << "  --max-record-bytes N   Header plus payload per record (default 1048576)\n"
           << "  --max-capture-bytes N  Total capture bytes (default 67108864)\n"
           << "  --max-text-bytes N     Rendered bytes per record (default 4194304)\n"
           << "  --max-catalog-bytes N  Retained/expanded definition bytes (default 16777216)\n"
           << "  --max-output-bytes N   Total rendered output (default 16777216)\n"
           << "Reads a completed capture through retained native Registry definitions.\n"
           << "Session attachment requires its qualified native ABI and may recover/write\n"
           << "Session state. The capture itself is opened read-only; live tailing and\n"
           << "foreign or physically read-only Session-file inspection are later work.\n";
  }

  struct Options
  {
    std::filesystem::path _session;
    std::filesystem::path _capture;
    InspectionLimits _inspection;
    std::uint64_t _captureBytes{67108864};
    std::uint64_t _outputBytes{16777216};
  };

  std::uint64_t unsigned_value(std::string_view text, std::string_view option)
  {
    std::uint64_t result{};
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), result);
    if (text.empty() || parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size())
    {
      fail(ErrorCode::InvalidArgument, "cli." + std::string{option}, "Expected an unsigned decimal integer");
    }
    return result;
  }

  Options parse_options(int argc, char** argv)
  {
    if (argc < 3)
    {
      fail(ErrorCode::InvalidArgument, "cli", "Expected a Session directory and capture file; use --help");
    }
    Options result;
    result._session = argv[1];
    result._capture = argv[2];
    std::set<std::string_view> seen;
    for (int index = 3; index < argc; index += 2)
    {
      const std::string_view option{argv[index]};
      if (!seen.insert(option).second || index + 1 == argc)
      {
        fail(ErrorCode::InvalidArgument, "cli." + std::string{option}, "A limit needs one value and may be supplied only once");
      }
      const auto limit = unsigned_value(argv[index + 1], option);
      if (option == "--max-record-bytes")
      {
        result._inspection._maxRecordBytes = limit;
      }
      else if (option == "--max-capture-bytes")
      {
        result._captureBytes = limit;
      }
      else if (option == "--max-text-bytes")
      {
        result._inspection._maxTextBytes = limit;
      }
      else if (option == "--max-catalog-bytes")
      {
        result._inspection._maxCatalogBytes = limit;
      }
      else if (option == "--max-output-bytes")
      {
        result._outputBytes = limit;
      }
      else
      {
        fail(ErrorCode::InvalidArgument, "cli." + std::string{option}, "Unknown option");
      }
    }
    return result;
  }

  MessageInspector open_inspector(const Options& options)
  {
    auto session = angaria::detail::value_or_throw<ToolFailure>(Session::attach(options._session));
    return angaria::detail::value_or_throw<ToolFailure>(MessageInspector::from_session(session, options._inspection));
    // The inspector owns its Field/schema definitions. Session attachment and
    // any required recovery finish before capture reading begins.
  }

  void inspect_capture(const Options& options)
  {
    const auto inspector = open_inspector(options);
    std::ifstream input(options._capture, std::ios::binary);
    if (!input)
    {
      fail(ErrorCode::IoError, options._capture.string(), "Cannot open capture for reading");
    }
    auto reader = angaria::detail::value_or_throw<ToolFailure>(MessageCaptureReader::create(input, options._inspection._maxRecordBytes,
                                                     options._captureBytes));
    std::uint64_t outputBytes{};
    for (;;)
    {
      const auto offset = reader.bytes_read();
      const auto index = reader.records_read();
      auto next = angaria::detail::value_or_throw<ToolFailure>(reader.read_next());
      if (!next)
      {
        break;
      }
      auto rendered = inspector.inspect(next->view());
      if (auto* error = std::get_if<Error>(&rendered))
      {
        error->_path = "capture.records[" + std::to_string(index) + "]@" +
            std::to_string(offset) + "." + error->_path;
      }
      const auto text = angaria::detail::value_or_throw<ToolFailure>(std::move(rendered));
      if (text.size() > options._outputBytes - outputBytes ||
          text.size() > static_cast<std::uint64_t>(std::numeric_limits<std::streamsize>::max()))
      {
        fail(ErrorCode::CapacityExceeded, "capture.records[" + std::to_string(index) + "]@" +
             std::to_string(offset) + ".output", "Complete rendered record exceeds the remaining output budget");
      }
      // Previous records may already be printed if a later record fails. Each
      // new record's complete text is checked before any of its bytes are sent.
      std::cout.write(text.data(), static_cast<std::streamsize>(text.size()));
      if (!std::cout)
      {
        fail(ErrorCode::IoError, "cli.stdout", "Cannot write rendered capture output");
      }
      outputBytes += text.size();
    }
    std::cout.flush();
    if (!std::cout)
    {
      fail(ErrorCode::IoError, "cli.stdout", "Cannot flush rendered capture output");
    }
  }
}

int main(int argc, char** argv)
{
  if (argc == 2 && std::string_view{argv[1]} == "--help")
  {
    usage(std::cout);
    return 0;
  }
  try
  {
    inspect_capture(parse_options(argc, argv));
    return 0;
  }
  catch (const ToolFailure& failure)
  {
    std::cerr << error_name(failure._error._code) << " at ";
    write_diagnostic_text(std::cerr, failure._error._path);
    std::cerr << ": ";
    write_diagnostic_text(std::cerr, failure._error._message);
    std::cerr << '\n';
  }
  catch (const std::bad_alloc&)
  {
    std::cerr << "CapacityExceeded at cli.memory: Insufficient process memory\n";
  }
  catch (const std::exception& error)
  {
    std::cerr << "IoError at cli: ";
    write_diagnostic_text(std::cerr, error.what());
    std::cerr << '\n';
  }
  return 1;
}
