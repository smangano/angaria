// Register a logical participant before it runs, then attach and replace its
// claim. Registration describes a Task; attachment owns its current lifetime.

#include "example_support.h"

#include <exception>
#include <iostream>
#include <stdexcept>
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
    const auto options = primer::session_options(argv[1], "Tasks and claims");
    angaria::TaskDescriptor creator;
    creator._name = "Setup";
    auto created = primer::value(angaria::Session::create(options, creator));
    auto registry = created._creator.registry();

    angaria::TaskDescriptor consumer;
    consumer._name = "Consumer";
    consumer._doc = "Reads application messages";
    const auto consumer_id = primer::value(registry.register_task(consumer));
    const auto retry = primer::value(registry.register_task(consumer));
    std::cout << "Registered Consumer task: " << static_cast<unsigned>(consumer_id) << '\n'
              << "Exact retry kept task ID: " << static_cast<unsigned>(retry) << '\n';

    // Exact registration is safe to repeat. A different descriptor under the
    // same name is an explicit Conflict, not a request to edit the old Task.
    auto changed = consumer;
    changed._doc = "A different description";
    const auto conflict = registry.register_task(changed);
    const auto* conflict_error = std::get_if<angaria::Error>(&conflict);
    if (!conflict_error || conflict_error->_code != angaria::ErrorCode::Conflict)
    {
      throw std::runtime_error("Expected Conflict for the changed Task descriptor");
    }
    std::cout << "Expected Conflict: the registered description stays unchanged\n";

    // A Session Registry can inspect metadata. Mutations need the live Task's
    // actor-bound Registry, which attributes the change to that Task.
    const auto read_only = created._session.registry().register_task(consumer);
    const auto* read_only_error = std::get_if<angaria::Error>(&read_only);
    if (!read_only_error || read_only_error->_code != angaria::ErrorCode::ReadOnlyRegistry)
    {
      throw std::runtime_error("Expected ReadOnlyRegistry for a Session view");
    }
    std::cout << "Expected ReadOnlyRegistry: Session views cannot register Tasks\n";

    {
      auto running = primer::value(created._session.attach_task(consumer_id));
      std::cout << "Consumer incarnation: " << running.claim()._incarnation << '\n';
    } // No endpoint or actor Registry retains this claim, so it closes here.
    auto replacement = primer::value(created._session.attach_task(consumer_id));
    std::cout << "Consumer replacement incarnation: " << replacement.claim()._incarnation << '\n';
    return 0;
  }
  catch (const std::exception& error)
  {
    std::cerr << argv[1] << ": " << error.what() << '\n';
    return 1;
  }
}
