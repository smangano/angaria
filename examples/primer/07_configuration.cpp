// Load the primer's TOML connection instead of spelling out registrations.
// The file creates definitions and Active topology, not message deliveries.
#include "example_support.h"

#include <exception>
#include <iostream>

int main(int argc, char** argv)
{
  if (argc != 3)
  {
    std::cerr << "Usage: " << argv[0] << " <configuration.toml> <creator-name>\n";
    return 1;
  }
  try
  {
    {
      // The creator must name a Process Task declared by this file. It uses
      // that declaration's capacity entry and receives the initial claim.
      auto created = primer::value(angaria::Session::create_from_file(argv[1], argv[2]));
      auto registry = created._session.registry();
      const auto options = primer::value(created._session.options());
      const auto creator = primer::value(registry.describe_task(created._creator.claim()._task));
      const auto source = primer::value(registry.find_source_set("ConsumerInputs"));
      const auto queue = primer::value(registry.find_queue("FeedToConsumer"));
      const auto publishing = primer::value(registry.find_publishing_set("FeedUpdates"));
      const auto quote = primer::value(registry.find_message("Quote", 1));
      std::cout << "Created from TOML: " << options._name << '\n'
                << "Creator: " << creator._name << '\n'
                << "Session directory: " << options._directory << '\n'
                << "Source members: " << primer::value(registry.source_members(source)).size() << '\n'
                << "Quote v1 Fields: " << primer::value(registry.describe_message(quote))._fields.size() << '\n'
                << "Connection active="
                << (primer::value(registry.state(source)) == angaria::EntityState::Active &&
                    primer::value(registry.state(queue)) == angaria::EntityState::Active &&
                    primer::value(registry.state(publishing)) == angaria::EntityState::Active ? "yes" : "no")
                << '\n';
    } // Closing every creation handle releases the creator's claim.

    // Attachment checks the file's settings against the saved Session. It
    // neither reruns registration nor attaches any of the declared Tasks.
    auto attached = primer::value(angaria::Session::attach_from_file(argv[1]));
    auto registry = attached.registry();
    const auto creator = primer::value(registry.find_task(argv[2]));
    const auto status = primer::value(registry.task_status(creator));
    std::cout << "Reattached from TOML; creator attached=" << (status._attached ? "yes" : "no") << '\n'
              << "No messages were transferred.\n";
    return 0;
  }
  catch (const std::exception& error)
  {
    std::cerr << argv[1] << ": " << error.what() << '\n';
    return 1;
  }
}
