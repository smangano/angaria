// Run these roles as independent programs against one Session directory.
// They share definitions and ownership claims; they do not transfer payloads.
#include "angaria/runtime.h"

#include <cstdint>
#include <exception>
#include <filesystem>
#include <iostream>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace
{
  using namespace angaria;

  OperationResult initialize(const std::filesystem::path& directory)
  {
    TaskDescriptor setup;
    setup._name = "Setup";
    SessionOptions options;
    options._name = "Primer processes";
    options._directory = directory;
    options._maxLogBytes = 1024 * 1024;
    options._limits = CapacityLimits::empty().tasks(8).queues(8)
        .source_sets(4).publishing_sets(4).memberships_per_set(4)
        .fields(16).message_schemas(8);
    options._defaults._ringCapacity = 8;
    auto create_result = Session::create(
        options, setup);
    if (const auto* error = std::get_if<angaria::Error>(&create_result))
    {
      return *error;
    }
    auto created = std::get<angaria::CreatedSession>(std::move(create_result));
    auto registry = created._creator.registry();
    TaskDescriptor producer;
    producer._name = "Feed";
    auto register_task_result = registry.register_task(producer);
    if (const auto* error = std::get_if<angaria::Error>(&register_task_result))
    {
      return *error;
    }
    const auto feed = std::get<angaria::TaskId>(std::move(register_task_result));
    TaskDescriptor consumer;
    consumer._name = "Strategy";
    auto register_task_result_2 = registry.register_task(consumer);
    if (const auto* error = std::get_if<angaria::Error>(&register_task_result_2))
    {
      return *error;
    }
    const auto strategy = std::get<angaria::TaskId>(std::move(register_task_result_2));
    SourceQueueSetDescriptor inputs;
    inputs._name = "StrategyInputs";
    inputs._owner = strategy;
    auto register_source_set_result = registry.register_source_set(inputs);
    if (const auto* error = std::get_if<angaria::Error>(&register_source_set_result))
    {
      return *error;
    }
    const auto source = std::get<angaria::SourceQueueSetId>(std::move(register_source_set_result));
    QueueDescriptor quotes;
    quotes._name = "Quotes";
    quotes._producer = feed;
    quotes._sourceSet = source;
    auto register_queue_result = registry.register_queue(quotes);
    if (const auto* error = std::get_if<angaria::Error>(&register_queue_result))
    {
      return *error;
    }
    const auto queue = std::get<angaria::QueueId>(std::move(register_queue_result));
    PublishingQueueSetDescriptor updates;
    updates._name = "QuoteUpdates";
    updates._owner = feed;
    updates._destinations = {queue};
    auto register_publishing_set_result = registry.register_publishing_set(updates);
    if (const auto* error = std::get_if<angaria::Error>(&register_publishing_set_result))
    {
      return *error;
    }
    const auto publisher = std::get<angaria::PublishingQueueSetId>(std::move(register_publishing_set_result));
    auto activate_source_set_result = registry.activate_source_set(source);
    if (const auto* error = std::get_if<angaria::Error>(&activate_source_set_result))
    {
      return *error;
    }
    auto activate_queue_result = registry.activate_queue(queue);
    if (const auto* error = std::get_if<angaria::Error>(&activate_queue_result))
    {
      return *error;
    }
    auto activate_publishing_set_result = registry.activate_publishing_set(publisher);
    if (const auto* error = std::get_if<angaria::Error>(&activate_publishing_set_result))
    {
      return *error;
    }
    std::cout << "Initialized Feed -> Quotes -> StrategyInputs.\n";
    // Ending this function closes Setup's claim. The registered definitions
    // stay in the Session so later processes can find them by name.
    return std::monostate{};
  }

  OperationResult inspect(const std::filesystem::path& directory)
  {
    auto attach_result = Session::attach(directory);
    if (const auto* error = std::get_if<angaria::Error>(&attach_result))
    {
      return *error;
    }
    auto session = std::get<angaria::Session>(std::move(attach_result));
    auto registry = session.registry();
    auto metadata_result = session.metadata();
    if (const auto* error = std::get_if<angaria::Error>(&metadata_result))
    {
      return *error;
    }
    const auto metadata = std::get<angaria::SessionMetadata>(std::move(metadata_result));
    auto options_result = session.options();
    if (const auto* error = std::get_if<angaria::Error>(&options_result))
    {
      return *error;
    }
    std::cout << "Session=" << std::get<angaria::SessionOptions>(std::move(options_result))._name
              << ", Angaria=" << metadata._angariaVersion << '\n';
    for (const auto* name : {"Feed", "Strategy"})
    {
      auto find_task_result = registry.find_task(name);
      if (const auto* error = std::get_if<angaria::Error>(&find_task_result))
      {
        return *error;
      }
      const auto id = std::get<angaria::TaskId>(std::move(find_task_result));
      auto task_status_result = registry.task_status(id);
      if (const auto* error = std::get_if<angaria::Error>(&task_status_result))
      {
        return *error;
      }
      const auto status = std::get<angaria::TaskRuntimeInfo>(std::move(task_status_result));
      std::cout << name << " attached=" << (status._attached ? "yes" : "no")
                << " incarnation=" << status._incarnation << '\n';
    }
    auto find_source_set_result = registry.find_source_set("StrategyInputs");
    if (const auto* error = std::get_if<angaria::Error>(&find_source_set_result))
    {
      return *error;
    }
    const auto inputs = std::get<angaria::SourceQueueSetId>(std::move(find_source_set_result));
    auto source_members_result = registry.source_members(inputs);
    if (const auto* error = std::get_if<angaria::Error>(&source_members_result))
    {
      return *error;
    }
    std::cout << "StrategyInputs members=" << std::get<std::vector<angaria::QueueId>>(std::move(source_members_result)).size() << '\n';
    // A Session Registry reads definitions without taking a Task claim.
    return std::monostate{};
  }

  void wait_if_requested(bool hold)
  {
    if (hold)
    {
      std::cout << "Press Enter to close this role, or terminate the process.\n";
      std::string line;
      std::getline(std::cin, line);
    }
  }

  OperationResult run_producer(const std::filesystem::path& directory, bool hold)
  {
    auto attach_result_2 = Session::attach(directory);
    if (const auto* error = std::get_if<angaria::Error>(&attach_result_2))
    {
      return *error;
    }
    auto session = std::get<angaria::Session>(std::move(attach_result_2));
    auto registry = session.registry();
    auto find_task_result_2 = registry.find_task("Feed");
    if (const auto* error = std::get_if<angaria::Error>(&find_task_result_2))
    {
      return *error;
    }
    const auto feed = std::get<angaria::TaskId>(std::move(find_task_result_2));
    auto find_queue_result = registry.find_queue("Quotes");
    if (const auto* error = std::get_if<angaria::Error>(&find_queue_result))
    {
      return *error;
    }
    const auto quotes = std::get<angaria::QueueId>(std::move(find_queue_result));
    auto find_publishing_set_result = registry.find_publishing_set("QuoteUpdates");
    if (const auto* error = std::get_if<angaria::Error>(&find_publishing_set_result))
    {
      return *error;
    }
    const auto updates = std::get<angaria::PublishingQueueSetId>(std::move(find_publishing_set_result));
    std::optional<QueueWriter> writer;
    std::optional<PublishingQueueSet> publisher;
    std::uint64_t incarnation{};
    {
      auto attach_task_result = session.attach_task(feed);
      if (const auto* error = std::get_if<angaria::Error>(&attach_task_result))
      {
        return *error;
      }
      auto task = std::get<angaria::Task>(std::move(attach_task_result));
      incarnation = task.claim()._incarnation;
      auto open_writer_result = task.open_writer(quotes);
      if (const auto* error = std::get_if<angaria::Error>(&open_writer_result))
      {
        return *error;
      }
      writer.emplace(std::get<angaria::QueueWriter>(std::move(open_writer_result)));
      auto open_publishing_set_result = task.open_publishing_set(updates);
      if (const auto* error = std::get_if<angaria::Error>(&open_publishing_set_result))
      {
        return *error;
      }
      publisher.emplace(std::get<angaria::PublishingQueueSet>(std::move(open_publishing_set_result)));
      auto next_publication_id_result = task.next_publication_id();
      if (const auto* error = std::get_if<angaria::Error>(&next_publication_id_result))
      {
        return *error;
      }
      const auto publication = std::get<angaria::PublicationId>(std::move(next_publication_id_result));
      std::cout << "Reserved publication sequence=" << publication._sequence << " (not sent).\n";
    }
    // The Task variable has ended, but the endpoint handles retain its claim.
    std::cout << "Writer=" << static_cast<unsigned>(writer->id())
              << ", publisher=" << static_cast<unsigned>(publisher->id()) << '\n';
    std::cout << "READY producer incarnation=" << incarnation << std::endl;
    wait_if_requested(hold);
    return std::monostate{};
  }

  OperationResult run_consumer(const std::filesystem::path& directory, bool hold)
  {
    auto attach_result_3 = Session::attach(directory);
    if (const auto* error = std::get_if<angaria::Error>(&attach_result_3))
    {
      return *error;
    }
    auto session = std::get<angaria::Session>(std::move(attach_result_3));
    auto registry = session.registry();
    auto find_task_result_3 = registry.find_task("Strategy");
    if (const auto* error = std::get_if<angaria::Error>(&find_task_result_3))
    {
      return *error;
    }
    const auto strategy = std::get<angaria::TaskId>(std::move(find_task_result_3));
    auto find_source_set_result_2 = registry.find_source_set("StrategyInputs");
    if (const auto* error = std::get_if<angaria::Error>(&find_source_set_result_2))
    {
      return *error;
    }
    const auto inputs = std::get<angaria::SourceQueueSetId>(std::move(find_source_set_result_2));
    std::optional<SourceQueueSet> source;
    std::uint64_t incarnation{};
    {
      auto attach_task_result_2 = session.attach_task(strategy);
      if (const auto* error = std::get_if<angaria::Error>(&attach_task_result_2))
      {
        return *error;
      }
      auto task = std::get<angaria::Task>(std::move(attach_task_result_2));
      incarnation = task.claim()._incarnation;
      auto open_source_set_result = task.open_source_set(inputs);
      if (const auto* error = std::get_if<angaria::Error>(&open_source_set_result))
      {
        return *error;
      }
      source.emplace(std::get<angaria::SourceQueueSet>(std::move(open_source_set_result)));
    }
    std::cout << "Source=" << static_cast<unsigned>(source->id()) << '\n';
    std::cout << "READY consumer incarnation=" << incarnation << std::endl;
    wait_if_requested(hold);
    return std::monostate{};
  }

  void usage()
  {
    std::cerr << "Usage: angaria_primer_06_processes <init|inspect|producer|consumer> <session-directory> [--hold]\n";
    std::cerr << "Use --hold only with producer or consumer. No messages are transferred yet.\n";
  }
}

int main(int argc, char** argv)
{
  if (argc != 3 && argc != 4)
  {
    usage();
    return 1;
  }
  const std::string command = argv[1];
  const bool hold = argc == 4;
  if (hold && (std::string{argv[3]} != "--hold" || (command != "producer" && command != "consumer")))
  {
    usage();
    return 1;
  }
  try
  {
    angaria::OperationResult operation{std::monostate{}};
    if (command == "init")
    {
      operation = initialize(argv[2]);
    }
    else if (command == "inspect")
    {
      operation = inspect(argv[2]);
    }
    else if (command == "producer")
    {
      operation = run_producer(argv[2], hold);
    }
    else if (command == "consumer")
    {
      operation = run_consumer(argv[2], hold);
    }
    else
    {
      usage();
      return 1;
    }
    if (const auto* error = std::get_if<angaria::Error>(&operation))
    {
      if (error->_code == angaria::ErrorCode::Conflict)
      {
        std::cerr << "Conflict: ";
      }
      else if (error->_code == angaria::ErrorCode::OwnershipBusy)
      {
        std::cerr << "OwnershipBusy: ";
      }
      std::cerr << "Angaria error " << static_cast<int>(error->_code)
                << " at " << error->_path << ": " << error->_message << '\n';
      return 1;
    }
    return 0;
  }
  catch (const std::exception& error)
  {
    std::cerr << argv[2] << ": " << error.what() << '\n';
    return 1;
  }
}
