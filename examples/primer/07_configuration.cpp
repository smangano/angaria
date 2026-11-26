// Load the primer's TOML connection instead of spelling out registrations.
// The file creates definitions and Active topology, not message deliveries.
#include "angaria/runtime.h"

#include <exception>
#include <iostream>
#include <utility>
#include <variant>
#include <vector>

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
      auto create_from_file_result = angaria::Session::create_from_file(argv[1], argv[2]);
      if (const auto* error = std::get_if<angaria::Error>(&create_from_file_result))
      {
        if (error->_code == angaria::ErrorCode::Conflict)
        {
          std::cerr << "Conflict: ";
        }
        std::cerr << "Angaria error " << static_cast<int>(error->_code)
                  << " at " << error->_path << ": " << error->_message << '\n';
        return 1;
      }
      auto created = std::get<angaria::CreatedSession>(std::move(create_from_file_result));
      auto registry = created._session.registry();
      auto options_result = created._session.options();
      if (const auto* error = std::get_if<angaria::Error>(&options_result))
      {
        std::cerr << "Angaria error " << static_cast<int>(error->_code)
                  << " at " << error->_path << ": " << error->_message << '\n';
        return 1;
      }
      const auto options = std::get<angaria::SessionOptions>(std::move(options_result));
      auto describe_task_result = registry.describe_task(created._creator.claim()._task);
      if (const auto* error = std::get_if<angaria::Error>(&describe_task_result))
      {
        std::cerr << "Angaria error " << static_cast<int>(error->_code)
                  << " at " << error->_path << ": " << error->_message << '\n';
        return 1;
      }
      const auto creator = std::get<angaria::TaskDescriptor>(std::move(describe_task_result));
      auto find_source_set_result = registry.find_source_set("ConsumerInputs");
      if (const auto* error = std::get_if<angaria::Error>(&find_source_set_result))
      {
        std::cerr << "Angaria error " << static_cast<int>(error->_code)
                  << " at " << error->_path << ": " << error->_message << '\n';
        return 1;
      }
      const auto source = std::get<angaria::SourceQueueSetId>(std::move(find_source_set_result));
      auto find_queue_result = registry.find_queue("FeedToConsumer");
      if (const auto* error = std::get_if<angaria::Error>(&find_queue_result))
      {
        std::cerr << "Angaria error " << static_cast<int>(error->_code)
                  << " at " << error->_path << ": " << error->_message << '\n';
        return 1;
      }
      const auto queue = std::get<angaria::QueueId>(std::move(find_queue_result));
      auto find_publishing_set_result = registry.find_publishing_set("FeedUpdates");
      if (const auto* error = std::get_if<angaria::Error>(&find_publishing_set_result))
      {
        std::cerr << "Angaria error " << static_cast<int>(error->_code)
                  << " at " << error->_path << ": " << error->_message << '\n';
        return 1;
      }
      const auto publishing = std::get<angaria::PublishingQueueSetId>(std::move(find_publishing_set_result));
      auto find_message_result = registry.find_message("Quote", 1);
      if (const auto* error = std::get_if<angaria::Error>(&find_message_result))
      {
        std::cerr << "Angaria error " << static_cast<int>(error->_code)
                  << " at " << error->_path << ": " << error->_message << '\n';
        return 1;
      }
      const auto quote = std::get<angaria::SchemaId>(std::move(find_message_result));
      auto source_members_result = registry.source_members(source);
      if (const auto* error = std::get_if<angaria::Error>(&source_members_result))
      {
        std::cerr << "Angaria error " << static_cast<int>(error->_code)
                  << " at " << error->_path << ": " << error->_message << '\n';
        return 1;
      }
      auto describe_message_result = registry.describe_message(quote);
      if (const auto* error = std::get_if<angaria::Error>(&describe_message_result))
      {
        std::cerr << "Angaria error " << static_cast<int>(error->_code)
                  << " at " << error->_path << ": " << error->_message << '\n';
        return 1;
      }
      auto state_result = registry.state(source);
      if (const auto* error = std::get_if<angaria::Error>(&state_result))
      {
        std::cerr << "Angaria error " << static_cast<int>(error->_code)
                  << " at " << error->_path << ": " << error->_message << '\n';
        return 1;
      }
      auto state_result_2 = registry.state(queue);
      if (const auto* error = std::get_if<angaria::Error>(&state_result_2))
      {
        std::cerr << "Angaria error " << static_cast<int>(error->_code)
                  << " at " << error->_path << ": " << error->_message << '\n';
        return 1;
      }
      auto state_result_3 = registry.state(publishing);
      if (const auto* error = std::get_if<angaria::Error>(&state_result_3))
      {
        std::cerr << "Angaria error " << static_cast<int>(error->_code)
                  << " at " << error->_path << ": " << error->_message << '\n';
        return 1;
      }
      std::cout << "Created from TOML: " << options._name << '\n'
                << "Creator: " << creator._name << '\n'
                << "Session directory: " << options._directory << '\n'
                << "Source members: " << std::get<std::vector<angaria::QueueId>>(std::move(source_members_result)).size() << '\n'
                << "Quote v1 Fields: " << std::get<angaria::MessageDescriptor>(std::move(describe_message_result))._fields.size() << '\n'
                << "Connection active="
                << (std::get<angaria::EntityState>(std::move(state_result)) == angaria::EntityState::Active &&
                    std::get<angaria::EntityState>(std::move(state_result_2)) == angaria::EntityState::Active &&
                    std::get<angaria::EntityState>(std::move(state_result_3)) == angaria::EntityState::Active ? "yes" : "no")
                << '\n';
    } // Closing every creation handle releases the creator's claim.

    // Attachment checks the file's settings against the saved Session. It
    // neither reruns registration nor attaches any of the declared Tasks.
    auto attach_from_file_result = angaria::Session::attach_from_file(argv[1]);
    if (const auto* error = std::get_if<angaria::Error>(&attach_from_file_result))
    {
      std::cerr << "Angaria error " << static_cast<int>(error->_code)
                << " at " << error->_path << ": " << error->_message << '\n';
      return 1;
    }
    auto attached = std::get<angaria::Session>(std::move(attach_from_file_result));
    auto registry = attached.registry();
    auto find_task_result = registry.find_task(argv[2]);
    if (const auto* error = std::get_if<angaria::Error>(&find_task_result))
    {
      std::cerr << "Angaria error " << static_cast<int>(error->_code)
                << " at " << error->_path << ": " << error->_message << '\n';
      return 1;
    }
    const auto creator = std::get<angaria::TaskId>(std::move(find_task_result));
    auto task_status_result = registry.task_status(creator);
    if (const auto* error = std::get_if<angaria::Error>(&task_status_result))
    {
      std::cerr << "Angaria error " << static_cast<int>(error->_code)
                << " at " << error->_path << ": " << error->_message << '\n';
      return 1;
    }
    const auto status = std::get<angaria::TaskRuntimeInfo>(std::move(task_status_result));
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
