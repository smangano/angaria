#include "runtime_state.h"

#include "journal_validation.h"
#include "native_atomic.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <exception>
#include <limits>
#include <map>
#include <set>
#include <stdexcept>
#include <system_error>

#include <pthread.h>
#include <time.h>
#if defined(__linux__)
#include <sys/syscall.h>
#include <unistd.h>
#endif

namespace angaria::detail
{
  [[noreturn]] void runtime_fail(ErrorCode code, std::string path, std::string message)
  {
    throw RuntimeFailure{Error{code, std::move(message), std::move(path)}};
  }

  Error runtime_caught_error()
  {
    try
    {
      throw;
    }
    catch (const RuntimeFailure& failure)
    {
      return failure._error;
    }
    catch (const std::bad_alloc&)
    {
      return Error{ErrorCode::CapacityExceeded, "Insufficient process memory", "runtime"};
    }
    catch (const std::exception& error)
    {
      return Error{ErrorCode::IoError, error.what(), "runtime"};
    }
    catch (...)
    {
      return Error{ErrorCode::IoError, "Unexpected native runtime failure", "runtime"};
    }
  }

  std::int64_t runtime_now_ns()
  {
    timespec time{};
    if (::clock_gettime(CLOCK_REALTIME, &time) != 0)
    {
      runtime_fail(ErrorCode::IoError, "clock", "Cannot read UTC clock");
    }
    const auto nanoseconds = static_cast<__int128>(time.tv_sec) * 1000000000 + time.tv_nsec;
    if (nanoseconds < std::numeric_limits<std::int64_t>::min() ||
        nanoseconds > std::numeric_limits<std::int64_t>::max())
    {
      runtime_fail(ErrorCode::CapacityExceeded, "clock", "UTC time exceeds the mapped timestamp range");
    }
    return static_cast<std::int64_t>(nanoseconds);
  }

  std::uint64_t runtime_thread_id()
  {
#if defined(__APPLE__)
    std::uint64_t value{};
    if (::pthread_threadid_np(nullptr, &value) != 0 || value == 0)
    {
      runtime_fail(ErrorCode::IoError, "task.thread", "Cannot obtain native thread identity");
    }
    return value;
#elif defined(__linux__)
    const auto value = ::syscall(SYS_gettid);
    if (value <= 0)
    {
      runtime_fail(ErrorCode::IoError, "task.thread", "Cannot obtain native thread identity");
    }
    return static_cast<std::uint64_t>(value);
#else
    runtime_fail(ErrorCode::UnsupportedPolicy, "task.thread", "Native thread identity is unsupported");
#endif
  }

  mapped::RedoCommand runtime_write_words(std::uint64_t offset, std::span<const std::uint64_t> values)
  {
    mapped::RedoCommand command;
    command._target = offset;
    command._payload.resize(values.size() * 8);
    for (std::size_t index = 0; index < values.size(); ++index)
    {
      for (unsigned byte = 0; byte < 8; ++byte)
      {
        command._payload[index * 8 + byte] = static_cast<std::byte>(values[index] >> (byte * 8));
      }
    }
    return command;
  }

  namespace
  {
    std::shared_ptr<SessionState> checked_session(std::shared_ptr<SessionState> session)
    {
      if (!session || !session->_controller)
      {
        runtime_fail(ErrorCode::InvalidState, "session", "Session has no live state");
      }
      runtime_value(session->_controller->check_owner());
      return session;
    }
  }

  RuntimeAccess::RuntimeAccess(std::shared_ptr<SessionState> session, bool requireReady)
      : _session(checked_session(std::move(session))),
        _guard(runtime_value(_session->_controller->acquire_mutation())),
        _journal(runtime_value(mapped::MutationJournal::open(_session->_controller, _guard))),
        _snapshot(runtime_value(_journal.snapshot(_guard))),
        _file(runtime_value(_session->_controller->file(_guard)))
  {
    const auto flags = read_words(112, 1)[0];
    if ((flags & ~session_ready_flag) != 0 || (requireReady && flags != session_ready_flag))
    {
      runtime_fail(ErrorCode::IoError, "session.flags", "Session is incomplete or has unknown flags");
    }
    index_records();
  }

  std::vector<std::uint64_t> RuntimeAccess::read_words(std::uint64_t offset, std::size_t count) const
  {
    if (offset % 8 != 0 || count > std::numeric_limits<std::size_t>::max() / 8 ||
        !mapped::contains_range(_snapshot._fileBytes, offset, std::uint64_t{count} * 8))
    {
      runtime_fail(ErrorCode::IoError, "runtime.words", "Atomic range exceeds the recovered file");
    }
    std::vector<std::uint64_t> result(count);
    if (count == 0)
    {
      return result;
    }
    auto region = runtime_value(_file->map_read_write(offset, count * 8));
    auto bytes = runtime_value(region.writable_bytes());
    for (std::size_t index = 0; index < count; ++index)
    {
      result[index] = runtime_value(mapped::AtomicWord::bind(bytes, index * 8)).load_acquire();
    }
    return result;
  }

  std::vector<std::byte> RuntimeAccess::read_bytes(std::uint64_t offset, std::uint64_t length) const
  {
    if (length > std::numeric_limits<std::size_t>::max() ||
        !mapped::contains_range(_snapshot._tail, offset, length))
    {
      runtime_fail(ErrorCode::IoError, "runtime.bytes", "Immutable range exceeds committed history");
    }
    std::vector<std::byte> result(static_cast<std::size_t>(length));
    runtime_value(_file->read_exact(offset, result));
    return result;
  }

  void RuntimeAccess::index_records()
  {
    const auto layout = mapped::make_fixed_layout(_session->_controller->configuration()._limits);
    if (!layout || _snapshot._tail < layout->_arenaBegin ||
        _snapshot._tail % mapped::section_alignment != 0)
    {
      runtime_fail(ErrorCode::IoError, "record", "Invalid committed record arena");
    }
    // The journal has recovered and validated this prefix. Capture its exact
    // boundaries once while holding the gate; definition lookups then reuse
    // the same owning envelope copies instead of rescanning preceding records.
    for (auto offset = layout->_arenaBegin; offset < _snapshot._tail;)
    {
      if (!mapped::contains_range(_snapshot._tail, offset, mapped::record_envelope_bytes))
      {
        runtime_fail(ErrorCode::IoError, "record", "Committed history ends inside a record envelope");
      }
      mapped::RecordEnvelopeBytes bytes{};
      runtime_value(_file->read_exact(offset, std::span{bytes}.first(48)));
      const auto readiness = read_words(offset + 48, 1)[0];
      for (unsigned byte = 0; byte < 8; ++byte)
      {
        bytes[48 + byte] = static_cast<std::byte>(readiness >> (byte * 8));
      }
      runtime_value(_file->read_exact(offset + 56, std::span{bytes}.subspan(56)));
      const auto envelope = runtime_value(mapped::decode_record_envelope(bytes));
      if (envelope._readiness != 2 || envelope._generation > _snapshot._generation ||
          envelope._totalBytes == 0 ||
          !mapped::contains_range(_snapshot._tail, offset, envelope._totalBytes))
      {
        runtime_fail(ErrorCode::IoError, "record", "Reference is outside committed record history");
      }
      const auto body = read_bytes(offset + mapped::record_envelope_bytes,
                                  std::min<std::uint64_t>(128, envelope._immutableBytes));
      runtime_value(mapped::validate_journal_record(offset, envelope, body, layout->_arenaBegin));
      _recordBoundaries.push_back(RecordBoundary{offset, envelope});
      offset += envelope._totalBytes;
    }
  }

  const mapped::RecordEnvelope& RuntimeAccess::record_envelope(std::uint64_t reference, std::uint16_t kind) const
  {
    if (reference % mapped::section_alignment != 0)
    {
      runtime_fail(ErrorCode::IoError, "record.reference", "Invalid immutable record reference");
    }
    const auto found = std::lower_bound(_recordBoundaries.begin(), _recordBoundaries.end(), reference,
                                       [](const auto& candidate, std::uint64_t value)
                                       {
                                         return candidate._offset < value;
                                       });
    if (found == _recordBoundaries.end() || found->_offset != reference)
    {
      runtime_fail(ErrorCode::IoError, "record.reference", "Reference does not name a committed record boundary");
    }
    if (found->_envelope._kind != kind)
    {
      runtime_fail(ErrorCode::IoError, "record.kind", "Reference has the wrong record kind");
    }
    return found->_envelope;
  }

  std::vector<std::byte> RuntimeAccess::immutable_record(std::uint64_t reference, std::uint16_t kind) const
  {
    const auto& envelope = record_envelope(reference, kind);
    return read_bytes(reference + mapped::record_envelope_bytes, envelope._immutableBytes);
  }

  std::uint64_t RuntimeAccess::section_offset(mapped::SectionKind kind) const
  {
    const auto layout = mapped::make_fixed_layout(_session->_controller->configuration()._limits);
    if (layout)
    {
      for (const auto& section : layout->_sections)
      {
        if (section._kind == kind)
        {
          return section._offset;
        }
      }
    }
    runtime_fail(ErrorCode::IoError, "section", "Missing or invalid fixed section");
  }

  void runtime_validate_task_state(std::span<const std::uint64_t> words, TaskKind kind)
  {
    if (words.size() != 32 || (words[6] & ~std::uint64_t{7}) != 0 || words[8] > words[1] ||
        std::any_of(words.begin() + 10, words.end(), [](auto word)
        {
          return word != 0;
        }))
    {
      runtime_fail(ErrorCode::IoError, "task.state", "Task state has invalid flags, retirement floor, or reserved data");
    }
    if (words[0] == 0)
    {
      if (std::any_of(words.begin(), words.end(), [](auto word)
          {
            return word != 0;
          }))
      {
        runtime_fail(ErrorCode::IoError, "task.state", "Never-attached Task contains claim state");
      }
    }
    else if (words[3] == 0 || words[4] == 0 || (words[6] & task_attachment_time_flag) == 0 ||
             (kind == TaskKind::Process ? words[7] != 0 : words[7] == 0))
    {
      runtime_fail(ErrorCode::IoError, "task.state", "Task claim has invalid native diagnostics or parent incarnation");
    }
  }

  std::vector<LoadedTask> runtime_tasks(RuntimeAccess& access)
  {
    const auto capacity = access._session->_controller->configuration()._limits._tasks;
    const auto index = access.section_offset(mapped::SectionKind::TaskIndex);
    const auto states = access.section_offset(mapped::SectionKind::TaskState);
    const auto words = access.read_words(index, std::size_t{capacity} * 8);
    const auto stateWords = access.read_words(states, std::size_t{capacity} * 32);
    std::vector<LoadedTask> tasks;
    std::map<TaskId, TaskKind> kinds;
    std::set<std::string> names;
    for (std::size_t slot = 0; slot < capacity; ++slot)
    {
      const auto row = std::span{words}.subspan(slot * 8, 8);
      const auto state = std::span{stateWords}.subspan(slot * 32, 32);
      if (row[0] == 0)
      {
        if (std::any_of(row.begin(), row.end(), [](auto value)
            {
              return value != 0;
            }) ||
            std::any_of(state.begin(), state.end(), [](auto value)
            {
              return value != 0;
            }))
        {
          runtime_fail(ErrorCode::IoError, "task.index", "Empty Task index row contains data");
        }
        continue;
      }
      if (row[0] > capacity || row[1] == 0 || row[2] == 0 || row[2] > access._snapshot._revision ||
          std::any_of(row.begin() + 3, row.end(), [](auto value)
          {
            return value != 0;
          }))
      {
        runtime_fail(ErrorCode::IoError, "task.index", "Task index has invalid identity, revision, or reserved data");
      }
      auto definition = runtime_value(mapped::decode_task_definition(access.immutable_record(row[1], 2), row[1]));
      if (static_cast<std::uint16_t>(definition._id) != row[0] || definition._registration._revision != row[2] ||
          access.record_envelope(row[1], 2)._registryRevision != row[2] ||
          !kinds.emplace(definition._id, definition._descriptor._kind).second ||
          !names.insert(definition._descriptor._name).second)
      {
        runtime_fail(ErrorCode::IoError, "task.index", "Task index disagrees with unique registered definitions");
      }
      runtime_validate_task_state(state, definition._descriptor._kind);
      tasks.push_back(LoadedTask{std::move(definition), row[1], states + slot * mapped::task_state_bytes});
    }
    for (const auto& task : tasks)
    {
      const auto& definition = task._definition;
      if (!kinds.contains(definition._registration._createdBy))
      {
        runtime_fail(ErrorCode::IoError, "task.created_by", "Task registration names an unknown creator");
      }
      if (definition._descriptor._kind == TaskKind::Thread)
      {
        const auto parent = kinds.find(*definition._descriptor._process);
        if (parent == kinds.end() || parent->second != TaskKind::Process)
        {
          runtime_fail(ErrorCode::IoError, "task.process", "Thread Task parent is not a registered Process Task");
        }
      }
    }
    return tasks;
  }

  LoadedTask runtime_task(RuntimeAccess& access, TaskId task)
  {
    if (task == TaskId::Invalid)
    {
      runtime_fail(ErrorCode::InvalidArgument, "task", "Task identity is zero");
    }
    auto tasks = runtime_tasks(access);
    for (auto& candidate : tasks)
    {
      if (candidate._definition._id == task)
      {
        return std::move(candidate);
      }
    }
    runtime_fail(ErrorCode::NotFound, "task", "Task is not registered");
  }

  void runtime_validate_options(const SessionOptions& options, ErrorCode invalidCode)
  {
    if (!mapped::valid_log_limit(options._maxLogBytes) || !mapped::valid_capacities(options._limits) ||
        options._limits._tasks == 0 || !mapped::make_fixed_layout(options._limits) ||
        !mapped::ring_storage_bytes(options._defaults._ringCapacity))
    {
      runtime_fail(invalidCode, "session.capacity", "Invalid log limit, capacities, or default ring capacity");
    }
    if (options._storage == StorageMapping::TwoPlane && options._segmentBytes)
    {
      runtime_fail(invalidCode, "session.segment_bytes", "Two-plane storage does not accept segment_bytes");
    }
    if (options._storage != StorageMapping::TwoPlane && options._storage != StorageMapping::Segmented)
    {
      runtime_fail(invalidCode, "session.storage", "Unknown storage policy");
    }
    const auto& defaults = options._defaults;
    if ((defaults._selection != SelectionPolicy::Strict && defaults._selection != SelectionPolicy::Fair &&
         defaults._selection != SelectionPolicy::WeightedRandom) ||
        (defaults._wait != WaitPolicy::Spin && defaults._wait != WaitPolicy::EventFd) ||
        (defaults._delivery != DeliveryPolicy::RecoverFromLog && defaults._delivery != DeliveryPolicy::AllowLoss) ||
        (defaults._acceptance != AcceptancePolicy::AllDestinations && defaults._acceptance != AcceptancePolicy::Partial))
    {
      runtime_fail(invalidCode, "session.defaults", "Unknown default policy code");
    }
    if (options._storage != StorageMapping::TwoPlane || defaults._selection != SelectionPolicy::Strict ||
        defaults._wait != WaitPolicy::Spin || defaults._delivery != DeliveryPolicy::RecoverFromLog ||
        defaults._acceptance != AcceptancePolicy::AllDestinations)
    {
      runtime_fail(ErrorCode::UnsupportedPolicy, "session.defaults", "Requested native policy is not implemented yet");
    }
  }

  mapped::SessionDefinition runtime_session_definition(RuntimeAccess& access)
  {
    const auto reference = access.read_words(72, 1)[0];
    auto definition = runtime_value(mapped::decode_session_definition(access.immutable_record(reference, 1),
        reference, access._session->_controller->configuration()));
    runtime_validate_options(definition._options, ErrorCode::IoError);
    if (definition._policyFeatures != 0)
    {
      runtime_fail(ErrorCode::UnsupportedPolicy, "session.policy_features", "Session requires unsupported policy features");
    }
    const auto creator = runtime_task(access, definition._metadata._createdBy);
    if (creator._definition._descriptor._kind != TaskKind::Process ||
        creator._definition._registration._createdBy != definition._metadata._createdBy ||
        creator._definition._registration._revision != 1 ||
        creator._definition._registration._createdAt != definition._metadata._createdAt)
    {
      runtime_fail(ErrorCode::IoError, "session.creator", "Session creator registration is inconsistent");
    }
    return definition;
  }
} // namespace angaria::detail
