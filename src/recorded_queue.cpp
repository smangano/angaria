#include "recorded_queue.h"

#include "angaria/detail/byte_operations.h"
#include "angaria/detail/checked_arithmetic.h"
#include "angaria/detail/error_support.h"
#include "message_retention.h"
#include "native_atomic.h"
#include "native_claim.h"
#include "native_file.h"
#include "runtime_state.h"

#include <algorithm>
#include <array>
#include <limits>
#include <span>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace angaria::detail::recorded
{
  namespace
  {
    namespace mapped = angaria::detail::mapped;
    constexpr std::size_t connection_prefix_bytes = 4096;
    constexpr std::size_t credit_offset = 256;
    constexpr std::size_t consumer_incarnation_offset = 512;
    constexpr std::size_t acknowledged_sequence_offset = 768;
    constexpr std::size_t slot_bytes = 64;
    constexpr std::string_view connection_magic = "ANGACONN";

    std::filesystem::path progress_path(const RecordedQueueOptions& options)
    {
      if (!options._progressPath.empty())
      {
        return options._progressPath;
      }
      auto path = options._connectionPath;
      path += ".progress";
      return path;
    }

    ProgressOptions progress_options(const RecordedQueueOptions& options, mapped::FileIdentity identity)
    {
      return {progress_path(options), options._log._header._session, options._consumer, options._source,
              options._queue, options._generation, identity, options._log._header._publicationCapacity,
              options._log._header._liveAbiProfile};
    }

    template <typename T>
    T value(Result<T> result)
    {
      return value_or_throw<CodecFailure>(std::move(result));
    }

    void require(bool condition, ErrorCode code, std::string_view message)
    {
      codec_require(condition, code, "recorded_queue", message);
    }

    template <typename T, typename Function>
    Result<T> protect(Function&& operation)
    {
      try
      {
        return operation();
      }
      catch (const CodecFailure& failure)
      {
        return failure._error;
      }
      catch (...)
      {
        return runtime_caught_error();
      }
    }

    template <typename Entry, typename Id>
    const Entry& selected_snapshot(const std::vector<Entry>& entries, Id id)
    {
      const Entry* selected{};
      for (const auto& entry : entries)
      {
        if (entry._id == id)
        {
          require(!selected, ErrorCode::ConfigurationMismatch, "Duplicate selected catalog identity");
          selected = &entry;
        }
      }
      require(selected, ErrorCode::ConfigurationMismatch, "Selected identity is absent from the frozen catalog");
      return *selected;
    }

    struct BoundSchema
    {
      SchemaId _schema;
      std::uint64_t _reference;
      BinaryPayloadCodec _codec;
      MessageView _messageTemplate;
    };

    auto schema_key(SchemaId schema)
    {
      return std::pair{static_cast<std::uint16_t>(schema._type), schema._version};
    }

    std::vector<BoundSchema> checked_options(const RecordedQueueOptions& options)
    {
      const auto& header = options._log._header;
      value(encode_producer_header(header));
      value(live_profile_compatibility(header, value(mapped::qualify_native_profile())._liveAbiProfile));
      require(!options._connectionPath.empty() && options._connectionPath != options._producerPaths._log &&
                  options._connectionPath != options._producerPaths._owner,
              ErrorCode::InvalidArgument, "Connection path must be nonempty and separate from producer resources");
      require(options._acknowledged || options._progressPath.empty(), ErrorCode::UnsupportedPolicy,
              "Unacknowledged delivery has no processing progress file");
      if (options._acknowledged)
      {
        const auto path = progress_path(options);
        require(path != options._connectionPath && path != options._producerPaths._log &&
                    path != options._producerPaths._owner,
                ErrorCode::InvalidArgument, "Acknowledged progress must have a separate resource path");
      }
      require(options._queue != QueueId::Invalid && options._source != SourceQueueSetId::Invalid &&
                  options._consumer != TaskId::Invalid &&
                  std::count(options._log._queues.begin(), options._log._queues.end(), options._queue) == 1,
              ErrorCode::UnsupportedPolicy, "Recorded binding requires its outgoing Queue and registered Tasks");
      require(options._capacity != 0 && options._capacity <= 65536 &&
                  options._capacity <= header._publicationCapacity && options._generation != 0 &&
                  options._topologyRevision != 0 &&
                  options._topologyRevision <= header._catalogRevision,
              ErrorCode::InvalidArgument, "Invalid recorded Queue bounds or catalog references");
      require(options._catalog._revision == header._catalogRevision &&
                  options._catalog._currentRevision == header._catalogRevision,
              ErrorCode::ConfigurationMismatch, "Recorded endpoints require the exact sealed catalog revision");
      for (const auto task : {header._producer, options._consumer})
      {
        const auto& definition = selected_snapshot(options._catalog._tasks, task);
        if (definition._descriptor._kind == TaskKind::Thread)
        {
          require(definition._descriptor._process && *definition._descriptor._process != task &&
                      selected_snapshot(options._catalog._tasks, *definition._descriptor._process)._descriptor._kind == TaskKind::Process,
                  ErrorCode::ConfigurationMismatch, "Thread Task requires a registered Process parent");
        }
        else
        {
          require(definition._descriptor._kind == TaskKind::Process && !definition._descriptor._process,
                  ErrorCode::ConfigurationMismatch, "Invalid Process Task definition");
        }
      }
      const auto& queue = selected_snapshot(options._catalog._queues, options._queue);
      const auto& source = selected_snapshot(options._catalog._sources, options._source);
      require(queue._state == EntityState::Active && source._state == EntityState::Active &&
                  queue._descriptor._producer == header._producer && queue._descriptor._sourceSet == options._source &&
                  queue._registration._revision == options._topologyRevision &&
                  source._descriptor._owner == options._consumer && source._members == std::vector{options._queue},
              ErrorCode::ConfigurationMismatch, "Queue/Source differs from the sealed active topology");
      for (const auto outgoing : options._log._queues)
      {
        const auto& definition = selected_snapshot(options._catalog._queues, outgoing);
        require(definition._descriptor._producer == header._producer,
                ErrorCode::ConfigurationMismatch, "Producer log includes another Task's outgoing Queue");
      }
      require((!queue._descriptor._ringCapacity || *queue._descriptor._ringCapacity == options._capacity) &&
                  (!queue._descriptor._delivery || *queue._descriptor._delivery == DeliveryPolicy::RecoverFromLog) &&
                  (!source._descriptor._selection || *source._descriptor._selection == SelectionPolicy::Strict) &&
                  (!source._descriptor._wait || *source._descriptor._wait == WaitPolicy::Spin),
              ErrorCode::UnsupportedPolicy, "Recorded profile requires recover-from-log, strict single-input nonblocking operation");
      auto bindings = options._schemaBindings;
      if (bindings.empty())
      {
        bindings.push_back({options._schema, options._schemaReference});
      }
      else
      {
        require(bindings.size() == options._catalog._messages.size(), ErrorCode::ConfigurationMismatch,
                "Public recorded binding must cache all registered exact schemas");
      }
      std::sort(bindings.begin(), bindings.end(), [](const auto& left, const auto& right)
      {
        return schema_key(left._schema) < schema_key(right._schema);
      });
      std::vector<BoundSchema> schemas;
      schemas.reserve(bindings.size());
      for (const auto& binding : bindings)
      {
        require(binding._reference >= producer_header_bytes && binding._reference % 8 == 0 &&
                    fits_host_size(binding._reference) &&
                    (schemas.empty() || schema_key(schemas.back()._schema) < schema_key(binding._schema)),
                ErrorCode::InvalidArgument, "Invalid or duplicate exact schema binding");
        schemas.push_back({binding._schema, binding._reference,
            value(BinaryPayloadCodec::from_snapshot(options._catalog, binding._schema, header._fileBytes)), {}});
      }
      require(!schemas.empty(), ErrorCode::UnsupportedPolicy, "Recorded transport requires a registered exact schema");
      return schemas;
    }

    std::uint64_t connection_size(const RecordedQueueOptions& options)
    {
      // checked_options has already bounded capacity to 65,536.
      return connection_prefix_bytes + options._capacity * slot_bytes;
    }

    using ConnectionHeader = std::array<std::byte, producer_immutable_bytes>;

    ConnectionHeader connection_header(const RecordedQueueOptions& options, mapped::FileIdentity identity,
                                        const std::vector<BoundSchema>& schemas, mapped::FileIdentity connectionIdentity,
                                        mapped::FileIdentity progressIdentity = {})
    {
      ConnectionHeader bytes{};
      for (std::size_t index = 0; index != connection_magic.size(); ++index)
      {
        bytes[index] = static_cast<std::byte>(static_cast<unsigned char>(connection_magic[index]));
      }
      const auto& log = options._log._header;
      write_unsigned_le<std::uint16_t>(bytes, 8, log_format_major);
      write_unsigned_le<std::uint16_t>(bytes, 10, log_format_minor);
      write_unsigned_le<std::uint32_t>(bytes, 12, connection_prefix_bytes);
      write_unsigned_le<std::uint64_t>(bytes, 16, log._session._high);
      write_unsigned_le<std::uint64_t>(bytes, 24, log._session._low);
      write_unsigned_le<std::uint16_t>(bytes, 32, static_cast<std::uint16_t>(options._queue));
      write_unsigned_le<std::uint16_t>(bytes, 34, static_cast<std::uint16_t>(log._producer));
      write_unsigned_le<std::uint16_t>(bytes, 36, static_cast<std::uint16_t>(options._consumer));
      write_unsigned_le<std::uint16_t>(bytes, 38, static_cast<std::uint16_t>(options._source));
      write_unsigned_le<std::uint64_t>(bytes, 40, log._catalogRevision);
      write_unsigned_le<std::uint16_t>(bytes, 48, static_cast<std::uint16_t>(schemas.front()._schema._type));
      write_unsigned_le<std::uint32_t>(bytes, 52, schemas.front()._schema._version);
      write_unsigned_le<std::uint64_t>(bytes, 56, schemas.front()._reference);
      write_unsigned_le<std::uint64_t>(bytes, 64, options._topologyRevision);
      write_unsigned_le<std::uint64_t>(bytes, 72, options._generation);
      write_unsigned_le<std::uint64_t>(bytes, 80, options._capacity);
      write_unsigned_le<std::uint64_t>(bytes, 88, identity._device);
      write_unsigned_le<std::uint64_t>(bytes, 96, identity._inode);
      write_unsigned_le<std::uint64_t>(bytes, 104, log._fileBytes);
      write_unsigned_le<std::uint64_t>(bytes, 112, log._publicationCapacity);
      write_unsigned_le<std::uint32_t>(bytes, 120, log._liveAbiProfile);
      write_unsigned_le<std::uint16_t>(bytes, 124, log_synchronization_version);
      write_unsigned_le<std::uint16_t>(bytes, 126, 8);
      write_unsigned_le<std::uint64_t>(bytes, 128, schemas.front()._codec.payload_size());
      write_unsigned_le<std::uint64_t>(bytes, 136, options._acknowledged ? 3 : 1);
      write_unsigned_le<std::uint64_t>(bytes, 144, connection_size(options));
      write_unsigned_le<std::uint64_t>(bytes, 152, connectionIdentity._device);
      write_unsigned_le<std::uint64_t>(bytes, 160, connectionIdentity._inode);
      write_unsigned_le<std::uint64_t>(bytes, 168, progressIdentity._device);
      write_unsigned_le<std::uint64_t>(bytes, 176, progressIdentity._inode);
      if (!options._schemaBindings.empty())
      {
        // This binds the complete sealed schema list, not only its first entry.
        // The caller's catalog authority and revision remain the trust boundary.
        std::uint64_t fingerprint = 14695981039346656037ULL;
        for (const auto& schema : schemas)
        {
          const std::array words{static_cast<std::uint64_t>(schema._schema._type),
              static_cast<std::uint64_t>(schema._schema._version), schema._reference, schema._codec.payload_size()};
          for (const auto word : words)
          {
            for (std::size_t byte = 0; byte != 8; ++byte)
            {
              fingerprint ^= static_cast<unsigned char>(word >> (byte * 8));
              fingerprint *= 1099511628211ULL;
            }
          }
        }
        write_unsigned_le<std::uint64_t>(bytes, 184, schemas.size());
        write_unsigned_le<std::uint64_t>(bytes, 192, fingerprint);
      }
      return bytes;
    }

    struct Slot
    {
      mapped::AtomicWord _stamp;
      mapped::AtomicWord _offset;
      mapped::AtomicWord _end;
      mapped::AtomicWord _generation;
    };

    struct Connection
    {
      Connection(const RecordedQueueOptions& options, std::vector<BoundSchema> schemas)
          : _schemas(std::move(schemas)), _session(options._log._header._session), _producer(options._log._header._producer), _queue(options._queue), _source(options._source),
            _consumer(options._consumer),
            _topologyRevision(options._topologyRevision), _generation(options._generation),
            _capacity(options._capacity), _publicationCapacity(options._log._header._publicationCapacity)
      {
      }

      // Consumer lifetime claim closes last, after all mappings and bound data.
      std::optional<mapped::NativeClaim> _claim;
      mapped::NativeFile _file;
      mapped::MappedRegion _mapping;
      std::optional<mapped::AtomicWord> _credit;
      std::optional<mapped::AtomicWord> _incarnation;
      std::optional<mapped::AtomicWord> _acknowledgedSequence;
      std::vector<Slot> _slots;
      std::vector<BoundSchema> _schemas;
      SessionId _session;
      TaskId _producer;
      QueueId _queue;
      SourceQueueSetId _source;
      TaskId _consumer;
      std::uint64_t _topologyRevision;
      std::uint64_t _generation;
      std::uint64_t _capacity;
      std::uint64_t _publicationCapacity;
      std::uint64_t _observedCredit{};
      std::uint64_t _observedAcknowledged{};
      std::thread::id _thread{std::this_thread::get_id()};
      bool _failed{};
      bool _inOperation{};
      bool _threadBound{true};

      void check_thread(bool allowFailed = false) const
      {
        if (_claim)
        {
          value(_claim->check_owner());
        }
        require(!_threadBound || _thread == std::this_thread::get_id(), ErrorCode::WrongOwner, "Recorded endpoint belongs to another thread");
        require(!_inOperation, ErrorCode::InvalidState, "Recorded endpoint cannot reenter an operation");
        require(allowFailed || !_failed, ErrorCode::IoError, "Recorded endpoint requires reopening after validation failure");
      }

      const BoundSchema& schema(SchemaId id, ErrorCode code = ErrorCode::SchemaConflict) const
      {
        // The usual one-schema binding stays one comparison. No codec, Registry
        // snapshot or retention allocation is constructed while messaging.
        if (_schemas.front()._schema == id)
        {
          return _schemas.front();
        }
        const auto selected = std::lower_bound(_schemas.begin(), _schemas.end(), schema_key(id),
            [](const auto& entry, const auto& key)
            {
              return schema_key(entry._schema) < key;
            });
        require(selected != _schemas.end() && selected->_schema == id, code,
                "Message schema is absent from the sealed exact-schema cache");
        return *selected;
      }

      std::uint64_t read_credit(std::uint64_t accepted)
      {
        const auto credit = _credit->load_acquire();
        if (credit < _observedCredit || credit > accepted || credit > _publicationCapacity)
        {
          _failed = true;
          require(false, ErrorCode::IoError, "Returned Queue credit is outside accepted monotonic progress");
        }
        _observedCredit = credit;
        return credit;
      }

      Slot& slot(std::uint64_t sequence)
      {
        return _slots[static_cast<std::size_t>((sequence - 1) % _capacity)];
      }
    };

    std::unique_ptr<Connection> open_connection(const RecordedQueueOptions& options, bool consumer)
    {
      auto connection = std::make_unique<Connection>(options, checked_options(options));
      if (consumer)
      {
        connection->_claim.emplace(value(mapped::NativeClaim::acquire_existing(options._connectionPath)));
      }
      auto log = value(mapped::NativeFile::open_existing(options._producerPaths._log, mapped::FileAccess::ReadOnly,
                                                         options._log._header._fileBytes));
      const auto logIdentity = value(log.identity());
      connection->_file = value(mapped::NativeFile::open_existing(options._connectionPath, mapped::FileAccess::ReadWrite,
                                                                   connection_size(options)));
      const auto connectionIdentity = value(connection->_file.identity());
      mapped::FileIdentity progressIdentity{};
      require(value(connection->_file.size()) == connection_size(options) && connectionIdentity != logIdentity,
              ErrorCode::ConfigurationMismatch, "Recorded connection has an invalid reservation or resource alias");
      ConnectionHeader actual{};
      value(connection->_file.read_exact(0, actual));
      require(std::equal(connection_magic.begin(), connection_magic.end(), actual.begin(), [](char expected, std::byte stored)
              {
                return static_cast<unsigned char>(expected) == std::to_integer<unsigned char>(stored);
              }),
              ErrorCode::IoError, "Invalid recorded connection magic");
      require(read_unsigned_le<std::uint16_t>(actual, 8) == log_format_major &&
                  read_unsigned_le<std::uint16_t>(actual, 10) == log_format_minor &&
                  read_unsigned_le<std::uint32_t>(actual, 120) == options._log._header._liveAbiProfile &&
                  read_unsigned_le<std::uint16_t>(actual, 124) == log_synchronization_version &&
                  read_unsigned_le<std::uint16_t>(actual, 126) == 8 &&
                  read_unsigned_le<std::uint64_t>(actual, 136) == (options._acknowledged ? 3u : 1u),
              ErrorCode::UnsupportedPolicy, "Unsupported recorded connection format, native profile or required features");
      if (options._acknowledged)
      {
        auto progress = value(mapped::NativeFile::open_existing(progress_path(options), mapped::FileAccess::ReadOnly));
        progressIdentity = value(progress.identity());
        require(progressIdentity != connectionIdentity && progressIdentity != logIdentity,
                ErrorCode::ConfigurationMismatch, "Acknowledged progress aliases a connection resource");
      }
      require(actual == connection_header(options, logIdentity, connection->_schemas, connectionIdentity, progressIdentity),
              ErrorCode::ConfigurationMismatch, "Recorded connection differs from its sealed context or generation");
      connection->_mapping = value(connection->_file.map_read_write(0, connection_size(options)));
      const auto bytes = value(connection->_mapping.writable_bytes());
      connection->_credit.emplace(value(mapped::AtomicWord::bind(bytes, credit_offset)));
      connection->_incarnation.emplace(value(mapped::AtomicWord::bind(bytes, consumer_incarnation_offset)));
      if (options._acknowledged)
      {
        connection->_acknowledgedSequence.emplace(value(mapped::AtomicWord::bind(bytes, acknowledged_sequence_offset)));
      }
      connection->_slots.reserve(static_cast<std::size_t>(options._capacity));
      for (std::size_t index = 0; index != options._capacity; ++index)
      {
        const auto offset = connection_prefix_bytes + index * slot_bytes;
        connection->_slots.push_back(Slot{value(mapped::AtomicWord::bind(bytes, offset)),
            value(mapped::AtomicWord::bind(bytes, offset + 8)), value(mapped::AtomicWord::bind(bytes, offset + 16)),
            value(mapped::AtomicWord::bind(bytes, offset + 24))});
      }
      for (auto& schema : connection->_schemas)
      {
        MessageHeader header;
        header._schema = schema._schema;
        schema._messageTemplate = value(MessageAccess::bind(header, {}, connection->_session, schema._codec));
      }
      return connection;
    }

    struct OperationScope
    {
      explicit OperationScope(bool& busy) : _busy(busy)
      {
        _busy = true;
      }
      ~OperationScope()
      {
        _busy = false;
      }
      bool& _busy;
    };

    ProducerAppendResult append_result(const PublicationRecordView& record, bool duplicate)
    {
      return {record._reference, record._recordOrdinal, value(record.destination_at(0))._queueSequence, duplicate};
    }

    std::optional<LogReference> next_hint(Connection& connection, std::uint64_t sequence)
    {
      if (sequence <= connection._observedCredit)
      {
        return std::nullopt;
      }
      const auto& slot = connection.slot(sequence);
      const auto stamp = slot._stamp.load_acquire();
      require(stamp <= sequence, ErrorCode::IoError, "Recorded descriptor skips the next uncredited sequence");
      if (stamp != sequence)
      {
        return std::nullopt;
      }
      require(slot._generation.load_acquire() == connection._generation,
              ErrorCode::IoError, "Recorded descriptor has a stale connection generation");
      return LogReference{connection._session, connection._producer, slot._offset.load_acquire(), slot._end.load_acquire()};
    }

    MessageView bound_message(Connection& connection, const PublicationRecordView& record, TaskClaim actor,
                                std::uint64_t sequence, UtcTimestamp dequeuedAt,
                                const std::optional<LogReference>& hint = {})
    {
      const auto destination = value(record.destination_at(0));
      const auto& schema = connection.schema(record._header._schema, ErrorCode::IoError);
      require(destination._queue == connection._queue && destination._queueSequence == sequence &&
                  record._header._format == PayloadFormat::Binary &&
                  record._schemaReference == schema._reference && record._topologyRevision == connection._topologyRevision &&
                  (!hint || *hint == record._reference),
              ErrorCode::IoError, "Accepted record differs from the next bound Queue delivery");
      value(schema._codec.validate(record._payload));
      auto message = schema._messageTemplate;
      message._header = record._header;
      message._header._queue = connection._queue;
      message._header._queueSequence = sequence;
      message._header._consumer = actor;
      message._header._enqueuedAt = destination._enqueuedAt;
      message._header._dequeuedAt = dequeuedAt;
      message._payload = record._payload;
      return message;
    }

    struct NextQueueRecord
    {
      std::optional<PublicationRecordView> _record;
      bool _exhausted{};
    };

    NextQueueRecord next_queue_record(ProducerLogReader& reader, ProducerReplayCursor& cursor, QueueId queue)
    {
      // Several outgoing Queues share one Task log. Bound each receive attempt
      // while advancing its saved cursor past records for other destinations.
      // The single-Queue path still examines precisely one new record.
      for (std::size_t examined = 0; examined != 32; ++examined)
      {
        auto record = value(reader.poll_next(cursor));
        if (!record)
        {
          return {{}, true};
        }
        if (value(record->destination_at(0))._queue == queue)
        {
          return {std::move(record), false};
        }
      }
      return {{}, false};
    }
  } // namespace

  OperationResult create_recorded_queue(const RecordedQueueOptions& options)
  {
    return protect<std::monostate>([&]
    {
      (void)checked_options(options);
      value(ProducerLog::create(options._producerPaths, options._log));
      value(create_recorded_connection(options));
      return std::monostate{};
    });
  }

  OperationResult create_recorded_connection(const RecordedQueueOptions& options)
  {
    return protect<std::monostate>([&]
    {
      const auto schemas = checked_options(options);
      auto log = value(mapped::NativeFile::open_existing(options._producerPaths._log, mapped::FileAccess::ReadOnly));
      auto file = value(mapped::NativeFile::create_exclusive(options._connectionPath, connection_size(options)));
      value(file.reserve(connection_size(options)));
      const auto connectionIdentity = value(file.identity());
      mapped::FileIdentity progressIdentity{};
      if (options._acknowledged)
      {
        value(ProgressStore::create(progress_options(options, connectionIdentity)));
        auto progress = value(mapped::NativeFile::open_existing(progress_path(options), mapped::FileAccess::ReadOnly));
        progressIdentity = value(progress.identity());
      }
      const auto header = connection_header(options, value(log.identity()), schemas, connectionIdentity, progressIdentity);
      auto mapping = value(file.map_read_write(0, connection_size(options)));
      auto bytes = value(mapping.writable_bytes());
      std::fill(bytes.begin(), bytes.end(), std::byte{});
      std::copy(header.begin(), header.end(), bytes.begin());
      value(mapped::AtomicWord::bind(bytes, credit_offset)).publish_control(0);
      value(mapped::AtomicWord::bind(bytes, consumer_incarnation_offset)).publish_control(0);
      if (options._acknowledged)
      {
        value(mapped::AtomicWord::bind(bytes, acknowledged_sequence_offset)).publish_control(0);
      }
      return std::monostate{};
    });
  }

  struct RecordedProducer::State
  {
    std::unique_ptr<Connection> _connection;
    std::shared_ptr<ProducerLog> _log;
    std::uint64_t _accepted{};
    bool _shared{};
    RecordedQueueObserver* _observer{};

    void check(bool allowFailed = false) const
    {
      value(_log->claim()); // Fork-safe actual-inode ownership, without a syscall.
      _connection->check_thread(allowFailed);
    }

    void publish(const ProducerAppendResult& accepted)
    {
      auto& connection = *_connection;
      if (_observer)
      {
        value(_observer->checkpoint(RecordedQueueCheckpoint::CommittedBeforeHint));
      }
      if (accepted._queueSequence <= connection.read_credit(_accepted))
      {
        return; // Log catch-up or replacement replay already returned credit.
      }
      auto& slot = connection.slot(accepted._queueSequence);
      slot._offset.store_release(accepted._reference._offset);
      slot._end.store_release(accepted._reference._end);
      slot._generation.store_release(connection._generation);
      if (_observer)
      {
        value(_observer->checkpoint(RecordedQueueCheckpoint::DescriptorBeforeStamp));
      }
      slot._stamp.store_release(accepted._queueSequence);
      if (_observer)
      {
        value(_observer->checkpoint(RecordedQueueCheckpoint::DescriptorPublished));
      }
    }
  };

  RecordedProducer::RecordedProducer() noexcept = default;
  RecordedProducer::~RecordedProducer() = default;
  RecordedProducer::RecordedProducer(RecordedProducer&&) noexcept = default;
  RecordedProducer& RecordedProducer::operator=(RecordedProducer&&) noexcept = default;
  RecordedProducer::RecordedProducer(std::unique_ptr<State> state) noexcept : _state(std::move(state))
  {
  }

  Result<RecordedProducer> RecordedProducer::open(const RecordedQueueOptions& options, RecordedQueueObserver* observer)
  {
    return protect<RecordedProducer>([&]
    {
      // Private fixtures keep strict thread affinity. Public Tasks instead
      // retain one shared producer owner and serialize their outgoing endpoints.
      auto connection = open_connection(options, false);
      auto log = std::make_shared<ProducerLog>(value(ProducerLog::open_writer(options._producerPaths, options._log, observer)));
      auto state = std::make_unique<State>();
      state->_connection = std::move(connection);
      state->_log = std::move(log);
      state->_accepted = value(state->_log->queue_sequence(options._queue));
      state->_connection->read_credit(state->_accepted);
      state->_observer = observer;
      return RecordedProducer(std::move(state));
    });
  }

  Result<RecordedProducer> RecordedProducer::open_shared(const RecordedQueueOptions& options,
      std::shared_ptr<ProducerLog> log, RecordedQueueObserver* observer)
  {
    return protect<RecordedProducer>([&]
    {
      require(bool(log), ErrorCode::InvalidArgument, "Shared producer owner is absent");
      const auto actor = value(log->claim());
      require(actor._session == options._log._header._session && actor._task == options._log._header._producer,
              ErrorCode::WrongOwner, "Shared producer owner differs from the sealed binding");
      auto state = std::make_unique<State>();
      state->_connection = open_connection(options, false);
      state->_connection->_threadBound = false;
      state->_log = std::move(log);
      state->_shared = true;
      state->_accepted = value(state->_log->queue_sequence(options._queue));
      state->_connection->read_credit(state->_accepted);
      state->_observer = observer;
      return RecordedProducer(std::move(state));
    });
  }

  Result<TaskClaim> RecordedProducer::claim() const
  {
    return protect<TaskClaim>([&]
    {
      require(bool(_state), ErrorCode::InvalidState, "Recorded producer is empty or moved-from");
      _state->check(true);
      return value(_state->_log->claim());
    });
  }

  Result<PublicationId> RecordedProducer::next_publication_id()
  {
    return protect<PublicationId>([&]
    {
      require(bool(_state), ErrorCode::InvalidState, "Recorded producer is empty or moved-from");
      _state->check();
      return value(_state->_log->next_publication_id());
    });
  }

  RecordedSendResult RecordedProducer::try_send(PublicationId publication, const MessageView& message)
  {
    bool accepted{};
    try
    {
      require(bool(_state), ErrorCode::InvalidState, "Recorded producer is empty or moved-from");
      _state->check();
      auto& state = *_state;
      auto& connection = *state._connection;
      if (state._shared)
      {
        // Another QueueWriter for this same Task/Queue may have accepted since
        // this endpoint last ran. The Task's producer mutex serializes access.
        state._accepted = value(state._log->queue_sequence(connection._queue));
      }
      const auto existing = value(state._log->lookup(publication));
      ProducerAppendRequest request{publication, connection._queue, message,
          0, connection._topologyRevision, {}};
      if (existing)
      {
        request._schemaReference = value(state._log->view(*existing))._schemaReference;
      }
      if (!existing)
      {
        // Credit can delay a fresh acceptance, but cannot revive a retired ID
        // or an unaccepted ID issued by a previous producer claim.
        value(state._log->check_new_publication(publication));
        const auto& schema = connection.schema(message._header._schema);
        request._schemaReference = schema._reference;
        require(message._header._format == PayloadFormat::Binary, ErrorCode::UnsupportedPolicy,
                "Initial recorded Queue profile requires Binary payloads");
        require(message._header._payloadBytes == message._payload.size(), ErrorCode::InvalidArgument,
                "Outgoing payload length is inconsistent");
        value(schema._codec.validate(message._payload));
        const auto credit = connection.read_credit(state._accepted);
        require(state._accepted < std::numeric_limits<std::uint64_t>::max(), ErrorCode::CapacityExceeded,
                "Recorded Queue sequence is exhausted");
        if (state._accepted + 1 - credit > connection._capacity)
        {
          return QueueCreditFull{};
        }
        request._publishedAt = UtcTimestamp{std::chrono::nanoseconds{runtime_now_ns()}};
      }
      OperationScope scope(connection._inOperation);
      auto appended = state._log->append(request);
      const auto* error = std::get_if<Error>(&appended);
      accepted = !error || error->_code == ErrorCode::OutcomeUnknown;
      // Includes an append whose acceptance succeeded before an injected or
      // interrupted return. Reconcile from the authority before another send.
      state._accepted = value(state._log->queue_sequence(connection._queue));
      if (error)
      {
        return *error;
      }
      const auto result = std::get<ProducerAppendResult>(appended);
      state.publish(result);
      return result;
    }
    catch (const CodecFailure& failure)
    {
      return accepted ? Error{ErrorCode::OutcomeUnknown, "Accepted publication requires outcome lookup", "recorded_queue"}
                      : failure._error;
    }
    catch (...)
    {
      return accepted ? Error{ErrorCode::OutcomeUnknown, "Accepted publication requires outcome lookup", "recorded_queue"}
                      : runtime_caught_error();
    }
  }

  Result<std::optional<ProducerAppendResult>> RecordedProducer::lookup(PublicationId publication) const
  {
    return protect<std::optional<ProducerAppendResult>>([&]() -> std::optional<ProducerAppendResult>
    {
      require(bool(_state), ErrorCode::InvalidState, "Recorded producer is empty or moved-from");
      _state->check(true);
      const auto reference = value(_state->_log->lookup(publication));
      return reference ? std::optional{append_result(value(_state->_log->view(*reference)), true)} : std::nullopt;
    });
  }

  Result<std::uint64_t> RecordedProducer::acknowledged_sequence() const
  {
    return protect<std::uint64_t>([&]
    {
      require(bool(_state), ErrorCode::InvalidState, "Recorded producer is empty or moved-from");
      _state->check();
      auto& connection = *_state->_connection;
      if (_state->_shared)
      {
        _state->_accepted = value(_state->_log->queue_sequence(connection._queue));
      }
      require(bool(connection._acknowledgedSequence), ErrorCode::UnsupportedPolicy,
              "Unacknowledged delivery has no processing watermark");
      // Acquire acknowledgment before checking its preceding credit repair.
      const auto acknowledged = connection._acknowledgedSequence->load_acquire();
      const auto credit = connection.read_credit(_state->_accepted);
      require(acknowledged >= connection._observedAcknowledged && acknowledged <= credit,
              ErrorCode::IoError, "Acknowledgment hint exceeds accepted monotonic progress");
      connection._observedAcknowledged = acknowledged;
      return acknowledged;
    });
  }

  struct RecordedConsumer::State
  {
    std::unique_ptr<Connection> _connection;
    ProducerLogReader _reader;
    ProducerReplayCursor _cursor;
    TaskClaim _actor;
    std::uint64_t _nextSequence{1};
    RecordedQueueObserver* _observer{};
  };

  RecordedConsumer::RecordedConsumer() noexcept = default;
  RecordedConsumer::~RecordedConsumer() = default;
  RecordedConsumer::RecordedConsumer(RecordedConsumer&&) noexcept = default;
  RecordedConsumer& RecordedConsumer::operator=(RecordedConsumer&&) noexcept = default;
  RecordedConsumer::RecordedConsumer(std::unique_ptr<State> state) noexcept : _state(std::move(state))
  {
  }

  Result<RecordedConsumer> RecordedConsumer::open(const RecordedQueueOptions& options, RecordedQueueObserver* observer,
                                                 std::optional<TaskClaim> actor)
  {
    return protect<RecordedConsumer>([&]
    {
      require(!options._acknowledged, ErrorCode::UnsupportedPolicy, "Binding requires an acknowledged consumer");
      auto state = std::make_unique<State>();
      state->_connection = open_connection(options, true);
      state->_connection->_threadBound = !actor.has_value();
      state->_reader = value(ProducerLogReader::open(options._producerPaths._log, options._log, !actor.has_value()));
      state->_cursor = value(state->_reader.capture());
      auto& connection = *state->_connection;
      connection.read_credit(value(state->_reader.queue_sequence(connection._queue)));
      const auto previous = connection._incarnation->load_acquire();
      require(actor || previous != std::numeric_limits<std::uint64_t>::max(), ErrorCode::CapacityExceeded,
              "Recorded consumer incarnation is exhausted");
      if (actor)
      {
        require(actor->_session == connection._session && actor->_task == connection._consumer &&
                    actor->_incarnation != 0 && actor->_incarnation >= previous,
                ErrorCode::WrongOwner, "Authoritative consumer claim differs from its binding or precedes its incarnation");
      }
      state->_actor = actor ? *actor : TaskClaim{connection._session, connection._consumer, previous + 1};
      state->_observer = observer;
      connection._incarnation->publish_control(state->_actor._incarnation);
      return RecordedConsumer(std::move(state));
    });
  }

  Result<TaskClaim> RecordedConsumer::claim() const
  {
    return protect<TaskClaim>([&]
    {
      require(bool(_state), ErrorCode::InvalidState, "Recorded consumer is empty or moved-from");
      _state->_connection->check_thread();
      return _state->_actor;
    });
  }

  Result<std::optional<MessageView>> RecordedConsumer::try_receive()
  {
    bool progressed{};
    try
    {
      require(bool(_state), ErrorCode::InvalidState, "Recorded consumer is empty or moved-from");
      auto& state = *_state;
      auto& connection = *state._connection;
      connection.check_thread();
      OperationScope scope(connection._inOperation);
      const auto sequence = state._nextSequence;
      // Returned credit permits slot reuse. Replacement replay at/below that
      // watermark must never inspect a possibly recycling descriptor body.
      const auto hint = next_hint(connection, sequence);
      // Load any matching stamp before acquiring the tail in poll_next: a
      // previously acquired tail may legitimately lag a just-published hint.
      const auto next = next_queue_record(state._reader, state._cursor, connection._queue);
      const auto& record = next._record;
      if (!record)
      {
        require(!next._exhausted || !hint, ErrorCode::IoError, "Recorded descriptor has no accepted record");
        return std::optional<MessageView>{};
      }
      progressed = true;
      auto message = bound_message(connection, *record, state._actor, sequence,
                                    UtcTimestamp{std::chrono::nanoseconds{runtime_now_ns()}}, hint);
      if (state._observer)
      {
        value(state._observer->checkpoint(RecordedQueueCheckpoint::DeliveryBeforeCredit));
      }
      ++state._nextSequence; // Bounded publication capacity prevents wrapping.
      if (sequence > connection._observedCredit)
      {
        require(sequence == connection._observedCredit + 1, ErrorCode::IoError, "Recorded credit would skip a delivery");
        connection._credit->store_release(sequence);
        connection._observedCredit = sequence;
      }
      if (state._observer)
      {
        value(state._observer->checkpoint(RecordedQueueCheckpoint::DeliveryAfterCredit));
      }
      return std::optional{std::move(message)};
    }
    catch (const CodecFailure& failure)
    {
      if (_state && (progressed ||
          (failure._error._code != ErrorCode::WrongOwner && failure._error._code != ErrorCode::InvalidState)))
      {
        _state->_connection->_failed = true;
      }
      return failure._error;
    }
    catch (...)
    {
      if (_state && progressed)
      {
        _state->_connection->_failed = true;
      }
      return runtime_caught_error();
    }
  }
  struct RecordedAcknowledgedConsumer::State
  {
    // Connection ownership outlives the progress claim and all mapped borrows.
    std::unique_ptr<Connection> _connection;
    ProducerLogReader _reader;
    ProgressStore _progress;
    ProducerReplayCursor _cursor;
    ProducerReplayCursor _pinStart;
    TaskClaim _actor;
    std::optional<Delivery> _pinned;
    RecordedQueueObserver* _observer{};
    bool _needsSettle{};

    DeliveryToken token(const ProgressState& source) const
    {
      return {{_connection->_session, _connection->_consumer, source._actorIncarnation},
              _connection->_source, _connection->_queue, source._pinSequence, source._pinIssuance};
    }

    ProgressImage next_image() const
    {
      const auto current = value(_progress.snapshot());
      require(current._generation != std::numeric_limits<std::uint64_t>::max(), ErrorCode::CapacityExceeded,
              "Acknowledged progress generation is exhausted");
      return {current._generation + 1, current._source, 0, {}};
    }

    ProgressReceipt checked_receipt(std::uint64_t sequence)
    {
      auto result = _progress.receipt(sequence);
      if (const auto* error = std::get_if<Error>(&result); error && error->_code == ErrorCode::IoError)
      {
        _connection->_failed = true;
      }
      return value(std::move(result));
    }

    void commit(const ProgressImage& image)
    {
      const auto result = _progress.commit(image);
      if (const auto* error = std::get_if<Error>(&result))
      {
        _needsSettle = error->_code == ErrorCode::OutcomeUnknown;
        throw CodecFailure{*error};
      }
      // Until dependent hints are repaired, a failed return is still uncertain.
      _needsSettle = true;
    }

    void repair_hints(const ProgressState& source)
    {
      auto& connection = *_connection;
      require(source._credit <= value(_reader.queue_sequence(connection._queue)) && source._acknowledged <= source._credit,
              ErrorCode::IoError, "Processing progress exceeds accepted records");
      const auto credit = connection._credit->load_acquire();
      const auto acknowledged = connection._acknowledgedSequence->load_acquire();
      require(credit >= connection._observedCredit && credit <= source._credit &&
                  acknowledged >= connection._observedAcknowledged && acknowledged <= source._acknowledged,
              ErrorCode::IoError, "Connection hints exceed committed processing progress");
      if (credit != source._credit)
      {
        connection._credit->store_release(source._credit);
      }
      connection._observedCredit = source._credit;
      if (acknowledged != source._acknowledged)
      {
        connection._acknowledgedSequence->store_release(source._acknowledged);
      }
      connection._observedAcknowledged = source._acknowledged;
    }

    void settle()
    {
      if (!_needsSettle)
      {
        return;
      }
      value(_progress.recover());
      const auto source = value(_progress.snapshot())._source;
      repair_hints(source);
      if (_pinned)
      {
        if (source._pinSequence == 0)
        {
          if (source._acknowledged < _pinned->_token._sequence)
          {
            _cursor = _pinStart; // A committed release makes this record pending.
          }
          _pinned.reset();
        }
        else
        {
          require(token(source) == _pinned->_token && source._dequeuedAt == _pinned->_message._header._dequeuedAt,
                  ErrorCode::IoError, "Recovered pin differs from its prepared delivery");
        }
      }
      _needsSettle = false;
    }

    template <typename T, typename Function>
    Result<T> run(Function&& operation)
    {
      bool entered{};
      try
      {
        _connection->check_thread();
        OperationScope scope(_connection->_inOperation);
        entered = true;
        settle();
        return operation();
      }
      catch (const CodecFailure& failure)
      {
        return entered && _needsSettle ? Error{ErrorCode::OutcomeUnknown,
            "Processing progress requires settlement before reusing its borrow", "recorded_queue"} : failure._error;
      }
      catch (...)
      {
        return entered && _needsSettle ? Error{ErrorCode::OutcomeUnknown,
            "Processing progress requires settlement before reusing its borrow", "recorded_queue"} : runtime_caught_error();
      }
    }

    void require_pin(const ProgressState& source, DeliveryToken supplied) const
    {
      require(source._pinSequence != 0 && supplied == token(source), ErrorCode::StaleDelivery,
              "Delivery token does not identify this Source's current issuance");
    }

    static void clear_pin(ProgressState& source)
    {
      source._pinSequence = 0;
      source._pinIssuance = 0;
      source._dequeuedAt.reset();
    }

    void release_pin(DeliveryToken supplied)
    {
      require_pin(value(_progress.snapshot())._source, supplied);
      auto image = next_image();
      image._receiptSequence = image._source._pinSequence;
      image._receipt = checked_receipt(image._receiptSequence);
      image._receipt._state = ProgressReceiptState::Pending;
      clear_pin(image._source);
      commit(image);
      repair_hints(image._source);
      _needsSettle = false;
      _cursor = _pinStart;
      _pinned.reset();
    }
  };

  RecordedAcknowledgedConsumer::RecordedAcknowledgedConsumer() noexcept = default;
  RecordedAcknowledgedConsumer::RecordedAcknowledgedConsumer(std::unique_ptr<State> state) noexcept : _state(std::move(state))
  {
  }
  RecordedAcknowledgedConsumer::~RecordedAcknowledgedConsumer()
  {
    (void)close();
  }
  RecordedAcknowledgedConsumer::RecordedAcknowledgedConsumer(RecordedAcknowledgedConsumer&&) noexcept = default;
  RecordedAcknowledgedConsumer& RecordedAcknowledgedConsumer::operator=(RecordedAcknowledgedConsumer&& other) noexcept
  {
    if (this != &other)
    {
      (void)close();
      _state = std::move(other._state);
    }
    return *this;
  }

  Result<RecordedAcknowledgedConsumer> RecordedAcknowledgedConsumer::open(const RecordedQueueOptions& options,
      RecordedQueueObserver* observer, ProgressObserver* progressObserver, std::optional<TaskClaim> actor)
  {
    return protect<RecordedAcknowledgedConsumer>([&]
    {
      require(options._acknowledged, ErrorCode::UnsupportedPolicy, "Binding has no acknowledged progress");
      auto state = std::make_unique<State>();
      state->_connection = open_connection(options, true);
      auto& connection = *state->_connection;
      state->_connection->_threadBound = !actor.has_value();
      state->_reader = value(ProducerLogReader::open(options._producerPaths._log, options._log, !actor.has_value()));
      state->_cursor = value(state->_reader.capture());
      const auto previous = connection._incarnation->load_acquire();
      require(actor || previous != std::numeric_limits<std::uint64_t>::max(), ErrorCode::CapacityExceeded,
              "Recorded consumer incarnation is exhausted");
      if (actor)
      {
        require(actor->_session == connection._session && actor->_task == connection._consumer &&
                    actor->_incarnation != 0 && actor->_incarnation >= previous,
                ErrorCode::WrongOwner, "Authoritative consumer claim differs from its binding or precedes its incarnation");
      }
      state->_actor = actor ? *actor : TaskClaim{connection._session, connection._consumer, previous + 1};
      state->_observer = observer;
      state->_progress = value(ProgressStore::open(progress_options(options, value(connection._file.identity())),
                                                  progressObserver, !actor.has_value()));
      const auto snapshot = value(state->_progress.snapshot());
      require(snapshot._source._actorIncarnation == previous ||
                  (snapshot._source._actorIncarnation > previous &&
                   snapshot._source._actorIncarnation <= state->_actor._incarnation && snapshot._source._issuance == 0 &&
                   snapshot._source._pinSequence == 0),
              ErrorCode::IoError, "Source context differs from persisted consumer incarnation");
      require(snapshot._source._acknowledged <= value(state->_reader.queue_sequence(connection._queue)),
              ErrorCode::IoError, "Acknowledged watermark exceeds accepted records");
      state->repair_hints(snapshot._source);
      if (snapshot._source._actorIncarnation != state->_actor._incarnation || snapshot._source._pinSequence != 0)
      {
        auto image = state->next_image();
        if (image._source._pinSequence)
        {
          image._receiptSequence = image._source._pinSequence;
          image._receipt = state->checked_receipt(image._receiptSequence);
          image._receipt._state = ProgressReceiptState::Pending;
        }
        State::clear_pin(image._source);
        if (image._source._actorIncarnation != state->_actor._incarnation)
        {
          image._source._issuance = 0;
        }
        image._source._actorIncarnation = state->_actor._incarnation;
        state->commit(image);
        state->repair_hints(image._source);
        state->_needsSettle = false;
      }
      // Cold positioning can walk this affected stream. Subsequent messages
      // advance the saved cursor, never restarting at the acknowledged prefix.
      for (std::uint64_t sequence = 1; sequence <= snapshot._source._acknowledged; ++sequence)
      {
        auto record = value(state->_reader.next(state->_cursor));
        while (record && value(record->destination_at(0))._queue != connection._queue)
        {
          record = value(state->_reader.next(state->_cursor));
        }
        require(record && value(record->destination_at(0))._queueSequence == sequence,
                ErrorCode::IoError, "Acknowledged prefix is not present in its producer stream");
      }
      connection._incarnation->publish_control(state->_actor._incarnation);
      return RecordedAcknowledgedConsumer(std::move(state));
    });
  }

  Result<TaskClaim> RecordedAcknowledgedConsumer::claim() const
  {
    return protect<TaskClaim>([&]
    {
      require(bool(_state), ErrorCode::InvalidState, "Acknowledged consumer is empty or closed");
      _state->_connection->check_thread(true);
      return _state->_actor;
    });
  }

  Result<std::optional<Delivery>> RecordedAcknowledgedConsumer::try_receive(std::optional<UtcTimestamp> dequeuedAt)
  {
    if (!_state)
    {
      return Error{ErrorCode::InvalidState, "Acknowledged consumer is empty or closed", "recorded_queue"};
    }
    auto& state = *_state;
    bool prepared{}, validating{};
    ProducerReplayCursor before;
    auto result = state.run<std::optional<Delivery>>([&]() -> std::optional<Delivery>
    {
      const auto snapshot = value(state._progress.snapshot());
      if (snapshot._source._pinSequence)
      {
        require(state._pinned && state._pinned->_token == state.token(snapshot._source),
                ErrorCode::IoError, "Current pin has no bound delivery");
        return state._pinned;
      }
      const auto sequence = snapshot._source._acknowledged + 1;
      before = state._cursor;
      validating = true;
      const auto hint = next_hint(*state._connection, sequence);
      const auto next = next_queue_record(state._reader, state._cursor, state._connection->_queue);
      const auto& record = next._record;
      if (!record)
      {
        require(!next._exhausted || !hint, ErrorCode::IoError, "Recorded descriptor has no accepted record");
        return std::nullopt;
      }
      validating = false;
      prepared = true;
      require(snapshot._generation < std::numeric_limits<std::uint64_t>::max() - 1 &&
                  snapshot._source._issuance != std::numeric_limits<std::uint64_t>::max(),
              ErrorCode::CapacityExceeded, "Fresh issuance must reserve a terminal progress generation");
      ProgressImage image{snapshot._generation + 1, snapshot._source, 0, {}};
      const auto when = dequeuedAt ? *dequeuedAt : UtcTimestamp{std::chrono::nanoseconds{runtime_now_ns()}};
      validating = true;
      auto message = bound_message(*state._connection, *record, state._actor, sequence, when, hint);
      validating = false;
      image._source._issuance += 1;
      image._source._pinSequence = sequence;
      image._source._pinIssuance = image._source._issuance;
      image._source._dequeuedAt = when;
      image._source._credit = std::max(image._source._credit, sequence);
      image._receiptSequence = sequence;
      image._receipt = {ProgressReceiptState::Issued, state._actor._incarnation, image._source._issuance, when, {}};
      state._pinStart = before;
      state._pinned = Delivery{std::move(message), state.token(image._source)};
      state.commit(image);
      if (state._observer)
      {
        value(state._observer->checkpoint(RecordedQueueCheckpoint::DeliveryBeforeCredit));
      }
      state.repair_hints(image._source);
      if (state._observer)
      {
        value(state._observer->checkpoint(RecordedQueueCheckpoint::DeliveryAfterCredit));
      }
      state._needsSettle = false;
      return state._pinned;
    });
    if (std::holds_alternative<Error>(result))
    {
      if (validating)
      {
        state._connection->_failed = true;
      }
      if (prepared && !state._needsSettle)
      {
        state._cursor = before;
        state._pinned.reset();
      }
    }
    return result;
  }

  OperationResult RecordedAcknowledgedConsumer::acknowledge(DeliveryToken supplied, std::optional<UtcTimestamp> acknowledgedAt)
  {
    if (!_state)
    {
      return Error{ErrorCode::InvalidState, "Acknowledged consumer is empty or closed", "recorded_queue"};
    }
    auto& state = *_state;
    return state.run<std::monostate>([&]
    {
      state.require_pin(value(state._progress.snapshot())._source, supplied);
      auto image = state.next_image();
      const auto when = acknowledgedAt ? *acknowledgedAt : UtcTimestamp{std::chrono::nanoseconds{runtime_now_ns()}};
      image._receiptSequence = image._source._pinSequence;
      image._receipt = state.checked_receipt(image._receiptSequence);
      image._receipt._state = ProgressReceiptState::Acknowledged;
      image._receipt._acknowledgedAt = when;
      image._source._acknowledged = image._receiptSequence;
      image._source._lastAcknowledgedAt = when;
      State::clear_pin(image._source);
      state.commit(image);
      if (state._observer)
      {
        value(state._observer->checkpoint(RecordedQueueCheckpoint::AcknowledgmentBeforeHint));
      }
      state.repair_hints(image._source);
      if (state._observer)
      {
        value(state._observer->checkpoint(RecordedQueueCheckpoint::AcknowledgmentAfterHint));
      }
      state._needsSettle = false;
      state._pinned.reset();
      return std::monostate{};
    });
  }

  OperationResult RecordedAcknowledgedConsumer::release(DeliveryToken supplied)
  {
    if (!_state)
    {
      return Error{ErrorCode::InvalidState, "Acknowledged consumer is empty or closed", "recorded_queue"};
    }
    return _state->run<std::monostate>([&]
    {
      _state->release_pin(supplied);
      return std::monostate{};
    });
  }

  OperationResult RecordedAcknowledgedConsumer::close()
  {
    if (!_state)
    {
      return std::monostate{};
    }
    auto result = _state->run<std::monostate>([&]
    {
      const auto source = value(_state->_progress.snapshot())._source;
      if (source._pinSequence)
      {
        _state->release_pin(_state->token(source));
      }
      return std::monostate{};
    });
    if (std::holds_alternative<std::monostate>(result))
    {
      _state.reset(); // End the operation scope before releasing its mappings.
    }
    return result;
  }

  Result<ProgressSnapshot> RecordedAcknowledgedConsumer::progress() const
  {
    return protect<ProgressSnapshot>([&]
    {
      require(bool(_state), ErrorCode::InvalidState, "Acknowledged consumer is empty or closed");
      _state->_connection->check_thread();
      require(!_state->_needsSettle, ErrorCode::OutcomeUnknown, "Processing progress requires settlement");
      return value(_state->_progress.snapshot());
    });
  }

  Result<ProgressReceipt> RecordedAcknowledgedConsumer::receipt(std::uint64_t sequence)
  {
    if (!_state)
    {
      return Error{ErrorCode::InvalidState, "Acknowledged consumer is empty or closed", "recorded_queue"};
    }
    return _state->run<ProgressReceipt>([&]
    {
      const auto cut = sequence <= _state->_cursor.record_count() ? _state->_cursor : value(_state->_reader.capture());
      require(sequence != 0 && sequence <= cut.record_count(), ErrorCode::NotFound, "Queue sequence is not accepted");
      return _state->checked_receipt(sequence);
    });
  }
} // namespace angaria::detail::recorded
