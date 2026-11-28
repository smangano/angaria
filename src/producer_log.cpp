#include "producer_log.h"

#include "angaria/detail/byte_operations.h"
#include "angaria/detail/checked_arithmetic.h"
#include "angaria/detail/error_support.h"
#include "native_atomic.h"
#include "native_claim.h"
#include "native_file.h"

#include <algorithm>
#include <array>
#include <limits>
#include <new>
#include <stdexcept>
#include <string_view>
#include <thread>
#include <tuple>
#include <type_traits>
#include <utility>
#include <variant>

#include <unistd.h>

namespace angaria::detail::recorded
{
  namespace
  {
    namespace mapped = angaria::detail::mapped;
    constexpr std::size_t owner_file_bytes = 4096;
    constexpr std::size_t owner_incarnation_offset = 256;
    constexpr std::string_view owner_magic = "ANGAOWNR";

    template <typename T>
    T value(Result<T> result)
    {
      return value_or_throw<CodecFailure>(std::move(result));
    }

    void require(bool condition, ErrorCode code, std::string_view message)
    {
      codec_require(condition, code, "producer_log", message);
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
      catch (const std::bad_alloc&)
      {
        return Error{ErrorCode::CapacityExceeded, "Cannot allocate producer log state", "producer_log"};
      }
      catch (const std::length_error&)
      {
        return Error{ErrorCode::CapacityExceeded, "Producer log state exceeds host capacity", "producer_log"};
      }
    }

    struct QueueProgress
    {
      QueueId _queue{};
      std::uint64_t _sequence{};
    };

    struct IndexEntry
    {
      PublicationId _publication;
      LogReference _reference;
      std::uint64_t _ordinal{};
      std::uint64_t _queueSequence{};
    };
    static_assert(std::is_nothrow_copy_constructible_v<IndexEntry>);

    auto publication_key(PublicationId publication)
    {
      return std::pair{publication._producer._incarnation, publication._sequence};
    }

    struct LogData
    {
      mapped::NativeFile _file;
      mapped::MappedRegion _mapping;
      std::span<const std::byte> _bytes;
      std::optional<mapped::AtomicReadWord> _tail;
      ProducerLogHeader _header;
      mapped::FileIdentity _identity;
      std::vector<QueueProgress> _queues;
      std::vector<IndexEntry> _records;
      std::uint64_t _end{producer_header_bytes};
      bool _failed{};
    };

    std::vector<QueueProgress> checked_queues(const ProducerLogOptions& options)
    {
      value(encode_producer_header(options._header));
      require(!options._queues.empty() && options._queues.size() <= std::numeric_limits<std::uint16_t>::max(),
              ErrorCode::InvalidArgument, "Producer requires a bounded nonempty outgoing Queue list");
      std::vector<QueueProgress> queues;
      queues.reserve(options._queues.size());
      for (const auto queue : options._queues)
      {
        require(queue != QueueId::Invalid, ErrorCode::InvalidArgument, "Invalid outgoing Queue identity");
        queues.push_back(QueueProgress{queue, 0});
      }
      std::sort(queues.begin(), queues.end(), [](const auto& left, const auto& right)
      {
        return left._queue < right._queue;
      });
      require(std::adjacent_find(queues.begin(), queues.end(), [](const auto& left, const auto& right)
      {
        return left._queue == right._queue;
      }) == queues.end(), ErrorCode::InvalidArgument, "Duplicate outgoing Queue identity");
      return queues;
    }

    QueueProgress& queue_progress(LogData& data, QueueId queue, ErrorCode code)
    {
      const auto found = std::lower_bound(data._queues.begin(), data._queues.end(), queue,
          [](const auto& entry, QueueId id)
          {
            return entry._queue < id;
          });
      require(found != data._queues.end() && found->_queue == queue, code, "Queue is not owned by this producer");
      return *found;
    }

    const IndexEntry* find_publication(const LogData& data, PublicationId publication)
    {
      const auto key = publication_key(publication);
      // New issued IDs increase monotonically. Their ordinary append path need
      // not search the historical index; retries still use bounded lookup.
      if (data._records.empty() || publication_key(data._records.back()._publication) < key)
      {
        return nullptr;
      }
      const auto found = std::lower_bound(data._records.begin(), data._records.end(), key,
          [](const auto& entry, const auto& wanted)
          {
            return publication_key(entry._publication) < wanted;
          });
      return found != data._records.end() && publication_key(found->_publication) == key ? &*found : nullptr;
    }

    void check_publication_identity(const LogData& data, PublicationId publication)
    {
      require(publication._producer._session == data._header._session &&
                  publication._producer._task == data._header._producer &&
                  publication._producer._incarnation != 0 && publication._sequence != 0,
              ErrorCode::InvalidArgument, "Publication belongs to another producer or is unissued");
    }

    PublicationRecordView decode_reference(const LogData& data, const LogReference& reference)
    {
      return value(decode_publication_record(
          data._bytes.subspan(static_cast<std::size_t>(reference._offset),
                              static_cast<std::size_t>(reference._end - reference._offset)),
          data._header, reference));
    }

    PublicationRecordView indexed_view(const LogData& data, const LogReference& reference)
    {
      require(!data._failed, ErrorCode::IoError, "Producer log requires reopening after validation failure");
      const auto found = std::lower_bound(data._records.begin(), data._records.end(), reference._offset,
          [](const auto& entry, std::uint64_t offset)
          {
            return entry._reference._offset < offset;
          });
      require(found != data._records.end() && found->_reference == reference,
              ErrorCode::NotFound, "Reference is not an indexed committed record boundary");
      return decode_reference(data, reference);
    }

    std::uint64_t acquired_end(const LogData& data)
    {
      require(!data._failed, ErrorCode::IoError, "Producer log requires reopening after validation failure");
      const auto end = data._tail->load_acquire();
      value(validate_committed_tail(data._header, end));
      require(end >= data._end, ErrorCode::IoError, "Producer committed tail moved backward");
      return end;
    }

    void refresh(LogData& data)
    {
      try
      {
        const auto cut = acquired_end(data);
        while (data._end != cut)
        {
          require(data._records.size() < data._header._publicationCapacity &&
                      contains_range(cut, data._end, publication_fixed_bytes),
                  ErrorCode::IoError, "Committed prefix exceeds its count or record bounds");
          const auto bytes = read_unsigned_le<std::uint64_t>(data._bytes, static_cast<std::size_t>(data._end + 8));
          require(bytes >= minimum_publication_bytes && bytes % publication_alignment == 0 &&
                      contains_range(cut, data._end, bytes),
                  ErrorCode::IoError, "Malformed record in the committed prefix");
          const LogReference reference{data._header._session, data._header._producer, data._end, data._end + bytes};
          const auto record = decode_reference(data, reference);
          require(record._recordOrdinal == data._records.size() + 1,
                  ErrorCode::IoError, "Committed record ordinals are not contiguous");
          require(record.destination_count() == 1 && record._publishingSet == PublishingQueueSetId::Invalid &&
                      record._acceptance == AcceptancePolicy::AllDestinations,
                  ErrorCode::UnsupportedPolicy, "Producer storage currently admits direct point-to-point records only");
          if (!data._records.empty())
          {
            require(publication_key(data._records.back()._publication) < publication_key(record._header._publication),
                    ErrorCode::IoError, "Committed publication identities are not ordered and unique");
          }
          const auto destination = value(record.destination_at(0));
          auto& queue = queue_progress(data, destination._queue, ErrorCode::IoError);
          require(destination._status == DestinationStatus::Accepted &&
                      destination._queueSequence == queue._sequence + 1,
                  ErrorCode::IoError, "Committed Queue sequences are not contiguous");
          data._records.push_back(IndexEntry{record._header._publication, reference,
                                            record._recordOrdinal, destination._queueSequence});
          queue._sequence = destination._queueSequence;
          data._end = reference._end;
        }
      }
      catch (...)
      {
        // Never salvage a malformed accepted prefix by truncating its tail.
        data._failed = true;
        throw;
      }
    }

    LogData open_data(const std::filesystem::path& path, const ProducerLogOptions& options, bool writable)
    {
      LogData data;
      data._header = options._header;
      data._queues = checked_queues(options);
      const auto profile = value(mapped::qualify_native_profile());
      value(live_profile_compatibility(data._header, profile._liveAbiProfile));
      data._file = value(mapped::NativeFile::open_existing(path,
          writable ? mapped::FileAccess::ReadWrite : mapped::FileAccess::ReadOnly, data._header._fileBytes));
      require(value(data._file.size()) == data._header._fileBytes, ErrorCode::ConfigurationMismatch,
              "Producer file size differs from its fixed reservation");
      ProducerLogHeaderBytes bytes{};
      value(data._file.read_exact(0, bytes));
      require(value(decode_producer_header(bytes)) == data._header, ErrorCode::ConfigurationMismatch,
              "Producer header differs from the checked catalog context");
      data._identity = value(data._file.identity());
      data._mapping = writable ? value(data._file.map_read_write(0, data._header._fileBytes))
                               : value(data._file.map_read_only(0, data._header._fileBytes));
      data._bytes = value(data._mapping.bytes());
      data._tail.emplace(value(mapped::AtomicReadWord::bind(data._bytes, committed_tail_offset)));
      data._records.reserve(static_cast<std::size_t>(data._header._publicationCapacity));
      refresh(data);
      return data;
    }

    std::array<std::byte, producer_immutable_bytes> owner_header(const ProducerLogHeader& header,
                                                               mapped::FileIdentity dataIdentity)
    {
      std::array<std::byte, producer_immutable_bytes> bytes{};
      for (std::size_t index = 0; index != owner_magic.size(); ++index)
      {
        bytes[index] = static_cast<std::byte>(static_cast<unsigned char>(owner_magic[index]));
      }
      write_unsigned_le<std::uint16_t>(bytes, 8, log_format_major);
      write_unsigned_le<std::uint16_t>(bytes, 10, log_format_minor);
      write_unsigned_le<std::uint32_t>(bytes, 12, owner_file_bytes);
      write_unsigned_le<std::uint64_t>(bytes, 16, header._session._high);
      write_unsigned_le<std::uint64_t>(bytes, 24, header._session._low);
      write_unsigned_le<std::uint16_t>(bytes, 32, static_cast<std::uint16_t>(header._producer));
      write_unsigned_le<std::uint32_t>(bytes, 40, header._liveAbiProfile);
      write_unsigned_le<std::uint16_t>(bytes, 44, log_synchronization_version);
      write_unsigned_le<std::uint16_t>(bytes, 46, 8);
      write_unsigned_le<std::uint64_t>(bytes, 48, dataIdentity._device);
      write_unsigned_le<std::uint64_t>(bytes, 56, dataIdentity._inode);
      return bytes;
    }

    void check_paths(const ProducerLogPaths& paths)
    {
      require(!paths._log.empty() && !paths._owner.empty() && paths._log != paths._owner,
              ErrorCode::InvalidArgument, "Producer storage paths must be distinct and nonempty");
    }

    OwnedPublicationRecord copy_view(const PublicationRecordView& view, const ProducerLogHeader& header,
                                     std::uint64_t limit)
    {
      require(view._payload.size() <= limit, ErrorCode::CapacityExceeded, "Owning replay exceeds its payload budget");
      OwnedPublicationRecord result;
      result._header = view._header;
      result._reference = view._reference;
      result._recordOrdinal = view._recordOrdinal;
      result._catalogRevision = header._catalogRevision;
      result._schemaReference = view._schemaReference;
      result._topologyRevision = view._topologyRevision;
      result._destinations.reserve(view.destination_count());
      for (std::size_t index = 0; index != view.destination_count(); ++index)
      {
        result._destinations.push_back(value(view.destination_at(index)));
      }
      result._payload.assign(view._payload.begin(), view._payload.end());
      return result;
    }

    bool same_request(const PublicationRecordView& record, const ProducerAppendRequest& request)
    {
      const auto destination = value(record.destination_at(0));
      const auto& message = request._message;
      return record._header._schema == message._header._schema && record._header._format == message._header._format &&
             record._header._flags == message._header._flags && record._payload.size() == message._payload.size() &&
             record._schemaReference == request._schemaReference && record._topologyRevision == request._topologyRevision &&
             destination._queue == request._queue &&
             std::equal(record._payload.begin(), record._payload.end(), message._payload.begin());
    }
  } // namespace

  struct ProducerLog::State
  {
    // Lifetime claim is destroyed last, after both writable mappings close.
    mapped::NativeClaim _claim;
    mapped::NativeFile _ownerFile;
    mapped::MappedRegion _ownerMapping;
    LogData _data;
    std::span<std::byte> _bytes;
    std::optional<mapped::AtomicWord> _tail;
    TaskClaim _actor;
    std::thread::id _thread{std::this_thread::get_id()};
    std::uint64_t _issued{};
    ProducerLogObserver* _observer{};
    bool _inAppend{};

    void check() const
    {
      value(_claim.check_owner());
      require(_thread == std::this_thread::get_id(), ErrorCode::WrongOwner, "Producer log belongs to another thread");
      require(!_inAppend, ErrorCode::InvalidState, "Producer log operation cannot reenter append");
      require(!_data._failed, ErrorCode::IoError, "Producer log requires reopening after validation failure");
    }
  };

  struct ProducerLogReader::State
  {
    LogData _data;
    std::uint64_t _pid{static_cast<std::uint64_t>(::getpid())};
    std::thread::id _thread{std::this_thread::get_id()};

    void check() const
    {
      require(_pid == static_cast<std::uint64_t>(::getpid()) && _thread == std::this_thread::get_id(),
              ErrorCode::WrongOwner, "Producer log reader belongs to another process or thread");
      require(!_data._failed, ErrorCode::IoError, "Producer log reader requires reopening after validation failure");
    }
  };

  ProducerLog::ProducerLog() noexcept = default;
  ProducerLog::~ProducerLog() = default;
  ProducerLog::ProducerLog(ProducerLog&&) noexcept = default;
  ProducerLog& ProducerLog::operator=(ProducerLog&&) noexcept = default;
  ProducerLog::ProducerLog(std::unique_ptr<State> state) noexcept : _state(std::move(state))
  {
  }

  OperationResult ProducerLog::create(const ProducerLogPaths& paths, const ProducerLogOptions& options)
  {
    return protect<std::monostate>([&]
    {
      check_paths(paths);
      checked_queues(options);
      const auto profile = value(mapped::qualify_native_profile());
      value(live_profile_compatibility(options._header, profile._liveAbiProfile));
      auto file = value(mapped::NativeFile::create_exclusive(paths._log, options._header._fileBytes));
      value(file.reserve(options._header._fileBytes));
      auto owner = value(mapped::NativeFile::create_exclusive(paths._owner, owner_file_bytes));
      value(owner.reserve(owner_file_bytes));
      auto dataMapping = value(file.map_read_write(0, options._header._fileBytes));
      auto ownerMapping = value(owner.map_read_write(0, owner_file_bytes));
      auto dataBytes = value(dataMapping.writable_bytes());
      auto ownerBytes = value(ownerMapping.writable_bytes());
      std::fill_n(dataBytes.begin(), producer_header_bytes, std::byte{});
      std::fill(ownerBytes.begin(), ownerBytes.end(), std::byte{});
      const auto header = value(encode_producer_header(options._header));
      std::copy(header.begin(), header.end(), dataBytes.begin());
      const auto ownerHeader = owner_header(options._header, value(file.identity()));
      std::copy(ownerHeader.begin(), ownerHeader.end(), ownerBytes.begin());
      value(mapped::AtomicWord::bind(ownerBytes, owner_incarnation_offset)).publish_control(0);
      value(mapped::AtomicWord::bind(dataBytes, committed_tail_offset)).publish_control(producer_header_bytes);
      return std::monostate{};
    });
  }

  Result<ProducerLog> ProducerLog::open_writer(const ProducerLogPaths& paths, const ProducerLogOptions& options,
                                              ProducerLogObserver* observer)
  {
    return protect<ProducerLog>([&]
    {
      check_paths(paths);
      checked_queues(options);
      auto state = std::make_unique<State>();
      state->_claim = value(mapped::NativeClaim::acquire_existing(paths._log));
      state->_data = open_data(paths._log, options, true);
      state->_ownerFile = value(mapped::NativeFile::open_existing(paths._owner, mapped::FileAccess::ReadWrite, owner_file_bytes));
      require(value(state->_ownerFile.size()) == owner_file_bytes &&
                  value(state->_ownerFile.identity()) != state->_data._identity,
              ErrorCode::IoError, "Invalid producer owner file reservation or alias");
      std::array<std::byte, producer_immutable_bytes> actual{};
      value(state->_ownerFile.read_exact(0, actual));
      require(actual == owner_header(options._header, state->_data._identity),
              ErrorCode::ConfigurationMismatch, "Producer owner header or log identity mismatch");
      state->_ownerMapping = value(state->_ownerFile.map_read_write(0, owner_file_bytes));
      auto ownerBytes = value(state->_ownerMapping.writable_bytes());
      auto incarnation = value(mapped::AtomicWord::bind(ownerBytes, owner_incarnation_offset));
      const auto previous = incarnation.load_acquire();
      require(state->_data._records.empty() || state->_data._records.back()._publication._producer._incarnation <= previous,
              ErrorCode::IoError, "Committed publication claim exceeds persisted ownership");
      require(previous != std::numeric_limits<std::uint64_t>::max(), ErrorCode::CapacityExceeded, "Producer incarnation is exhausted");
      state->_bytes = value(state->_data._mapping.writable_bytes());
      state->_tail.emplace(value(mapped::AtomicWord::bind(state->_bytes, committed_tail_offset)));
      state->_actor = TaskClaim{options._header._session, options._header._producer, previous + 1};
      state->_observer = observer;
      // One authoritative word needs no multiword owner journal. Publish before
      // exposing IDs or performing any operation of this new incarnation.
      incarnation.publish_control(state->_actor._incarnation);
      return ProducerLog(std::move(state));
    });
  }

  Result<TaskClaim> ProducerLog::claim() const
  {
    return protect<TaskClaim>([&]
    {
      require(bool(_state), ErrorCode::InvalidState, "Producer log is empty or moved-from");
      _state->check();
      return _state->_actor;
    });
  }

  Result<PublicationId> ProducerLog::next_publication_id()
  {
    return protect<PublicationId>([&]
    {
      require(bool(_state), ErrorCode::InvalidState, "Producer log is empty or moved-from");
      _state->check();
      require(_state->_issued != std::numeric_limits<std::uint64_t>::max(), ErrorCode::CapacityExceeded,
              "Producer publication sequence is exhausted");
      return PublicationId{_state->_actor, ++_state->_issued};
    });
  }

  Result<ProducerAppendResult> ProducerLog::append(const ProducerAppendRequest& request)
  {
    bool committed{};
    try
    {
      require(bool(_state), ErrorCode::InvalidState, "Producer log is empty or moved-from");
      _state->check();
      auto& data = _state->_data;
      check_publication_identity(data, request._publication);
      require(request._message._header._payloadBytes == request._message._payload.size(),
              ErrorCode::InvalidArgument, "Prepared message payload size is inconsistent");
      if (const auto* existing = find_publication(data, request._publication))
      {
        require(same_request(decode_reference(data, existing->_reference), request), ErrorCode::Conflict,
                "Publication ID was accepted with different request content");
        return ProducerAppendResult{existing->_reference, existing->_ordinal, existing->_queueSequence, true};
      }
      auto& queue = queue_progress(data, request._queue, ErrorCode::InvalidArgument);
      require(request._publication._producer == _state->_actor && request._publication._sequence <= _state->_issued &&
                  (data._records.empty() || publication_key(data._records.back()._publication) < publication_key(request._publication)),
              ErrorCode::InvalidArgument, "Publication was not issued by this claim or is retired");
      require(data._records.size() < data._header._publicationCapacity, ErrorCode::LogFull, "Producer record capacity is exhausted");
      MessageHeader header;
      header._schema = request._message._header._schema;
      header._format = request._message._header._format;
      header._flags = request._message._header._flags;
      header._payloadBytes = request._message._payload.size();
      header._publication = request._publication;
      header._publishedAt = request._publishedAt;
      const std::array destinations{DestinationRecord{request._queue, DestinationStatus::Accepted,
          ErrorCode::None, queue._sequence + 1, request._publishedAt}};
      PublicationRecordInput input;
      input._header = header;
      input._recordOrdinal = data._records.size() + 1;
      input._schemaReference = request._schemaReference;
      input._topologyRevision = request._topologyRevision;
      input._destinations = destinations;
      input._payload = request._message._payload;
      struct AppendScope
      {
        explicit AppendScope(bool& busy) : _busy(busy)
        {
          _busy = true;
        }
        ~AppendScope()
        {
          _busy = false;
        }
        bool& _busy;
      } scope(_state->_inAppend);
      if (_state->_observer)
      {
        // Validate first. Only the qualification path writes this provisional
        // prefix, then pauses before the ordinary complete codec materializes it.
        value(plan_publication_record(input, data._header, data._end));
        write_unsigned_le<std::uint64_t>(_state->_bytes, static_cast<std::size_t>(data._end), 1);
        value(_state->_observer->checkpoint(ProducerLogCheckpoint::PartialMaterialization));
      }
      const auto reference = value(encode_publication_record(input, data._header, data._end,
          _state->_bytes.subspan(static_cast<std::size_t>(data._end))));
      if (_state->_observer)
      {
        value(_state->_observer->checkpoint(ProducerLogCheckpoint::BeforeCommit));
      }
      _state->_tail->store_release(reference._end);
      committed = true;
      // Reserved capacity and trivial entries make this postcommit installation
      // allocation-free/nonthrowing. Process death still rebuilds it from bytes.
      data._records.push_back(IndexEntry{request._publication, reference, input._recordOrdinal, destinations[0]._queueSequence});
      queue._sequence = destinations[0]._queueSequence;
      data._end = reference._end;
      if (_state->_observer)
      {
        value(_state->_observer->checkpoint(ProducerLogCheckpoint::AfterCommit));
      }
      return ProducerAppendResult{reference, input._recordOrdinal, queue._sequence, false};
    }
    catch (const CodecFailure& failure)
    {
      if (committed)
      {
        return Error{ErrorCode::OutcomeUnknown, "Append committed before its result was observed; resolve the ID", "producer_log"};
      }
      return failure._error;
    }
    catch (const std::exception&)
    {
      return Error{committed ? ErrorCode::OutcomeUnknown : ErrorCode::IoError,
                   "Producer append did not return normally; resolve the ID", "producer_log"};
    }
  }

  Result<std::optional<LogReference>> ProducerLog::lookup(PublicationId publication) const
  {
    return protect<std::optional<LogReference>>([&]() -> std::optional<LogReference>
    {
      require(bool(_state), ErrorCode::InvalidState, "Producer log is empty or moved-from");
      _state->check();
      check_publication_identity(_state->_data, publication);
      require(publication._producer._incarnation <= _state->_actor._incarnation &&
                  (publication._producer._incarnation != _state->_actor._incarnation || publication._sequence <= _state->_issued),
              ErrorCode::InvalidArgument, "Publication identity has not been issued by this producer");
      const auto* found = find_publication(_state->_data, publication);
      return found ? std::optional{found->_reference} : std::nullopt;
    });
  }

  Result<PublicationRecordView> ProducerLog::view(const LogReference& reference) const
  {
    return protect<PublicationRecordView>([&]
    {
      require(bool(_state), ErrorCode::InvalidState, "Producer log is empty or moved-from");
      _state->check();
      return indexed_view(_state->_data, reference);
    });
  }

  Result<std::uint64_t> ProducerLog::committed_end() const
  {
    return protect<std::uint64_t>([&]
    {
      require(bool(_state), ErrorCode::InvalidState, "Producer log is empty or moved-from");
      _state->check();
      const auto end = acquired_end(_state->_data);
      require(end == _state->_data._end, ErrorCode::IoError, "Producer tail changed outside its owner");
      return end;
    });
  }

  Result<std::size_t> ProducerLog::record_count() const
  {
    return protect<std::size_t>([&]
    {
      require(bool(_state), ErrorCode::InvalidState, "Producer log is empty or moved-from");
      _state->check();
      return _state->_data._records.size();
    });
  }

  ProducerLogReader::ProducerLogReader() noexcept = default;
  ProducerLogReader::~ProducerLogReader() = default;
  ProducerLogReader::ProducerLogReader(ProducerLogReader&&) noexcept = default;
  ProducerLogReader& ProducerLogReader::operator=(ProducerLogReader&&) noexcept = default;
  ProducerLogReader::ProducerLogReader(std::unique_ptr<State> state) noexcept : _state(std::move(state))
  {
  }

  Result<ProducerLogReader> ProducerLogReader::open(const std::filesystem::path& logPath, const ProducerLogOptions& options)
  {
    return protect<ProducerLogReader>([&]
    {
      auto state = std::make_unique<State>();
      state->_data = open_data(logPath, options, false);
      return ProducerLogReader(std::move(state));
    });
  }

  Result<ProducerReplayCursor> ProducerLogReader::capture()
  {
    return protect<ProducerReplayCursor>([&]
    {
      require(bool(_state), ErrorCode::InvalidState, "Producer log reader is empty or moved-from");
      _state->check();
      refresh(_state->_data);
      ProducerReplayCursor cursor;
      cursor._session = _state->_data._header._session;
      cursor._producer = _state->_data._header._producer;
      cursor._device = _state->_data._identity._device;
      cursor._inode = _state->_data._identity._inode;
      cursor._end = _state->_data._end;
      cursor._limit = _state->_data._records.size();
      return cursor;
    });
  }

  Result<std::optional<PublicationRecordView>> ProducerLogReader::next(ProducerReplayCursor& cursor) const
  {
    return protect<std::optional<PublicationRecordView>>([&]() -> std::optional<PublicationRecordView>
    {
      require(bool(_state), ErrorCode::InvalidState, "Producer log reader is empty or moved-from");
      _state->check();
      const auto& data = _state->_data;
      require(cursor._session == data._header._session && cursor._producer == data._header._producer &&
                  cursor._device == data._identity._device && cursor._inode == data._identity._inode &&
                  cursor._index <= cursor._limit && cursor._limit <= data._records.size() &&
                  cursor._end == (cursor._limit == 0 ? producer_header_bytes : data._records[cursor._limit - 1]._reference._end),
              ErrorCode::InvalidArgument, "Replay cursor does not describe this validated producer-log cut");
      if (cursor._index == cursor._limit)
      {
        return std::nullopt;
      }
      auto record = decode_reference(data, data._records[cursor._index]._reference);
      ++cursor._index;
      return record;
    });
  }

  Result<PublicationRecordView> ProducerLogReader::view(const LogReference& reference) const
  {
    return protect<PublicationRecordView>([&]
    {
      require(bool(_state), ErrorCode::InvalidState, "Producer log reader is empty or moved-from");
      _state->check();
      return indexed_view(_state->_data, reference);
    });
  }

  Result<OwnedPublicationRecord> ProducerLogReader::copy_record(const LogReference& reference, std::uint64_t maxPayloadBytes) const
  {
    return protect<OwnedPublicationRecord>([&]
    {
      require(bool(_state), ErrorCode::InvalidState, "Producer log reader is empty or moved-from");
      _state->check();
      return copy_view(indexed_view(_state->_data, reference), _state->_data._header, maxPayloadBytes);
    });
  }

  Result<std::uint64_t> ProducerLogReader::committed_end() const
  {
    return protect<std::uint64_t>([&]
    {
      require(bool(_state), ErrorCode::InvalidState, "Producer log reader is empty or moved-from");
      _state->check();
      return acquired_end(_state->_data);
    });
  }

  std::uint64_t ProducerReplayCursor::committed_end() const noexcept
  {
    return _end;
  }

  std::size_t ProducerReplayCursor::record_count() const noexcept
  {
    return _limit;
  }
} // namespace angaria::detail::recorded
