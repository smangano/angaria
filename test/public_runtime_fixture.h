#pragma once

// Public application setup for admission tests. Only the temporary-directory
// owner and checked Result extraction come from test plumbing; no fabricated
// catalog, native record offset, or private endpoint is used to send messages.
#include "angaria/runtime.h"
#include "producer_log_fixture.h"
#include "result_helpers.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace angaria::test_support
{
  struct PublicRuntimeConfiguration
  {
    // An absent override demonstrates the public acknowledged default.
    std::optional<AcknowledgmentMode> _mode{};
    std::size_t _outputs{1};
    std::uint64_t _ringCapacity{8};
    std::uint64_t _publications{256};
    std::uint64_t _producerBytes{1048576};
    bool _threadProducer{};
    bool _threadConsumer{};
    bool _loopback{};
  };

  class PublicRuntimeFixture
  {
  public:
    explicit PublicRuntimeFixture(PublicRuntimeConfiguration configuration = {})
    {
      SessionOptions options;
      options._name = "Public messaging admission";
      options._directory = directory();
      options._maxLogBytes = 1048576;
      options._producerLogBytes = configuration._producerBytes;
      options._producerLogPublications = configuration._publications;
      options._limits = CapacityLimits{}.tasks(8).queues(8).source_sets(8).publishing_sets(4)
          .memberships_per_set(4).fields(4).message_schemas(4).symbols(0).string_pool_bytes(0);
      options._defaults._ringCapacity = configuration._ringCapacity;
      _created.emplace(require_value(Session::create(options, TaskDescriptor{"Producer"})));
      auto registry = created()._creator.registry();
      _producer = created()._creator.claim()._task;
      if (configuration._threadProducer)
      {
        TaskDescriptor thread;
        thread._name = "ProducerThread";
        thread._kind = TaskKind::Thread;
        thread._process = created()._creator.claim()._task;
        _producer = require_value(registry.register_task(thread));
      }
      for (std::size_t index = 0; index < configuration._outputs; ++index)
      {
        if (configuration._loopback)
        {
          _consumers.push_back(created()._creator.claim()._task);
        }
        else
        {
          TaskDescriptor consumer;
          consumer._name = "Consumer" + std::to_string(index);
          if (configuration._threadConsumer)
          {
            consumer._kind = TaskKind::Thread;
            consumer._process = created()._creator.claim()._task;
          }
          _consumers.push_back(require_value(registry.register_task(consumer)));
        }
      }
      FieldDescriptor value;
      value._id = FieldId{0xee000001};
      value._name = "Value";
      value._base = EFieldBase::BASE_INT64;
      value._size = 8;
      require_value(registry.register_field(value));
      for (std::uint32_t version = 1; version <= 2; ++version)
      {
        MessageDescriptor sample;
        sample._name = "Sample";
        sample._schema = {MessageType{1}, version};
        sample._fields.assign(version, value._id);
        _schemas.push_back(require_value(registry.register_message(sample)));
      }
      for (const auto schema : _schemas)
      {
        _codecs.push_back(require_value(BinaryPayloadCodec::from_registry(registry, schema, 64)));
      }
      for (std::size_t index = 0; index < configuration._outputs; ++index)
      {
        SourceQueueSetDescriptor input;
        input._name = "Input" + std::to_string(index);
        input._owner = _consumers[index];
        input._acknowledgment = configuration._mode;
        _sources.push_back(require_value(registry.register_source_set(input)));
        QueueDescriptor queue;
        queue._name = "Queue" + std::to_string(index);
        queue._producer = _producer;
        queue._sourceSet = _sources.back();
        queue._ringCapacity = configuration._ringCapacity;
        _queues.push_back(require_value(registry.register_queue(queue)));
      }
      // Complete every definition and activation before the first opening or
      // publication ID. That first messaging operation seals this catalog.
      for (std::size_t index = 0; index < configuration._outputs; ++index)
      {
        require_value(registry.activate_source_set(_sources[index]));
        require_value(registry.activate_queue(_queues[index]));
      }
    }

    CreatedSession& created()
    {
      return *_created;
    }

    std::filesystem::path directory() const
    {
      return _temporary.path() / "session";
    }

    TaskId producer_id() const noexcept
    {
      return _producer;
    }

    const std::vector<TaskId>& consumer_ids() const noexcept
    {
      return _consumers;
    }

    const std::vector<QueueId>& queues() const noexcept
    {
      return _queues;
    }

    const std::vector<SourceQueueSetId>& sources() const noexcept
    {
      return _sources;
    }

    const std::vector<SchemaId>& schemas() const noexcept
    {
      return _schemas;
    }

    const std::vector<BinaryPayloadCodec>& codecs() const noexcept
    {
      return _codecs;
    }

    std::vector<std::byte> payload(std::int64_t value, std::size_t schema = 0) const
    {
      if (schema == 0)
      {
        return require_value(_codecs[0].encode({value}));
      }
      return require_value(_codecs.at(schema).encode({value, value + 1}));
    }

    MessageView message(std::span<const std::byte> bytes, std::size_t schema = 0, std::uint32_t flags = 0x80000001u) const
    {
      MessageHeader header;
      header._schema = _schemas.at(schema);
      header._payloadBytes = bytes.size();
      header._flags = flags;
      return MessageView{header, bytes};
    }

  private:
    ProducerLogDirectory _temporary;
    std::optional<CreatedSession> _created;
    TaskId _producer{};
    std::vector<TaskId> _consumers;
    std::vector<QueueId> _queues;
    std::vector<SourceQueueSetId> _sources;
    std::vector<SchemaId> _schemas;
    std::vector<BinaryPayloadCodec> _codecs;
  };

  inline MessageView public_message(const ReceiveResult& result)
  {
    if (const auto* delivery = std::get_if<Delivery>(&result))
    {
      return delivery->_message;
    }
    if (const auto* message = std::get_if<MessageView>(&result))
    {
      return *message;
    }
    if (const auto* error = std::get_if<Error>(&result))
    {
      throw std::runtime_error(error->_path + ": " + error->_message);
    }
    throw std::runtime_error("Public receive did not deliver a message");
  }
} // namespace angaria::test_support
