#include "angaria/log.h"

#include "angaria/capture.h"
#include "angaria/detail/checked_arithmetic.h"
#include "angaria/inspection.h"
#include "message_retention.h"
#include "public_runtime_storage.h"

#include <algorithm>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <ostream>
#include <sstream>
#include <string_view>
#include <utility>
#include <vector>

namespace angaria::detail
{
  struct ArchiveSchema
  {
    SchemaId _schema;
    std::uint64_t _reference;
    BinaryPayloadCodec _codec;
    MessageView _messageTemplate;
  };

  struct ArchiveStream
  {
    TaskId _producer;
    recorded::ProducerLogReader _reader;
    recorded::ProducerReplayCursor _cut;
  };

  struct LogArchiveState
  {
    SessionId _session;
    ArchiveLimits _limits;
    std::shared_ptr<const RecordedSessionResources> _resources;
    std::vector<ArchiveSchema> _schemas;
    std::vector<ArchiveStream> _streams;
    std::uint64_t _publications{};
    // Different cursors have independent positions, while sharing read-only
    // mappings and the reader's terminal corruption state.
    std::mutex _readerMutex;
  };

  struct LogCursorState
  {
    std::shared_ptr<LogArchiveState> _archive;
    std::vector<recorded::ProducerReplayCursor> _positions;
    LogFilter _filter;
    std::size_t _stream{};
    bool _failed{};
    std::mutex _mutex;
  };
} // namespace angaria::detail

namespace angaria
{
  namespace
  {
    using namespace detail;

    template <typename T, typename Function>
    Result<T> protect(Function&& operation)
    {
      try
      {
        return operation();
      }
      catch (...)
      {
        return runtime_caught_error();
      }
    }

    void require(bool condition, ErrorCode code, const char* message)
    {
      if (!condition)
      {
        runtime_fail(code, "log_archive", message);
      }
    }

    std::uint64_t bounded_sum(std::uint64_t current, std::uint64_t addition, std::uint64_t limit)
    {
      const auto sum = checked_add(current, addition);
      require(sum && *sum <= limit, ErrorCode::CapacityExceeded, "Archive exceeds its configured reservation limits");
      return *sum;
    }

    const ArchiveSchema& schema(const LogArchiveState& archive, SchemaId id)
    {
      const auto selected = std::find_if(archive._schemas.begin(), archive._schemas.end(), [&](const auto& entry)
      {
        return entry._schema == id;
      });
      require(selected != archive._schemas.end(), ErrorCode::IoError, "Recorded schema is absent from its sealed catalog");
      return *selected;
    }

    bool selected_record(const recorded::PublicationRecordView& record, const LogFilter& filter)
    {
      if (filter._schema && record._header._schema != *filter._schema)
      {
        return false;
      }
      if (filter._queue)
      {
        for (std::size_t index = 0; index != record.destination_count(); ++index)
        {
          if (runtime_value(record.destination_at(index))._queue == *filter._queue)
          {
            return true;
          }
        }
        return false;
      }
      return true;
    }

    LogRecord copy_record(const LogArchiveState& archive, const recorded::PublicationRecordView& record)
    {
      require(record._reference._end >= record._reference._offset &&
                  record._reference._end - record._reference._offset <= archive._limits._maxRecordBytes,
              ErrorCode::CapacityExceeded, "Publication exceeds the archive record-copy limit");
      const auto& cached = schema(archive, record._header._schema);
      require(record._schemaReference == cached._reference && record._header._format == PayloadFormat::Binary,
              ErrorCode::IoError, "Publication differs from its sealed schema binding");
      runtime_value(cached._codec.validate(record._payload));
      auto view = cached._messageTemplate;
      view._header = record._header;
      view._payload = record._payload;
      LogRecord result;
      result._message = runtime_value(view.copy_message(archive._limits._maxRecordBytes));
      result._topologyRevision = record._topologyRevision;
      result._destinations.reserve(record.destination_count());
      for (std::size_t index = 0; index != record.destination_count(); ++index)
      {
        const auto destination = runtime_value(record.destination_at(index));
        const auto found = archive._resources->_queues.find(destination._queue);
        require(found != archive._resources->_queues.end() &&
                    found->second._topologyRevision == record._topologyRevision &&
                    found->second._log._header._producer == record._header._publication._producer._task,
                ErrorCode::IoError, "Publication destination differs from its sealed topology");
        result._destinations.push_back({destination._queue, destination._status, destination._reason,
            destination._queueSequence, destination._enqueuedAt});
      }
      return result;
    }
  } // namespace

  LogArchive::LogArchive(std::shared_ptr<detail::LogArchiveState> state) : _state(std::move(state))
  {
  }

  Result<LogArchive> LogArchive::capture_live(Session& session, ArchiveLimits limits)
  {
    return protect<LogArchive>([&]
    {
      require(bool(session._state), ErrorCode::InvalidState, "Session is empty or moved-from");
      require(limits._maxRecordBytes >= recorded::minimum_publication_bytes && limits._maxRecords != 0 &&
                  limits._maxArchiveBytes >= recorded::producer_header_bytes,
              ErrorCode::InvalidArgument, "Archive limits must permit a complete Publication and producer header");
      auto state = std::make_shared<detail::LogArchiveState>();
      state->_session = session.identity();
      state->_limits = limits;
      {
        RuntimeAccess access(session._state);
        // Zero actor reads only an already published seal. It cannot bootstrap
        // resources or change the application’s Registry/admission boundary.
        state->_resources = runtime_recorded_resources(access, TaskClaim{});
      }
      std::uint64_t capacities{}, fileBytes{};
      for (const auto& [task, producer] : state->_resources->_producers)
      {
        (void)task;
        capacities = bounded_sum(capacities, producer._options._header._publicationCapacity, limits._maxRecords);
        fileBytes = bounded_sum(fileBytes, producer._options._header._fileBytes, limits._maxArchiveBytes);
      }
      // Preflight every reservation before opening any reader, reserving its
      // full index or mapping its data file. Unused/ID-only Task logs count too.
      state->_streams.reserve(state->_resources->_producers.size());
      for (const auto& [task, producer] : state->_resources->_producers)
      {
        auto reader = runtime_value(recorded::ProducerLogReader::open(producer._paths._log, producer._options, false));
        auto cut = runtime_value(reader.capture());
        state->_publications = bounded_sum(state->_publications, cut.record_count(), limits._maxRecords);
        state->_streams.push_back({task, std::move(reader), cut});
      }
      if (!state->_resources->_queues.empty())
      {
        const auto& bindings = state->_resources->_queues.begin()->second._schemaBindings;
        state->_schemas.reserve(bindings.size());
        for (const auto& binding : bindings)
        {
          auto codec = runtime_value(BinaryPayloadCodec::from_snapshot(state->_resources->_catalog,
              binding._schema, limits._maxArchiveBytes));
          MessageHeader header;
          header._schema = binding._schema;
          auto message = runtime_value(MessageAccess::bind(header, {}, state->_session, codec));
          state->_schemas.push_back({binding._schema, binding._reference, std::move(codec), std::move(message)});
        }
      }
      return LogArchive{std::move(state)};
    });
  }

  SessionId LogArchive::session() const noexcept
  {
    return _state ? _state->_session : SessionId{};
  }

  std::uint64_t LogArchive::publication_count() const noexcept
  {
    return _state ? _state->_publications : 0;
  }

  Result<RegistrySnapshot> LogArchive::catalog() const
  {
    return protect<RegistrySnapshot>([&]
    {
      require(bool(_state), ErrorCode::InvalidState, "Log archive is empty or moved-from");
      return _state->_resources->_catalog;
    });
  }

  Result<LogCursor> LogArchive::publications(LogFilter filter) const
  {
    return protect<LogCursor>([&]
    {
      require(bool(_state), ErrorCode::InvalidState, "Log archive is empty or moved-from");
      const auto& catalog = _state->_resources->_catalog;
      require(!filter._producer || _state->_resources->_producers.contains(*filter._producer),
              ErrorCode::NotFound, "Producer filter is absent from the captured catalog");
      require(!filter._schema || std::any_of(catalog._messages.begin(), catalog._messages.end(), [&](const auto& entry)
              {
                return entry._descriptor._schema == *filter._schema;
              }), ErrorCode::NotFound, "Schema filter is absent from the captured catalog");
      require(!filter._queue || std::any_of(catalog._queues.begin(), catalog._queues.end(), [&](const auto& entry)
              {
                return entry._id == *filter._queue;
              }), ErrorCode::NotFound, "Queue filter is absent from the captured catalog");
      auto cursor = std::make_shared<detail::LogCursorState>();
      cursor->_archive = _state;
      cursor->_filter = filter;
      cursor->_positions.reserve(_state->_streams.size());
      for (const auto& stream : _state->_streams)
      {
        cursor->_positions.push_back(stream._cut);
      }
      return LogCursor{std::move(cursor)};
    });
  }

  LogCursor::LogCursor(std::shared_ptr<detail::LogCursorState> state) : _state(std::move(state))
  {
  }
  LogCursor::~LogCursor() = default;
  LogCursor::LogCursor(LogCursor&&) noexcept = default;
  LogCursor& LogCursor::operator=(LogCursor&&) noexcept = default;

  Result<std::optional<LogRecord>> LogCursor::read_next()
  {
    return protect<std::optional<LogRecord>>([&]() -> std::optional<LogRecord>
    {
      require(bool(_state), ErrorCode::InvalidState, "Log cursor is empty or moved-from");
      std::lock_guard cursorLock(_state->_mutex);
      require(!_state->_failed, ErrorCode::InvalidState, "Log cursor is terminal after a validation or copy failure");
      auto& archive = *_state->_archive;
      std::lock_guard readerLock(archive._readerMutex);
      try
      {
        while (_state->_stream < archive._streams.size())
        {
          auto& stream = archive._streams[_state->_stream];
          if (_state->_filter._producer && stream._producer != *_state->_filter._producer)
          {
            ++_state->_stream;
            continue;
          }
          auto record = runtime_value(stream._reader.next(_state->_positions[_state->_stream]));
          if (!record)
          {
            ++_state->_stream;
            continue;
          }
          if (selected_record(*record, _state->_filter))
          {
            return copy_record(archive, *record);
          }
        }
        return std::nullopt;
      }
      catch (...)
      {
        _state->_failed = true;
        throw;
      }
    });
  }

  OperationResult LogCursor::rewind()
  {
    return protect<std::monostate>([&]
    {
      require(bool(_state), ErrorCode::InvalidState, "Log cursor is empty or moved-from");
      std::lock_guard lock(_state->_mutex);
      require(!_state->_failed, ErrorCode::InvalidState, "Failed immutable input cannot be repaired by rewinding");
      for (std::size_t index = 0; index != _state->_positions.size(); ++index)
      {
        _state->_positions[index] = _state->_archive->_streams[index]._cut;
      }
      _state->_stream = 0;
      return std::monostate{};
    });
  }

  OperationResult LogArchive::inspect(std::ostream& output) const
  {
    return protect<std::monostate>([&]
    {
      auto cursor = runtime_value(publications());
      require(output.good(), ErrorCode::IoError, "Log inspection output is not writable");
      std::uint64_t renderedBytes{};
      const auto write_unit = [&](std::string_view text)
      {
        const auto next = checked_add(renderedBytes, static_cast<std::uint64_t>(text.size()));
        require(next && *next <= _state->_limits._maxTextBytes, ErrorCode::CapacityExceeded,
                "Log inspection exceeds the aggregate rendered-text limit");
        output << text;
        require(output.good(), ErrorCode::IoError, "Writing log inspection failed");
        renderedBytes = *next;
      };
      std::ostringstream summary;
      summary << "registry_revision=" << _state->_resources->_catalog._revision
              << " producer_cuts=" << _state->_streams.size() << " publications=" << publication_count() << '\n';
      write_unit(summary.str());
      InspectionLimits inspectionLimits;
      inspectionLimits._maxRecordBytes = _state->_limits._maxRecordBytes;
      inspectionLimits._maxTextBytes = _state->_limits._maxTextBytes;
      auto inspector = runtime_value(MessageInspector::from_snapshot(
          _state->_session, _state->_resources->_catalog, inspectionLimits));
      while (auto record = runtime_value(cursor.read_next()))
      {
        const auto& header = record->_message._header;
        // Keep native acceptance context beside the existing exact-schema
        // inspector. Build and budget the entire record before writing any of
        // it, so a capacity error cannot leave a partial destination/header.
        std::ostringstream unit;
        unit << "producer=" << static_cast<std::uint16_t>(header._publication._producer._task)
             << " incarnation=" << header._publication._producer._incarnation
             << " publication=" << header._publication._sequence
             << " schema=" << static_cast<std::uint16_t>(header._schema._type) << '.' << header._schema._version
             << " bytes=" << header._payloadBytes << " published_ns=" << header._publishedAt.time_since_epoch().count();
        for (const auto& destination : record->_destinations)
        {
          unit << " queue=" << static_cast<std::uint16_t>(destination._queue)
               << " sequence=" << destination._queueSequence;
        }
        unit << '\n' << runtime_value(inspector.inspect(MessageRecordView{header, record->_message._payload}));
        write_unit(unit.str());
      }
      require(output.good(), ErrorCode::IoError, "Writing log inspection failed");
      return std::monostate{};
    });
  }

  OperationResult LogArchive::export_capture(std::ostream& output) const
  {
    return protect<std::monostate>([&]
    {
      require(bool(_state), ErrorCode::InvalidState, "Log archive is empty or moved-from");
      auto writer = runtime_value(MessageCaptureWriter::create(output, _state->_limits._maxRecordBytes,
                                                                 _state->_limits._maxArchiveBytes));
      auto cursor = runtime_value(publications());
      while (auto record = runtime_value(cursor.read_next()))
      {
        runtime_value(writer.append(record->_message._header, record->_message._payload));
      }
      return std::monostate{};
    });
  }
} // namespace angaria
