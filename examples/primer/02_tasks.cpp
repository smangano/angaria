// Register a logical participant before it runs, then attach and replace its
// claim. Registration describes a Task; attachment owns its current lifetime.

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
    angaria::SessionOptions options;
    options._name = "Tasks and claims";
    options._directory = argv[1];
    options._maxLogBytes = 1024 * 1024;
    options._limits = angaria::CapacityLimits::empty().tasks(8).queues(8)
        .source_sets(4).publishing_sets(4).memberships_per_set(4)
        .fields(16).message_schemas(8);
    options._defaults._ringCapacity = 8;
    angaria::TaskDescriptor creator;
    creator._name = "Setup";
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
    auto registry = created._creator.registry();

    angaria::TaskDescriptor consumer;
    consumer._name = "Consumer";
    consumer._doc = "Reads application messages";
    auto consumer_id_result = registry.register_task(consumer);
    if (const auto* error = std::get_if<angaria::Error>(&consumer_id_result))
    {
      std::cerr << "Registering Consumer failed: " << error->_message << " at " << error->_path
                << " (code " << static_cast<int>(error->_code) << ")" << '\n';
      return 1;
    }
    const auto consumer_id = std::get<angaria::TaskId>(std::move(consumer_id_result));
    auto retry_result = registry.register_task(consumer);
    if (const auto* error = std::get_if<angaria::Error>(&retry_result))
    {
      std::cerr << "Retrying Consumer registration failed: " << error->_message << " at " << error->_path
                << " (code " << static_cast<int>(error->_code) << ")" << '\n';
      return 1;
    }
    const auto retry = std::get<angaria::TaskId>(std::move(retry_result));
    std::cout << "Registered Consumer task: " << static_cast<unsigned>(consumer_id) << '\n'
              << "Exact retry kept task ID: " << static_cast<unsigned>(retry) << '\n';

    // Exact registration is safe to repeat. A different descriptor under the
    // same name is an explicit Conflict, not a request to edit the old Task.
    auto changed = consumer;
    changed._doc = "A different description";
    const auto conflict = registry.register_task(changed);
    const auto* conflict_error = std::get_if<angaria::Error>(&conflict);
    if (!conflict_error)
    {
      std::cerr << "Expected Conflict, but the changed Task descriptor was accepted.\n";
      return 1;
    }
    if (conflict_error->_code != angaria::ErrorCode::Conflict)
    {
      std::cerr << "Changed Task registration failed with an unexpected error: "
                << conflict_error->_message << " at " << conflict_error->_path
                << " (code " << static_cast<int>(conflict_error->_code) << ")\n";
      return 1;
    }
    std::cout << "Expected Conflict: the registered description stays unchanged\n";

    // A Session Registry can inspect metadata. Mutations need the live Task's
    // actor-bound Registry, which attributes the change to that Task.
    const auto read_only = created._session.registry().register_task(consumer);
    const auto* read_only_error = std::get_if<angaria::Error>(&read_only);
    if (!read_only_error)
    {
      std::cerr << "Expected ReadOnlyRegistry, but the Session view accepted registration.\n";
      return 1;
    }
    if (read_only_error->_code != angaria::ErrorCode::ReadOnlyRegistry)
    {
      std::cerr << "Session-view registration failed with an unexpected error: "
                << read_only_error->_message << " at " << read_only_error->_path
                << " (code " << static_cast<int>(read_only_error->_code) << ")\n";
      return 1;
    }
    std::cout << "Expected ReadOnlyRegistry: Session views cannot register Tasks\n";

    {
      auto running_result = created._session.attach_task(consumer_id);
      if (const auto* error = std::get_if<angaria::Error>(&running_result))
      {
        std::cerr << "Attaching Consumer failed: " << error->_message << " at " << error->_path
                << " (code " << static_cast<int>(error->_code) << ")" << '\n';
        return 1;
      }
      auto running = std::get<angaria::Task>(std::move(running_result));
      std::cout << "Consumer incarnation: " << running.claim()._incarnation << '\n';
    } // No endpoint or actor Registry retains this claim, so it closes here.
    auto replacement_result = created._session.attach_task(consumer_id);
    if (const auto* error = std::get_if<angaria::Error>(&replacement_result))
    {
      std::cerr << "Replacing Consumer failed: " << error->_message << " at " << error->_path
                << " (code " << static_cast<int>(error->_code) << ")" << '\n';
      return 1;
    }
    auto replacement = std::get<angaria::Task>(std::move(replacement_result));
    std::cout << "Consumer replacement incarnation: " << replacement.claim()._incarnation << '\n';
    return 0;
  }
  catch (const std::exception& error)
  {
    std::cerr << argv[1] << ": " << error.what() << '\n';
    return 1;
  }
}
