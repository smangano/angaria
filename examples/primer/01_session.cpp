// Create a Session: the shared directory in which Angaria keeps its Registry
// and resources. Use a new directory so this example never overwrites a Session.

#include "angaria/runtime.h"

#include <exception>
#include <iostream>
#include <utility>
#include <variant>

int main(int argc, char** argv)
{
  if (argc != 2)
  {
    std::cerr << "Usage: " << argv[0] << " <new-session-directory>\n";
    return 1;
  }
  try
  {
    // These are upper bounds for this small application, not current counts.
    angaria::SessionOptions options;
    options._name = "First session";
    options._directory = argv[1];
    options._maxLogBytes = 1024 * 1024;
    options._limits = angaria::CapacityLimits::empty().tasks(8).queues(8)
        .source_sets(4).publishing_sets(4).memberships_per_set(4)
        .fields(16).message_schemas(8);
    options._defaults._ringCapacity = 8;

    // Creation also registers and attaches its first logical Process Task.
    angaria::TaskDescriptor creator;
    creator._name = "Setup";
    // Result<CreatedSession> holds either CreatedSession or Error. Check the
    // Error alternative before moving the successful handles out of the result.
    auto created_result = angaria::Session::create(options, creator);
    if (const auto* error = std::get_if<angaria::Error>(&created_result))
    {
      if (error->_code == angaria::ErrorCode::Conflict)
      {
        std::cerr << "Conflict: ";
      }
      std::cerr << "Session creation failed: " << error->_message << " at " << error->_path
                << " (code " << static_cast<int>(error->_code) << ")" << '\n';
      return 1;
    }
    auto created = std::get<angaria::CreatedSession>(std::move(created_result));

    auto recorded_result = created._session.options();
    if (const auto* error = std::get_if<angaria::Error>(&recorded_result))
    {
      std::cerr << "Reading Session options failed: " << error->_message << " at " << error->_path
                << " (code " << static_cast<int>(error->_code) << ")" << '\n';
      return 1;
    }
    const auto recorded = std::get<angaria::SessionOptions>(std::move(recorded_result));
    auto metadata_result = created._session.metadata();
    if (const auto* error = std::get_if<angaria::Error>(&metadata_result))
    {
      std::cerr << "Reading Session metadata failed: " << error->_message << " at " << error->_path
                << " (code " << static_cast<int>(error->_code) << ")" << '\n';
      return 1;
    }
    const auto metadata = std::get<angaria::SessionMetadata>(std::move(metadata_result));
    auto reader = created._session.registry();
    auto setup_id_result = reader.find_task("Setup");
    if (const auto* error = std::get_if<angaria::Error>(&setup_id_result))
    {
      std::cerr << "Finding Setup failed: " << error->_message << " at " << error->_path
                << " (code " << static_cast<int>(error->_code) << ")" << '\n';
      return 1;
    }
    const auto setup_id = std::get<angaria::TaskId>(std::move(setup_id_result));
    auto setup_result = reader.describe_task(setup_id);
    if (const auto* error = std::get_if<angaria::Error>(&setup_result))
    {
      std::cerr << "Describing Setup failed: " << error->_message << " at " << error->_path
                << " (code " << static_cast<int>(error->_code) << ")" << '\n';
      return 1;
    }
    const auto setup = std::get<angaria::TaskDescriptor>(std::move(setup_result));
    std::cout << "Created Session: " << recorded._name << '\n'
              << "Creator Task: " << setup._name << " (ID " << static_cast<unsigned>(setup_id) << ")\n"
              << "Framework version: " << metadata._angariaVersion << '\n'
              << "Task capacity: " << recorded._limits._tasks << '\n';

    // Both handles close at the end of this scope. The directory and Registry
    // remain on disk for another program to attach and inspect.
    return 0;
  }
  catch (const std::exception& error)
  {
    std::cerr << argv[1] << ": " << error.what() << '\n';
    return 1;
  }
}
