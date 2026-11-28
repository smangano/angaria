#pragma once

// Private admission fixture: two distinct connections share one real Session
// identity and directory. The supplied sealed catalog is not public Task
// lifecycle integration, which remains a later workstream step.
#include "recorded_queue_fixture.h"

#include <cstdint>
#include <filesystem>
#include <string>

namespace angaria::test_support
{
  inline detail::recorded::RecordedQueueOptions recorded_independence_options(
      const std::filesystem::path& directory, SessionId session, unsigned connection,
      std::uint64_t publications = 128, bool acknowledged = false)
  {
    auto first = recorded_queue_options(directory, 2, publications);
    first._acknowledged = acknowledged;
    first._log._header._session = session;
    first._producerPaths = producer_log_paths(directory, "producer-a");
    first._connectionPath = directory / "queue-a.connection";

    auto second = recorded_queue_options(directory, 2, publications);
    second._acknowledged = acknowledged;
    second._log._header._session = session;
    second._producerPaths = producer_log_paths(directory, "producer-b");
    second._connectionPath = directory / "queue-b.connection";
    second._log._header._producer = TaskId{4};
    second._log._queues = {QueueId{5}};
    second._queue = QueueId{5};
    second._source = SourceQueueSetId{3};
    second._consumer = TaskId{7};
    auto& catalog = second._catalog;
    catalog._tasks[0]._id = TaskId{4};
    catalog._tasks[0]._descriptor._name = "SecondProducer";
    catalog._tasks[1]._id = TaskId{7};
    catalog._tasks[1]._descriptor._name = "SecondConsumer";
    catalog._sources[0]._id = second._source;
    catalog._sources[0]._descriptor._name = "SecondConsumerInputs";
    catalog._sources[0]._descriptor._owner = second._consumer;
    catalog._sources[0]._members = {second._queue};
    catalog._queues[0]._id = second._queue;
    catalog._queues[0]._descriptor._name = "SecondProducerToConsumer";
    catalog._queues[0]._descriptor._producer = TaskId{4};
    catalog._queues[0]._descriptor._sourceSet = second._source;
    for (auto& task : catalog._tasks)
    {
      task._registration._createdBy = TaskId{4};
    }
    catalog._sources[0]._registration._createdBy = TaskId{4};
    catalog._queues[0]._registration._createdBy = TaskId{4};

    // Both endpoints see the same catalog, with unrelated Tasks/Queues present;
    // isolation is not obtained by inventing a second Session or namespace.
    first._catalog._tasks.insert(first._catalog._tasks.end(), catalog._tasks.begin(), catalog._tasks.end());
    first._catalog._sources.insert(first._catalog._sources.end(), catalog._sources.begin(), catalog._sources.end());
    first._catalog._queues.insert(first._catalog._queues.end(), catalog._queues.begin(), catalog._queues.end());
    second._catalog = first._catalog;
    return connection == 0 ? first : second;
  }

  inline constexpr std::uint64_t independence_message_count = 64;

  inline std::uint64_t independence_payload_value(unsigned connection, std::uint64_t sequence)
  {
    return (connection + 1) * 1000 + sequence;
  }
} // namespace angaria::test_support
