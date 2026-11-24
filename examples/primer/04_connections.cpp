// Register one producer-to-consumer connection and activate its endpoints.
// Two logical Process Tasks run in this one program for a small first example;
// 06_processes.cpp opens their counterparts in independent OS processes.

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
    const auto options = primer::session_options(argv[1], "One connection");
    angaria::TaskDescriptor producer;
    producer._name = "Feed";
    auto created = primer::value(angaria::Session::create(options, producer));
    auto registry = created._creator.registry();
    const auto producer_id = created._creator.claim()._task;
    angaria::TaskDescriptor consumer;
    consumer._name = "Consumer";
    const auto consumer_id = primer::value(registry.register_task(consumer));

    // A consumer owns a Source Queue Set. Its incoming Queue has one producer.
    angaria::SourceQueueSetDescriptor inputs;
    inputs._name = "ConsumerInputs";
    inputs._owner = consumer_id;
    const auto source_id = primer::value(registry.register_source_set(inputs));
    angaria::QueueDescriptor connection;
    connection._name = "FeedToConsumer";
    connection._producer = producer_id;
    connection._sourceSet = source_id;
    const auto queue_id = primer::value(registry.register_queue(connection));

    // A producer owns a Publishing Queue Set listing its destination Queues.
    angaria::PublishingQueueSetDescriptor updates;
    updates._name = "FeedUpdates";
    updates._owner = producer_id;
    updates._destinations = {queue_id};
    const auto publishing_id = primer::value(registry.register_publishing_set(updates));

    // Registration creates Draft entities. Activate in dependency order:
    // Source, then Queue (which allocates its ring), then Publishing set.
    primer::value(registry.activate_source_set(source_id));
    primer::value(registry.activate_queue(queue_id));
    primer::value(registry.activate_publishing_set(publishing_id));

    auto writer = primer::value(created._creator.open_writer(queue_id));
    auto publisher = primer::value(created._creator.open_publishing_set(publishing_id));
    auto running_consumer = primer::value(created._session.attach_task(consumer_id));
    auto source = primer::value(running_consumer.open_source_set(source_id));
    std::cout << "Active Queue: FeedToConsumer (ID " << static_cast<unsigned>(writer.id()) << ")\n"
              << "Active Source: ConsumerInputs (ID " << static_cast<unsigned>(source.id()) << ")\n"
              << "Active Publishing set: FeedUpdates (ID " << static_cast<unsigned>(publisher.id()) << ")\n"
              << "Source member count: " << primer::value(registry.source_members(source_id)).size() << '\n'
              << "Setup only: send/receive are not implemented yet.\n";
    return 0;
  }
  catch (const std::exception& error)
  {
    std::cerr << argv[1] << ": " << error.what() << '\n';
    return 1;
  }
}
