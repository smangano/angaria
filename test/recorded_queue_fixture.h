#pragma once

// Private fixed-topology qualification, not public Session configuration. The
// catalog contains real schema/topology definitions; schema file references are
// checked binding metadata supplied by this fixture's offline catalog owner.
#include "producer_log_fixture.h"
#include "recorded_queue.h"
#include "native_file.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <stdexcept>
#include <utility>
#include <variant>

namespace angaria::test_support
{
  // Qualification reads a live control word with native atomic access; it does
  // not decode shared mutable bytes through the ordinary wire helpers.
  inline std::uint64_t recorded_queue_credit(const detail::recorded::RecordedQueueOptions& options)
  {
    auto file = require_value(detail::mapped::NativeFile::open_existing(
        options._connectionPath, detail::mapped::FileAccess::ReadOnly));
    auto mapping = require_value(file.map_read_only(256, 8));
    const auto word = require_value(detail::mapped::AtomicReadWord::bind(require_value(mapping.bytes()), 0));
    return word.load_acquire();
  }

  inline detail::recorded::RecordedQueueOptions recorded_queue_options(
      const std::filesystem::path& directory, std::uint64_t capacity = 2,
      std::uint64_t publications = 128, std::uint64_t fileBytes = 1048576)
  {
    detail::recorded::RecordedQueueOptions options;
    options._producerPaths = producer_log_paths(directory);
    options._connectionPath = directory / "queue.connection";
    options._log = producer_log_options(fileBytes, publications);
    options._log._header._catalogRevision = 9;
    options._log._queues = {QueueId{4}};
    options._queue = QueueId{4};
    options._source = SourceQueueSetId{2};
    options._consumer = TaskId{6};
    options._schema = {MessageType{1}, 1};
    options._schemaReference = 4096;
    options._topologyRevision = 9;
    options._generation = 1;
    options._capacity = capacity;

    auto& catalog = options._catalog;
    catalog._revision = 9;
    catalog._currentRevision = 9;
    const RegistrationMetadata registered{TaskId{3}, UtcTimestamp{}, 1};
    TaskDescriptor producer;
    producer._name = "Producer";
    catalog._tasks.push_back({TaskId{3}, producer, registered});
    TaskDescriptor consumer;
    consumer._name = "Consumer";
    catalog._tasks.push_back({TaskId{6}, consumer, registered});

    SourceQueueSetDescriptor source;
    source._name = "ConsumerInputs";
    source._owner = options._consumer;
    source._selection = SelectionPolicy::Strict;
    source._wait = WaitPolicy::Spin;
    catalog._sources.push_back({options._source, source, registered, EntityState::Active, {options._queue}});
    QueueDescriptor queue;
    queue._name = "ProducerToConsumer";
    queue._producer = TaskId{3};
    queue._sourceSet = options._source;
    queue._ringCapacity = capacity;
    queue._delivery = DeliveryPolicy::RecoverFromLog;
    auto activated = registered;
    activated._revision = options._topologyRevision;
    catalog._queues.push_back({options._queue, queue, activated, EntityState::Active});

    FieldDescriptor field;
    field._id = FieldId{0xee000001};
    field._name = "Value";
    field._base = EFieldBase::BASE_UINT64;
    field._size = 8;
    catalog._fields.push_back({field, registered});
    MessageDescriptor message;
    message._schema = options._schema;
    message._name = "Sample";
    message._fields = {field._id};
    catalog._messages.push_back({message, registered});
    return options;
  }

  inline BinaryPayloadCodec recorded_queue_codec(const detail::recorded::RecordedQueueOptions& options)
  {
    return require_value(BinaryPayloadCodec::from_snapshot(options._catalog, options._schema, 8));
  }

  inline std::array<std::byte, 8> recorded_queue_payload(const BinaryPayloadCodec& codec, std::uint64_t value)
  {
    std::array<std::byte, 8> bytes{};
    const std::array<PayloadValue, 1> fields{value};
    require_value(codec.encode_into(fields, bytes));
    return bytes;
  }

  inline MessageView recorded_queue_message(const detail::recorded::RecordedQueueOptions& options,
                                             std::span<const std::byte> bytes,
                                             std::uint32_t flags = 0xa55a33ccu)
  {
    MessageHeader header;
    header._schema = options._schema;
    header._payloadBytes = bytes.size();
    header._flags = flags;
    return {header, bytes};
  }

  inline detail::recorded::ProducerAppendResult require_recorded_send(
      detail::recorded::RecordedSendResult result)
  {
    if (const auto* error = std::get_if<Error>(&result))
    {
      throw std::runtime_error(error->_path + ": " + error->_message);
    }
    if (std::holds_alternative<detail::recorded::QueueCreditFull>(result))
    {
      throw std::runtime_error("Expected acceptance, but Queue credit is full");
    }
    return std::get<detail::recorded::ProducerAppendResult>(std::move(result));
  }
} // namespace angaria::test_support
