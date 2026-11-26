#pragma once

#include "angaria/payload.h"
#include "angaria/runtime.h"
#include "result_helpers.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>

namespace angaria::test_support
{
  // Real public setup shared by Source usage tests and their separate process
  // qualification. No private records, synthetic claims, or profile edits.
  struct SourceScenario
  {
    CreatedSession _created;
    TaskId _consumer{};
    SourceQueueSetId _source{};
    QueueId _queue{};
    SchemaId _schema{};
  };

  inline SourceScenario create_source_scenario(const std::filesystem::path& directory,
                                                  bool activateQueue = true,
                                                  TaskKind consumerKind = TaskKind::Process)
  {
    SessionOptions options;
    options._name = "Source delivery examples";
    options._directory = directory;
    options._maxLogBytes = 2 * 1024 * 1024;
    options._limits = CapacityLimits::empty().tasks(8).queues(8).source_sets(8)
        .memberships_per_set(8).fields(8).message_schemas(8);
    options._defaults._ringCapacity = 2;
    TaskDescriptor producer;
    producer._name = "Producer";
    auto created = require_value(Session::create(options, producer));
    auto registry = created._creator.registry();
    TaskDescriptor consumer;
    consumer._name = "Consumer";
    consumer._kind = consumerKind;
    if (consumerKind == TaskKind::Thread)
    {
      consumer._process = created._creator.claim()._task;
    }
    const auto consumer_id = require_value(registry.register_task(consumer));
    SourceQueueSetDescriptor source;
    source._name = "Inputs";
    source._owner = consumer_id;
    const auto source_id = require_value(registry.register_source_set(source));
    QueueDescriptor queue;
    queue._name = "Updates";
    queue._producer = created._creator.claim()._task;
    queue._sourceSet = source_id;
    const auto queue_id = require_value(registry.register_queue(queue));
    require_value(registry.activate_source_set(source_id));
    if (activateQueue)
    {
      require_value(registry.activate_queue(queue_id));
    }
    FieldDescriptor field;
    field._id = FieldId{0xee000001};
    field._name = "Value";
    field._base = EFieldBase::BASE_UINT32;
    field._size = 4;
    require_value(registry.register_field(field));
    MessageDescriptor message;
    message._name = "Sample";
    message._schema = SchemaId{MessageType{1}, 1};
    message._fields = {field._id};
    const auto schema = require_value(registry.register_message(message));
    return {std::move(created), consumer_id, source_id, queue_id, schema};
  }

  inline BinaryPayloadCodec source_codec(const SourceScenario& scenario)
  {
    return require_value(BinaryPayloadCodec::from_registry(
        scenario._created._session.registry(), scenario._schema, 4));
  }

  inline MessageView source_message(SchemaId schema, std::span<const std::byte> bytes,
                                       PayloadFormat format = PayloadFormat::Binary,
                                       std::uint32_t flags = 0)
  {
    MessageHeader header;
    header._schema = schema;
    header._format = format;
    header._payloadBytes = bytes.size();
    header._flags = flags;
    return {header, bytes};
  }

  inline PublicationId send_source_value(SourceScenario& scenario, QueueWriter& writer,
                                            std::uint32_t value, std::uint32_t flags = 0)
  {
    const auto bytes = require_value(source_codec(scenario).encode({value}));
    const auto id = require_value(scenario._created._creator.next_publication_id());
    const auto result = require_value(writer.send(id, source_message(scenario._schema, bytes,
                                                                       PayloadFormat::Binary, flags)));
    if (result._status != AcceptanceStatus::Accepted)
    {
      throw std::runtime_error("Source scenario expected direct acceptance");
    }
    return id;
  }
} // namespace angaria::test_support
