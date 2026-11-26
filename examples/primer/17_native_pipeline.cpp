// One executable, three application processes. Each role's source shows its
// public Angaria calls directly; this file only validates the command line.
#include "native_pipeline/roles.h"

#include <charconv>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <iostream>
#include <optional>
#include <string_view>
#include <system_error>
#include <variant>

namespace
{
  void print_usage(const char* executable)
  {
    std::cerr << "Usage:\n"
              << "  " << executable << " init CONFIG\n"
              << "  " << executable << " feed CONFIG FIRST COUNT [--hold]\n"
              << "  " << executable << " strategy CONFIG COUNT [--hold-first]\n"
              << "  " << executable << " audit CONFIG COUNT\n"
              << "  " << executable << " status CONFIG Feed|Strategy INCARNATION SEQUENCE\n"
              << "Quote numbers and counts must be in 1.." << native_pipeline::max_quote_number << ".\n";
  }

  std::optional<std::uint64_t> positive_number(std::string_view text)
  {
    std::uint64_t value{};
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
    if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size() || value == 0)
    {
      return std::nullopt;
    }
    return value;
  }

  angaria::OperationResult run_command(int argc, char** argv)
  {
    using namespace angaria;
    const Error invalid{ErrorCode::InvalidArgument, "Invalid command or arguments", "pipeline.arguments"};
    if (argc < 3)
    {
      return invalid;
    }
    const std::string_view command{argv[1]};
    const std::filesystem::path configuration{argv[2]};
    if (command == "init" && argc == 3)
    {
      return native_pipeline::initialize(configuration);
    }
    if (command == "feed" && (argc == 5 || argc == 6))
    {
      const auto first = positive_number(argv[3]);
      const auto count = positive_number(argv[4]);
      if (!first || !count || *first > native_pipeline::max_quote_number ||
          *count > native_pipeline::max_quote_number - *first + 1 ||
          (argc == 6 && std::string_view{argv[5]} != "--hold"))
      {
        return invalid;
      }
      return native_pipeline::publish_quotes(configuration, static_cast<std::uint32_t>(*first),
          static_cast<std::uint32_t>(*count), argc == 6);
    }
    if ((command == "strategy" && (argc == 4 || argc == 5)) || (command == "audit" && argc == 4))
    {
      const auto count = positive_number(argv[3]);
      if (!count || *count > native_pipeline::max_quote_number ||
          (argc == 5 && std::string_view{argv[4]} != "--hold-first"))
      {
        return invalid;
      }
      if (command == "strategy")
      {
        return native_pipeline::process_quotes(configuration, static_cast<std::uint32_t>(*count), argc == 5);
      }
      return native_pipeline::audit_alerts(configuration, static_cast<std::uint32_t>(*count));
    }
    if (command == "status" && argc == 6)
    {
      const std::string_view task{argv[3]};
      const auto incarnation = positive_number(argv[4]);
      const auto sequence = positive_number(argv[5]);
      if (!incarnation || !sequence || (task != "Feed" && task != "Strategy"))
      {
        return invalid;
      }
      return native_pipeline::publication_status(configuration, task, *incarnation, *sequence);
    }
    return invalid;
  }
}

int main(int argc, char** argv)
{
  try
  {
    const auto result = run_command(argc, argv);
    if (const auto* error = std::get_if<angaria::Error>(&result))
    {
      if (error->_path == "pipeline.arguments")
      {
        print_usage(argv[0]);
      }
      // These are the expected actionable errors in this walkthrough. Keep
      // unfamiliar failures' original numeric code, path, and message too.
      if (error->_code == angaria::ErrorCode::Conflict)
      {
        std::cerr << "Conflict: ";
      }
      else if (error->_code == angaria::ErrorCode::OwnershipBusy)
      {
        std::cerr << "OwnershipBusy: ";
      }
      else if (error->_code == angaria::ErrorCode::InvalidArgument)
      {
        std::cerr << "InvalidArgument: ";
      }
      else if (error->_code == angaria::ErrorCode::OutcomeUnknown)
      {
        std::cerr << "OutcomeUnknown: ";
      }
      std::cerr << error->_path << ": " << error->_message
                << " (code " << static_cast<int>(error->_code) << ")\n";
      return 1;
    }
    return 0;
  }
  catch (const std::exception& error)
  {
    std::cerr << "native_pipeline: " << error.what() << '\n';
    return 1;
  }
}
