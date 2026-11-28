// Register and activate a producer-to-consumer connection in the Registry.
// Message schemas and actual messaging endpoint openings follow in lesson 14;
// 06_processes.cpp first demonstrates Task claims in independent OS processes.

#include "angaria/runtime.h"

#include <exception>
#include <iostream>
#include <utility>
#include <variant>
#include <vector>

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
    options._name = "One connection";
    options._directory = argv[1];
    options._maxLogBytes = 1024 * 1024;
    options._limits = angaria::CapacityLimits::empty().tasks(8).queues(8)
        .source_sets(4).publishing_sets(4).memberships_per_set(4)
        .fields(16).message_schemas(8);
    options._defaults._ringCapacity = 8;
    angaria::TaskDescriptor producer;
    producer._name = "Feed";
    auto created_result = angaria::Session::create(options, producer);
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
    const auto producer_id = created._creator.claim()._task;
    angaria::TaskDescriptor consumer;
    consumer._name = "Consumer";
    auto consumer_id_result = registry.register_task(consumer);
    if (const auto* error = std::get_if<angaria::Error>(&consumer_id_result))
    {
      std::cerr << "Registering Consumer failed: " << error->_message << " at " << error->_path
                << " (code " << static_cast<int>(error->_code) << ")" << '\n';
      return 1;
    }
    const auto consumer_id = std::get<angaria::TaskId>(std::move(consumer_id_result));

    // A consumer owns a Source Queue Set. Its incoming Queue has one producer.
    angaria::SourceQueueSetDescriptor inputs;
    inputs._name = "ConsumerInputs";
    inputs._owner = consumer_id;
    auto source_id_result = registry.register_source_set(inputs);
    if (const auto* error = std::get_if<angaria::Error>(&source_id_result))
    {
      std::cerr << "Registering ConsumerInputs failed: " << error->_message << " at " << error->_path
                << " (code " << static_cast<int>(error->_code) << ")" << '\n';
      return 1;
    }
    const auto source_id = std::get<angaria::SourceQueueSetId>(std::move(source_id_result));
    angaria::QueueDescriptor connection;
    connection._name = "FeedToConsumer";
    connection._producer = producer_id;
    connection._sourceSet = source_id;
    auto queue_id_result = registry.register_queue(connection);
    if (const auto* error = std::get_if<angaria::Error>(&queue_id_result))
    {
      std::cerr << "Registering FeedToConsumer failed: " << error->_message << " at " << error->_path
                << " (code " << static_cast<int>(error->_code) << ")" << '\n';
      return 1;
    }
    const auto queue_id = std::get<angaria::QueueId>(std::move(queue_id_result));

    // A producer owns a Publishing Queue Set listing its destination Queues.
    angaria::PublishingQueueSetDescriptor updates;
    updates._name = "FeedUpdates";
    updates._owner = producer_id;
    updates._destinations = {queue_id};
    auto publishing_id_result = registry.register_publishing_set(updates);
    if (const auto* error = std::get_if<angaria::Error>(&publishing_id_result))
    {
      std::cerr << "Registering FeedUpdates failed: " << error->_message << " at " << error->_path
                << " (code " << static_cast<int>(error->_code) << ")" << '\n';
      return 1;
    }
    const auto publishing_id = std::get<angaria::PublishingQueueSetId>(std::move(publishing_id_result));

    // Registration creates Draft entities. Activate in dependency order:
    // Source, then Queue, then Publishing set.
    // OperationResult has no success data to extract; checking Error is enough.
    auto source_activation_result = registry.activate_source_set(source_id);
    if (const auto* error = std::get_if<angaria::Error>(&source_activation_result))
    {
      std::cerr << "Activating ConsumerInputs failed: " << error->_message << " at " << error->_path
                << " (code " << static_cast<int>(error->_code) << ")" << '\n';
      return 1;
    }
    auto queue_activation_result = registry.activate_queue(queue_id);
    if (const auto* error = std::get_if<angaria::Error>(&queue_activation_result))
    {
      std::cerr << "Activating FeedToConsumer failed: " << error->_message << " at " << error->_path
                << " (code " << static_cast<int>(error->_code) << ")" << '\n';
      return 1;
    }
    auto publishing_activation_result = registry.activate_publishing_set(publishing_id);
    if (const auto* error = std::get_if<angaria::Error>(&publishing_activation_result))
    {
      std::cerr << "Activating FeedUpdates failed: " << error->_message << " at " << error->_path
                << " (code " << static_cast<int>(error->_code) << ")" << '\n';
      return 1;
    }

    // These are catalog identities, not messaging handles. Complete schema
    // registration before opening endpoints as demonstrated in lesson 14.
    auto members_result = registry.source_members(source_id);
    if (const auto* error = std::get_if<angaria::Error>(&members_result))
    {
      std::cerr << "Reading Source members failed: " << error->_message << " at " << error->_path
                << " (code " << static_cast<int>(error->_code) << ")" << '\n';
      return 1;
    }
    const auto members = std::get<std::vector<angaria::QueueId>>(std::move(members_result));
    std::cout << "Active Queue: FeedToConsumer (ID " << static_cast<unsigned>(queue_id) << ")\n"
              << "Active Source: ConsumerInputs (ID " << static_cast<unsigned>(source_id) << ")\n"
              << "Active Publishing set: FeedUpdates (ID " << static_cast<unsigned>(publishing_id) << ")\n"
              << "Source member count: " << members.size() << '\n'
              << "Setup only: no messages sent. Lesson 14 demonstrates direct send.\n";
    return 0;
  }
  catch (const std::exception& error)
  {
    std::cerr << argv[1] << ": " << error.what() << '\n';
    return 1;
  }
}
