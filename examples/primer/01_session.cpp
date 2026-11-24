// Create a Session: the shared directory in which Angaria keeps its Registry
// and resources. Use a new directory so this example never overwrites a Session.

#include "example_support.h"

#include <exception>
#include <iostream>

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
    auto created = primer::value(angaria::Session::create(options, creator));

    const auto recorded = primer::value(created._session.options());
    const auto metadata = primer::value(created._session.metadata());
    auto reader = created._session.registry();
    const auto setup_id = primer::value(reader.find_task("Setup"));
    const auto setup = primer::value(reader.describe_task(setup_id));
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
