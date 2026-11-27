#include "recorded_spsc.h"

#include "ipc_comparison_support.h"
#include "journal_format.h"
#include "message_retention.h"
#include "native_atomic.h"
#include "native_file.h"
#include "payload_runtime.h"
#include "publication_metadata.h"
#include "registry_access.h"
#include "topology_state.h"
#include "transport_support.h"

#include "angaria/detail/byte_operations.h"
#include "angaria/detail/checked_arithmetic.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include <sys/file.h>
#include <sys/mman.h>
#include <unistd.h>

namespace angaria::benchmarks::spsc
{
  namespace
  {
    namespace mapped = detail::mapped;
    using detail::read_unsigned_le;
    using detail::write_unsigned_le;
    constexpr std::uint64_t connection_magic = 0x31535053474e4141ULL;
    constexpr std::uint64_t connection_version = 1;
    constexpr std::size_t head_offset = 192;
    constexpr std::size_t tail_offset = 256;
    constexpr std::size_t slots_offset = 320;
    constexpr std::size_t slot_bytes = 64;
    constexpr std::size_t immutable_header_bytes = 192;

    [[noreturn]] void fail(ErrorCode code, std::string message)
    {
      throw BenchmarkFailure{Error{code, std::move(message), "benchmark.recorded_spsc"}};
    }

    void check(bool condition, std::string_view message)
    {
      if (!condition)
      {
        fail(ErrorCode::IoError, std::string{message});
      }
    }

    std::shared_ptr<detail::SessionState> session_state(const Options& options)
    {
      auto state = std::make_shared<detail::SessionState>();
      state->_directory = options._directory;
      state->_controller = value(mapped::SessionController::open_existing(options._directory));
      const auto& configuration = state->_controller->configuration();
      check(configuration._session == options._session && configuration._maxLogBytes == options._maxLogBytes &&
                configuration._storage == StorageMapping::TwoPlane,
            "Prototype connection requires the exact two-plane Session and log bound");
      check(options._capacity != 0 && options._capacity <= 65536 && options._payloadBytes != 0 &&
                options._payloadBytes <= options._maxLogBytes && options._queue != QueueId::Invalid &&
                options._producer != TaskId::Invalid,
            "Invalid bounded prototype geometry or identity");
      return state;
    }

    std::uint64_t connection_bytes(const Options& options)
    {
      return slots_offset + options._capacity * slot_bytes;
    }

    struct CheckedSetup
    {
      TaskId _consumer{};
      std::uint64_t _schemaReference{};
      std::uint64_t _queueRevision{};
      std::uint64_t _firstSequence{};
      BinaryPayloadCodec _codec;
    };

    CheckedSetup check_setup(detail::RuntimeAccess& access, const Options& options)
    {
      const auto topology = detail::runtime_topology(access);
      // A second appender would invalidate the captured-tail strategy. The
      // fixed study permits only this Queue and freezes Registry revisions.
      check(topology._queues.size() == 1 && topology._publishing.empty(),
            "The current-log prototype requires exactly one Queue and no Publishing sets");
      const auto& queue = detail::require_registered_entity(topology._queues, options._queue, "spsc.queue");
      const auto& source = detail::require_registered_entity(topology._sources,
          queue._definition._descriptor._sourceSet, "spsc.source");
      check(queue._definition._descriptor._producer == options._producer &&
                queue._definition._state == EntityState::Active && source._definition._state == EntityState::Active,
            "Prototype Queue/Source ownership or activation differs from setup");
      const auto words = access.read_words(queue._stateOffset, 1);
      check(words.front() != std::numeric_limits<std::uint64_t>::max(), "Prototype Queue sequence is exhausted");
      const auto schema = detail::runtime_schema_context(access, options._schema);
      auto codec = detail::runtime_payload_codec(schema._schema, schema._fields, options._maxLogBytes);
      check(codec.payload_size() == options._payloadBytes, "Prototype schema has a different exact payload size");
      return {source._definition._descriptor._owner, schema._schema._reference,
          queue._definition._registration._revision, words.front() + 1, std::move(codec)};
    }

    std::filesystem::path connection_path(const Options& options)
    {
      return options._directory / "recorded-spsc.connection";
    }

    std::filesystem::path lease_path(const Options& options, Role role)
    {
      return options._directory / (role == Role::Producer ? "recorded-spsc.producer" : "recorded-spsc.consumer");
    }

    // The ordinary NativeFile mapper protects current file-size ranges. This
    // experiment instead reserves a read-only virtual range through the fixed
    // log ceiling, without growing or touching future file pages. A released
    // committed tail/reference is the only permission to inspect a subrange.
    // Existing append grows the file before committing; it never truncates a
    // committed record. This qualification is POSIX/Linux/macOS specific.
    class FutureLogMapping
    {
    public:
      FutureLogMapping(mapped::NativeFile& file, std::uint64_t bytes) : _bytes(bytes)
      {
        check(detail::fits_host_size(bytes), "Prototype mapping exceeds host size");
        const auto descriptor = value(file.native_handle());
        _address = ::mmap(nullptr, static_cast<std::size_t>(bytes), PROT_READ, MAP_SHARED, descriptor, 0);
        if (_address == MAP_FAILED || _address == nullptr)
        {
          if (_address == nullptr)
          {
            ::munmap(_address, static_cast<std::size_t>(bytes));
          }
          fail(ErrorCode::IoError, "Cannot reserve the bounded prototype log mapping");
        }
      }

      ~FutureLogMapping()
      {
        ::munmap(_address, static_cast<std::size_t>(_bytes));
      }

      FutureLogMapping(const FutureLogMapping&) = delete;
      FutureLogMapping& operator=(const FutureLogMapping&) = delete;

      std::span<const std::byte> bytes() const
      {
        return {static_cast<const std::byte*>(_address), static_cast<std::size_t>(_bytes)};
      }

    private:
      void* _address{};
      std::uint64_t _bytes{};
    };

    std::uint64_t load_readiness(std::span<const std::byte> bytes, std::uint64_t reference)
    {
      // AtomicWord's binding surface also serves stores. This read-only mapped
      // binding is used exclusively with load_acquire, never a store operation.
      const auto word = bytes.subspan(static_cast<std::size_t>(reference + 48), 8);
      return value(mapped::AtomicWord::bind({const_cast<std::byte*>(word.data()), word.size()}, 0)).load_acquire();
    }

    mapped::RecordEnvelope envelope(std::span<const std::byte> bytes, std::uint64_t reference,
                                       std::uint64_t tail)
    {
      check(reference >= mapped::superblock_bytes && reference % 64 == 0 &&
                mapped::contains_range(tail, reference, mapped::record_envelope_bytes),
            "Prototype reference is outside the committed log");
      mapped::RecordEnvelopeBytes copy{};
      std::copy_n(bytes.begin() + static_cast<std::ptrdiff_t>(reference), 48, copy.begin());
      write_unsigned_le(copy, 48, load_readiness(bytes, reference));
      std::copy_n(bytes.begin() + static_cast<std::ptrdiff_t>(reference + 56), 8, copy.begin() + 56);
      const auto result = value(mapped::decode_record_envelope(copy));
      check(result._readiness == 2 && mapped::contains_range(tail, reference, result._totalBytes),
            "Prototype reference is not a complete permanently committed record");
      return result;
    }
  } // namespace

  struct RecordedSpsc::State
  {
    State(const Options& options, std::optional<Role> role, TaskClaim actor)
        : _options(options), _role(role), _actor(actor), _ownerPid(static_cast<std::uint64_t>(::getpid())),
          _session(session_state(options)),
          _file(value(mapped::NativeFile::open_existing(options._directory / "session.ang", mapped::FileAccess::ReadWrite,
                                                       options._maxLogBytes)))
    {
      detail::RuntimeAccess access(_session);
      auto checked = check_setup(access, options);
      _controlFile = value(mapped::NativeFile::open_existing(connection_path(options), mapped::FileAccess::ReadWrite,
                                                             connection_bytes(options)));
      check(value(_controlFile.size()) == connection_bytes(options), "Prototype connection has an invalid length");
      _controlMapping = value(_controlFile.map_read_write(0, connection_bytes(options)));
      _control = value(_controlMapping.writable_bytes());
      auto read = [&](std::size_t offset)
      {
        return read_unsigned_le<std::uint64_t>(_control, offset);
      };
      check(read(0) == connection_magic && read(8) == connection_version &&
                read(16) == options._session._high && read(24) == options._session._low &&
                read(32) == static_cast<std::uint64_t>(options._queue) &&
                read(40) == static_cast<std::uint64_t>(options._producer) &&
                read(48) == static_cast<std::uint64_t>(checked._consumer) &&
                read(56) == static_cast<std::uint64_t>(options._schema._type) && read(64) == options._schema._version &&
                read(72) == options._payloadBytes && read(80) == options._capacity && read(88) == options._maxLogBytes &&
                read(120) == _session->_controller->configuration()._liveAbiProfile &&
                read(128) == 0 &&
                read(136) == checked._schemaReference && read(144) == access._snapshot._revision &&
                read(152) == checked._queueRevision && detail::all_zero(std::span<const std::byte>{_control}.subspan(160, 32)),
            "Prototype connection is stale or bound to different immutable setup");
      _logBegin = read(96);
      _nextSequence = read(104);
      _generation = read(112);
      check(_generation != 0 && _logBegin >= mapped::superblock_bytes && _logBegin % 64 == 0 &&
                _logBegin <= access._snapshot._tail && _nextSequence != 0,
            "Prototype connection start or generation is invalid");
      _scanCursor = _logBegin;
      _schemaReference = checked._schemaReference;
      _queueRevision = checked._queueRevision;
      _registryRevision = access._snapshot._revision;
      if (role)
      {
        check(*role == Role::Producer || *role == Role::Consumer, "Prototype endpoint role is invalid");
        check(actor._session == options._session && actor._incarnation != 0 &&
                  actor._task == (*role == Role::Producer ? options._producer : checked._consumer),
              "Prototype endpoint actor has a different role or Session");
        const auto status = detail::runtime_value(detail::runtime_task_status(access, actor._task));
        check(status._attached && status._incarnation == actor._incarnation && status._processId == _ownerPid,
              "Prototype endpoint actor is not the current live native claim");
        _lease = value(mapped::NativeFile::open_existing(lease_path(options, *role), mapped::FileAccess::ReadWrite, 1));
        const auto fd = value(_lease.native_handle());
        if (::flock(fd, LOCK_EX | LOCK_NB) == -1)
        {
          fail(ErrorCode::OwnershipBusy, "Prototype endpoint role already has an owner");
        }
      }
      _head.emplace(value(mapped::AtomicWord::bind(_control, head_offset)));
      _tail.emplace(value(mapped::AtomicWord::bind(_control, tail_offset)));
      _prefixMapping = value(_file.map_read_write(0, mapped::superblock_bytes));
      const auto prefix = value(_prefixMapping.writable_bytes());
      _logTail.emplace(value(mapped::AtomicWord::bind(prefix, 88)));
      _revision.emplace(value(mapped::AtomicWord::bind(prefix, 80)));
      _log = std::make_unique<FutureLogMapping>(_file, options._maxLogBytes);
      MessageHeader header;
      header._schema = options._schema;
      _template = value(detail::MessageAccess::bind(header, {}, options._session, std::move(checked._codec)));
      _snapshotTail = access._snapshot._tail;
    }

    void check_owner(std::optional<Role> requested) const
    {
      check(_ownerPid == static_cast<std::uint64_t>(::getpid()), "Prototype endpoint requires exec after fork");
      check(!requested || _role == requested, "Prototype endpoint operation has the wrong role");
      check(_revision->load_acquire() == _registryRevision, "Prototype topology changed after attachment");
    }

    std::uint64_t checked_tail() const
    {
      const auto tail = _logTail->load_acquire();
      check(tail >= _logBegin && tail <= _options._maxLogBytes && tail % 64 == 0,
            "Prototype log tail is invalid");
      return tail;
    }

    MessageView message(std::uint64_t reference, std::uint64_t tail,
                          std::optional<PublicationId> expected = {}) const
    {
      const auto bytes = _log->bytes();
      const auto record = envelope(bytes, reference, tail);
      check(record._kind == 9 && record._encoding == 1, "Prototype descriptor is not a Publication");
      check(record._immutableBytes >= mapped::publication_fixed_bytes,
            "Prototype Publication has a truncated fixed header");
      const auto fixed = bytes.subspan(static_cast<std::size_t>(reference + mapped::record_envelope_bytes),
                                        mapped::publication_fixed_bytes);
      const auto prefix = value(mapped::decode_publication_prefix(fixed, record._immutableBytes, reference,
                                                                  _options._maxLogBytes));
      check(prefix._destinationCount == 1 && prefix._layout._totalBytes == record._totalBytes &&
                prefix._header._producer == _options._producer && prefix._header._schema == _options._schema &&
                prefix._header._schemaReference == _schemaReference && prefix._payloadBytes == _options._payloadBytes &&
                prefix._header._format == PayloadFormat::Binary && prefix._header._publishingSet == PublishingQueueSetId::Invalid &&
                prefix._header._acceptance == AcceptancePolicy::AllDestinations &&
                prefix._header._topologyRevision == _queueRevision,
            "Prototype Publication has different identity, schema or destination shape");
      const auto destination = value(mapped::decode_destination_row(bytes.subspan(
          static_cast<std::size_t>(prefix._layout._destinationsOffset), mapped::destination_row_bytes),
          reference, prefix._layout._receiptsOffset));
      check(destination._queue == _options._queue && destination._status == DestinationStatus::Accepted &&
                destination._reason == ErrorCode::None,
            "Prototype Publication did not accept its configured Queue");
      const auto payloadEnd = prefix._layout._payloadOffset + prefix._payloadBytes;
      check(detail::all_zero(bytes.subspan(static_cast<std::size_t>(payloadEnd),
                static_cast<std::size_t>(prefix._layout._receiptsOffset - payloadEnd))),
            "Prototype Publication padding is not canonical");
      auto view = _template;
      auto& header = view._header;
      header._schema = prefix._header._schema;
      header._format = prefix._header._format;
      header._publication = {{_options._session, prefix._header._producer, prefix._header._producerIncarnation},
                                prefix._header._publicationSequence};
      header._publishedAt = prefix._header._publishedAt;
      header._payloadBytes = prefix._payloadBytes;
      header._flags = prefix._header._flags;
      header._queue = destination._queue;
      header._queueSequence = destination._queueSequence;
      header._enqueuedAt = destination._enqueuedAt;
      view._payload = bytes.subspan(static_cast<std::size_t>(prefix._layout._payloadOffset),
                                     static_cast<std::size_t>(prefix._payloadBytes));
      if (expected)
      {
        check(header._publication == *expected, "Prototype captured tail does not identify the accepted Publication");
      }
      return view;
    }

    std::optional<MessageView> catch_up(std::uint64_t limit)
    {
      const auto bytes = _log->bytes();
      while (_scanCursor < limit)
      {
        check(mapped::contains_range(limit, _scanCursor, mapped::record_envelope_bytes),
              "Prototype catch-up has an incomplete record envelope");
        // Conservative even during interrupted redo: never advance beyond a
        // provisional record. Normal tail publication follows permanent marks.
        if (load_readiness(bytes, _scanCursor) != 2)
        {
          return std::nullopt;
        }
        const auto reference = _scanCursor;
        const auto record = envelope(bytes, reference, limit);
        _scanCursor += record._totalBytes;
        if (record._kind == 9)
        {
          auto view = message(reference, limit);
          const auto sequence = *view._header._queueSequence;
          if (_sequenceExhausted || sequence < _nextSequence)
          {
            continue;
          }
          check(sequence == _nextSequence, "Prototype committed Queue order has a gap");
          if (_nextSequence == std::numeric_limits<std::uint64_t>::max())
          {
            _sequenceExhausted = true;
          }
          else
          {
            ++_nextSequence;
          }
          return view;
        }
        check(record._kind == 11, "Prototype encountered an unexpected append after frozen topology");
      }
      return std::nullopt;
    }

    Options _options;
    std::optional<Role> _role;
    TaskClaim _actor;
    std::uint64_t _ownerPid{};
    std::shared_ptr<detail::SessionState> _session;
    mapped::NativeFile _file;
    mapped::NativeFile _controlFile;
    mapped::MappedRegion _controlMapping;
    std::span<std::byte> _control;
    mapped::NativeFile _lease;
    mapped::MappedRegion _prefixMapping;
    std::optional<mapped::AtomicWord> _head;
    std::optional<mapped::AtomicWord> _tail;
    std::optional<mapped::AtomicWord> _logTail;
    std::optional<mapped::AtomicWord> _revision;
    std::unique_ptr<FutureLogMapping> _log;
    MessageView _template;
    std::uint64_t _logBegin{};
    std::uint64_t _scanCursor{};
    std::uint64_t _nextSequence{};
    std::uint64_t _generation{};
    std::uint64_t _schemaReference{};
    std::uint64_t _queueRevision{};
    std::uint64_t _registryRevision{};
    std::uint64_t _snapshotTail{};
    bool _sequenceExhausted{};
  };

  void RecordedSpsc::create(const Options& options)
  {
    try
    {
      const auto state = session_state(options);
      detail::RuntimeAccess access(state);
      auto checked = check_setup(access, options);
      auto file = value(mapped::NativeFile::create_exclusive(connection_path(options), connection_bytes(options)));
      value(file.resize(connection_bytes(options)));
      std::array<std::byte, immutable_header_bytes> header{};
      auto write = [&](std::size_t offset, std::uint64_t data)
      {
        write_unsigned_le(header, offset, data);
      };
      write(0, connection_magic);
      write(8, connection_version);
      write(16, options._session._high);
      write(24, options._session._low);
      write(32, static_cast<std::uint64_t>(options._queue));
      write(40, static_cast<std::uint64_t>(options._producer));
      write(48, static_cast<std::uint64_t>(checked._consumer));
      write(56, static_cast<std::uint64_t>(options._schema._type));
      write(64, options._schema._version);
      write(72, options._payloadBytes);
      write(80, options._capacity);
      write(88, options._maxLogBytes);
      write(96, access._snapshot._tail);
      write(104, checked._firstSequence);
      write(112, 1);
      write(120, state->_controller->configuration()._liveAbiProfile);
      write(136, checked._schemaReference);
      write(144, access._snapshot._revision);
      write(152, checked._queueRevision);
      value(file.write_exact(0, header));
      for (const auto role : {Role::Producer, Role::Consumer})
      {
        (void)value(mapped::NativeFile::create_exclusive(lease_path(options, role), 1));
      }
    }
    catch (const detail::RuntimeFailure& failure)
    {
      throw BenchmarkFailure{failure._error};
    }
  }

  RecordedSpsc::RecordedSpsc(const Options& options, Role role, TaskClaim actor)
  try : _state(std::make_unique<State>(options, role, actor))
  {
  }
  catch (const detail::RuntimeFailure& failure)
  {
    throw BenchmarkFailure{failure._error};
  }

  RecordedSpsc::~RecordedSpsc() = default;
  RecordedSpsc::RecordedSpsc(RecordedSpsc&&) noexcept = default;
  RecordedSpsc& RecordedSpsc::operator=(RecordedSpsc&&) noexcept = default;

  bool RecordedSpsc::has_credit() const
  {
    check(_state != nullptr, "Prototype endpoint is moved from");
    _state->check_owner(Role::Producer);
    const auto head = _state->_head->load_acquire();
    const auto tail = _state->_tail->load_acquire();
    check(head >= tail && head - tail <= _state->_options._capacity && head != std::numeric_limits<std::uint64_t>::max(),
          "Prototype ring cursor or generation is invalid");
    return head - tail < _state->_options._capacity;
  }

  std::uint64_t RecordedSpsc::next_record_reference() const
  {
    check(_state != nullptr, "Prototype endpoint is moved from");
    _state->check_owner(Role::Producer);
    return _state->checked_tail();
  }

  void RecordedSpsc::publish_committed(std::uint64_t reference, PublicationId publication)
  {
    check(_state != nullptr, "Prototype endpoint is moved from");
    _state->check_owner(Role::Producer);
    check(publication._producer == _state->_actor, "Prototype publication belongs to a different producer claim");
    if (!has_credit())
    {
      fail(ErrorCode::OutcomeUnknown,
          "Publication was already accepted but descriptor credit is unavailable; recover from the log");
    }
    const auto message = _state->message(reference, _state->checked_tail(), publication);
    const auto head = _state->_head->load_acquire();
    const auto offset = slots_offset + static_cast<std::size_t>(head % _state->_options._capacity) * slot_bytes;
    auto slot = _state->_control.subspan(offset, slot_bytes);
    write_unsigned_le(slot, 0, reference);
    write_unsigned_le(slot, 8, publication._sequence);
    write_unsigned_le(slot, 16, publication._producer._incarnation);
    write_unsigned_le(slot, 24, *message._header._queueSequence);
    write_unsigned_le(slot, 32, head + 1);
    write_unsigned_le(slot, 40, _state->_generation);
    write_unsigned_le(slot, 48, std::uint64_t{});
    write_unsigned_le(slot, 56, std::uint64_t{});
    _state->_head->store_release(head + 1);
  }

  std::optional<MessageView> RecordedSpsc::try_receive()
  {
    check(_state != nullptr, "Prototype endpoint is moved from");
    _state->check_owner(Role::Consumer);
    for (;;)
    {
      const auto tail = _state->_tail->load_acquire();
      const auto head = _state->_head->load_acquire();
      check(head >= tail && head - tail <= _state->_options._capacity, "Prototype ring cursors are invalid");
      if (head == tail)
      {
        auto result = _state->catch_up(_state->checked_tail());
        if (result)
        {
          result->_header._consumer = _state->_actor;
          result->_header._dequeuedAt = UtcTimestamp{std::chrono::nanoseconds{ipc::realtime_ns()}};
        }
        return result;
      }
      const auto offset = slots_offset + static_cast<std::size_t>(tail % _state->_options._capacity) * slot_bytes;
      const auto slot = std::span<const std::byte>{_state->_control}.subspan(offset, slot_bytes);
      const auto reference = read_unsigned_le<std::uint64_t>(slot, 0);
      const PublicationId publication{{_state->_options._session, _state->_options._producer,
          read_unsigned_le<std::uint64_t>(slot, 16)}, read_unsigned_le<std::uint64_t>(slot, 8)};
      const auto sequence = read_unsigned_le<std::uint64_t>(slot, 24);
      check(read_unsigned_le<std::uint64_t>(slot, 32) == tail + 1 &&
                read_unsigned_le<std::uint64_t>(slot, 40) == _state->_generation &&
                detail::all_zero(slot.subspan(48)), "Prototype ring slot has a stale generation");
      // Even a descriptor already delivered through catch-up must still name
      // the same accepted record. Cursor deduplication does not bless bad refs.
      auto result = _state->message(reference, _state->checked_tail(), publication);
      check(result._header._queueSequence == sequence && reference >= _state->_logBegin,
            "Prototype descriptor differs from committed Queue order");
      if (_state->_sequenceExhausted || sequence < _state->_nextSequence)
      {
        // Catch-up may observe a committed record before the producer exposes
        // its descriptor. Returning this duplicate descriptor's credit is not
        // a second application delivery or an acknowledgment.
        _state->_tail->store_release(tail + 1);
        continue;
      }
      if (sequence > _state->_nextSequence)
      {
        auto result = _state->catch_up(_state->checked_tail());
        check(result.has_value(), "Prototype descriptor skipped a committed predecessor");
        result->_header._consumer = _state->_actor;
        result->_header._dequeuedAt = UtcTimestamp{std::chrono::nanoseconds{ipc::realtime_ns()}};
        return result;
      }
      const auto record = envelope(_state->_log->bytes(), reference, _state->checked_tail());
      _state->_scanCursor = std::max(_state->_scanCursor, reference + record._totalBytes);
      if (_state->_nextSequence == std::numeric_limits<std::uint64_t>::max())
      {
        _state->_sequenceExhausted = true;
      }
      else
      {
        ++_state->_nextSequence;
      }
      result._header._consumer = _state->_actor;
      result._header._dequeuedAt = UtcTimestamp{std::chrono::nanoseconds{ipc::realtime_ns()}};
      _state->_tail->store_release(tail + 1);
      return result;
    }
  }

  std::vector<Message> RecordedSpsc::replay(const Options& options)
  {
    try
    {
      State state(options, std::nullopt, {});
      std::vector<Message> result;
      while (const auto view = state.catch_up(state._snapshotTail))
      {
        result.push_back(value(view->copy_message(options._maxLogBytes)));
      }
      return result;
    }
    catch (const detail::RuntimeFailure& failure)
    {
      throw BenchmarkFailure{failure._error};
    }
  }
} // namespace angaria::benchmarks::spsc
