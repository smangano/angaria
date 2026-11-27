#include "mutation_journal.h"

#include "performance_diagnostics.h"

#include "angaria/detail/append_layout.h"
#include "angaria/detail/byte_operations.h"
#include "angaria/detail/checked_arithmetic.h"
#include "angaria/detail/error_support.h"
#include "journal_validation.h"
#include "loss_redo.h"
#include "mapped_read_window.h"
#include "native_atomic.h"
#include "task_claim_redo.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace angaria::detail::mapped
{
  namespace
  {
    struct JournalFailure
    {
      Error _error;
    };

    [[noreturn]] void fail(ErrorCode code, std::string message)
    {
      throw JournalFailure{Error{code, std::move(message), "journal"}};
    }

    Error caught_error()
    {
      try
      {
        throw;
      }
      catch (const JournalFailure& failure)
      {
        return failure._error;
      }
      catch (const std::exception& exception)
      {
        return Error{ErrorCode::IoError, exception.what(), "journal"};
      }
      catch (...)
      {
        return Error{ErrorCode::IoError, "Unexpected native journal failure", "journal"};
      }
    }

    std::size_t host_size(std::uint64_t bytes)
    {
      if (!fits_host_size(bytes))
      {
        fail(ErrorCode::IoError, "Journal range exceeds the host address space");
      }
      return static_cast<std::size_t>(bytes);
    }

    std::uint64_t add(std::uint64_t left, std::uint64_t right, ErrorCode code)
    {
      const auto sum = checked_add(left, right);
      if (!sum || *sum > max_log_bytes)
      {
        fail(code, "Journal byte arithmetic exceeds format bounds");
      }
      return *sum;
    }

    // Scan-local mappings avoid a native read/stat/map cycle for every record.
    // A window never extends beyond the prefix being validated, so recovery can
    // release it before shrinking an unaccepted append back to its original end.
    class PrefixReader
    {
    public:
      PrefixReader(NativeFile& file, std::uint64_t arena, std::uint64_t tail)
          : _arena(arena), _tail(tail), _window(file, tail)
      {
      }

      // Manifest validation and reference checks consume this view before the
      // next read. Retain the largest buffer until this scan ends; a later read
      // invalidates its contents even when it does not grow the allocation.
      std::span<const std::byte> read_bytes(std::uint64_t offset, std::uint64_t length)
      {
        check_range(offset, length);
        const auto count = host_size(length);
        if (count > _scratch.size())
        {
          _scratch.reserve(count);
          _scratch.resize(count);
        }
        const auto bytes = std::span{_scratch}.first(count);
        copy_bytes(offset, bytes);
        return bytes;
      }

      void copy_bytes(std::uint64_t offset, std::span<std::byte> output)
      {
        check_range(offset, output.size());
        value_or_throw<JournalFailure>(_window.copy_bytes(offset, output));
      }

      std::uint64_t load_acquire(std::uint64_t offset)
      {
        check_range(offset, sizeof(std::uint64_t));
        // Record readiness is eight-byte aligned. It cannot straddle a
        // 64 KiB boundary, and still uses the live mapped atomic adapter.
        return value_or_throw<JournalFailure>(_window.load_acquire(offset));
      }

    private:
      void check_range(std::uint64_t offset, std::uint64_t length) const
      {
        if (offset < _arena || !contains_range(_tail, offset, length))
        {
          fail(ErrorCode::IoError, "Journal read extends beyond the committed prefix");
        }
      }

      std::uint64_t _arena{};
      std::uint64_t _tail{};
      MappedReadWindow _window;
      std::vector<std::byte> _scratch;
    };

    std::vector<std::byte> read_bytes(NativeFile& file, std::uint64_t offset, std::uint64_t length)
    {
      if (!contains_range(value_or_throw<JournalFailure>(file.size()), offset, length))
      {
        fail(ErrorCode::IoError, "Journal read extends beyond the file");
      }
      std::vector<std::byte> bytes(host_size(length));
      value_or_throw<JournalFailure>(file.read_exact(offset, bytes));
      return bytes;
    }

    std::span<const std::byte> read_bytes(PrefixReader& reader, std::uint64_t offset, std::uint64_t length)
    {
      return reader.read_bytes(offset, length);
    }

    std::uint64_t load_at(NativeFile& file, std::uint64_t offset)
    {
      auto mapping = value_or_throw<JournalFailure>(file.map_read_write(offset, 8));
      return value_or_throw<JournalFailure>(AtomicWord::bind(value_or_throw<JournalFailure>(mapping.writable_bytes()), 0)).load_acquire();
    }

    void store_at(NativeFile& file, std::uint64_t offset, std::uint64_t value)
    {
      auto mapping = value_or_throw<JournalFailure>(file.map_read_write(offset, 8));
      value_or_throw<JournalFailure>(AtomicWord::bind(value_or_throw<JournalFailure>(mapping.writable_bytes()), 0)).store_release(value);
    }

    std::uint64_t load_at(PrefixReader& reader, std::uint64_t offset)
    {
      return reader.load_acquire(offset);
    }

    void copy_bytes(NativeFile& file, std::uint64_t offset, std::span<std::byte> output)
    {
      value_or_throw<JournalFailure>(file.read_exact(offset, output));
    }

    void copy_bytes(PrefixReader& reader, std::uint64_t offset, std::span<std::byte> output)
    {
      reader.copy_bytes(offset, output);
    }

    template <typename Reader>
    RecordEnvelope read_envelope(Reader& file, std::uint64_t offset)
    {
      RecordEnvelopeBytes bytes{};
      // Readiness is always atomic, including when assembling a local decoder input.
      copy_bytes(file, offset, std::span{bytes}.first(48));
      write_unsigned_le(bytes, 48, load_at(file, add(offset, 48, ErrorCode::IoError)));
      copy_bytes(file, add(offset, 56, ErrorCode::IoError), std::span{bytes}.subspan(56));
      return value_or_throw<JournalFailure>(decode_record_envelope(bytes));
    }

    struct PrefixRecords
    {
      std::vector<JournalRecordRange> _ranges;
      std::uint64_t _manifest{};
      std::uint64_t _revision{};
    };

    void validate_previous(const JournalRecordRange& record, std::span<const JournalRecordRange> earlier,
                           ErrorCode code)
    {
      const auto previous = record._envelope._previous;
      if (previous == 0)
      {
        return;
      }
      const auto* found = find_journal_record(earlier, previous);
      const auto kind = record._envelope._kind;
      if (!found ||
          (found->_envelope._kind != kind && !(kind == 10 && found->_envelope._kind == 9)))
      {
        fail(code, "Record predecessor is not an earlier compatible record boundary");
      }
    }

    template <typename Reader>
    JournalRecordRange read_range(Reader& file, std::uint64_t offset, std::uint64_t end,
                                  std::uint64_t arena)
    {
      if (!contains_range(end, offset, record_envelope_bytes))
      {
        fail(ErrorCode::IoError, "Record envelope is outside its allocation plan");
      }
      const auto envelope = read_envelope(file, offset);
      if (!contains_range(end, offset, envelope._totalBytes))
      {
        fail(ErrorCode::IoError, "Record extends beyond its allocation plan");
      }
      std::array<std::byte, 128> bodyBytes{};
      const auto length = static_cast<std::size_t>(std::min<std::uint64_t>(bodyBytes.size(), envelope._immutableBytes));
      const auto body = std::span{bodyBytes}.first(length);
      copy_bytes(file, offset + record_envelope_bytes, body);
      return value_or_throw<JournalFailure>(validate_journal_record(offset, envelope, body, arena));
    }

    template <typename Reader>
    Manifest read_manifest(Reader& file, const JournalRecordRange& record)
    {
      return value_or_throw<JournalFailure>(decode_manifest_body(read_bytes(file, record._offset + record_envelope_bytes,
                                                  record._envelope._immutableBytes), record._offset));
    }

    template <typename ReferenceAt>
    void check_manifest_records(std::size_t count, ReferenceAt&& referenceAt,
                                  std::span<const JournalRecordRange> records)
    {
      if (count != records.size())
      {
        fail(ErrorCode::IoError, "Manifest record count disagrees with the append batch");
      }
      for (std::size_t index = 0; index != records.size(); ++index)
      {
        if (referenceAt(index) != records[index]._offset)
        {
          fail(ErrorCode::IoError, "Manifest references do not name the complete ordered batch");
        }
      }
    }

    PrefixRecords scan_prefix(NativeFile& file, std::uint64_t arena, std::uint64_t tail,
                               std::uint64_t generation)
    {
      ANGARIA_DIAGNOSTIC_SCOPE(PrefixScan);
      ANGARIA_DIAGNOSTIC_COUNT(PrefixScans, 1);
      PrefixRecords prefix;
      if (tail < arena || tail % section_alignment != 0 || tail > value_or_throw<JournalFailure>(file.size()))
      {
        fail(ErrorCode::IoError, "Committed allocation tail is invalid");
      }
      PrefixReader reader(file, arena, tail);
      std::size_t groupStart{};
      std::uint64_t groupGeneration{};
      std::uint64_t previousGeneration{};
      for (auto offset = arena; offset < tail;)
      {
        ANGARIA_DIAGNOSTIC_COUNT(PrefixRecords, 1);
        auto record = read_range(reader, offset, tail, arena);
        const auto& envelope = record._envelope;
        if (envelope._readiness != 2 || envelope._generation > generation)
        {
          fail(ErrorCode::IoError, "Committed prefix contains an uncommitted or future record");
        }
        validate_previous(record, prefix._ranges, ErrorCode::IoError);
        if (groupGeneration == 0)
        {
          groupGeneration = envelope._generation;
          if (groupGeneration <= previousGeneration)
          {
            fail(ErrorCode::IoError, "Append generations do not increase");
          }
        }
        if (envelope._generation != groupGeneration)
        {
          fail(ErrorCode::IoError, "Append batch lacks its final manifest");
        }
        if (envelope._kind == 11)
        {
          // This view borrows the reader's scratch only until the next read.
          // Consume all references here; exported prefix ranges own values.
          const auto manifest = value_or_throw<JournalFailure>(validate_manifest_body(
              read_bytes(reader, offset + record_envelope_bytes, envelope._immutableBytes), offset));
          if (prefix._ranges.size() == groupStart || envelope._previous != prefix._manifest ||
              manifest._baseRevision != prefix._revision || manifest._newRevision != envelope._registryRevision)
          {
            fail(ErrorCode::IoError, "Committed manifest chain or Registry revision is invalid");
          }
          check_manifest_records(manifest._recordReferences.size() / sizeof(std::uint64_t),
              [&](std::size_t index)
              {
                return read_unsigned_le<std::uint64_t>(manifest._recordReferences, index * 8);
              }, std::span{prefix._ranges}.subspan(groupStart));
          for (const auto& member : std::span{prefix._ranges}.subspan(groupStart))
          {
            if (member._envelope._registryRevision != manifest._newRevision)
            {
              fail(ErrorCode::IoError, "Committed record revision disagrees with its manifest");
            }
          }
          prefix._manifest = offset;
          prefix._revision = manifest._newRevision;
          previousGeneration = groupGeneration;
          groupGeneration = 0;
          groupStart = prefix._ranges.size() + 1;
        }
        prefix._ranges.push_back(record);
        offset += envelope._totalBytes;
      }
      if (groupGeneration != 0)
      {
        fail(ErrorCode::IoError, "Committed prefix ends before a manifest");
      }
      return prefix;
    }
  } // namespace

  struct MutationJournalState
  {
    std::shared_ptr<SessionController> _controller;
    MappedRegion _prefix;
    JournalFaultInjector* _injector{};
  };

  namespace
  {
    void checkpoint(MutationJournalState& state, JournalCheckpoint point, std::uint64_t index = 0)
    {
      if (state._injector != nullptr)
      {
        value_or_throw<JournalFailure>(state._injector->checkpoint(point, index));
      }
    }

    std::span<std::byte> prefix_bytes(MutationJournalState& state)
    {
      return value_or_throw<JournalFailure>(state._prefix.writable_bytes());
    }

    std::uint64_t prefix_word(MutationJournalState& state, std::size_t offset)
    {
      return value_or_throw<JournalFailure>(AtomicWord::bind(prefix_bytes(state), offset)).load_acquire();
    }

    JournalControl control(MutationJournalState& state)
    {
      const auto value = prefix_word(state, journal_offset);
      if (value > static_cast<std::uint64_t>(JournalControl::Applied))
      {
        fail(ErrorCode::IoError, "Invalid journal control word");
      }
      return static_cast<JournalControl>(value);
    }

    void publish(MutationJournalState& state, JournalControl value, JournalCheckpoint point)
    {
      value_or_throw<JournalFailure>(AtomicWord::bind(prefix_bytes(state), journal_offset)).publish_control(static_cast<std::uint64_t>(value));
      checkpoint(state, point);
    }

    JournalSnapshot roots(MutationJournalState& state, NativeFile& file)
    {
      return JournalSnapshot{._generation = prefix_word(state, 120),
                             ._tail = prefix_word(state, 88),
                             ._accounted = prefix_word(state, 176),
                             ._revision = prefix_word(state, 80),
                             ._manifest = prefix_word(state, 96),
                             ._fileBytes = value_or_throw<JournalFailure>(file.size())};
    }

    FixedLayout fixed_layout(const MutationJournalState& state)
    {
      const auto layout = make_fixed_layout(state._controller->configuration()._limits);
      if (!layout)
      {
        fail(ErrorCode::IoError, "Controller has an invalid fixed layout");
      }
      return *layout;
    }

    JournalSnapshot idle_roots(MutationJournalState& state, NativeFile& file)
    {
      if (control(state) != JournalControl::Idle)
      {
        fail(ErrorCode::IoError, "Journal recovery is required");
      }
      return roots(state, file);
    }

    JournalSnapshot clean_snapshot(MutationJournalState& state, NativeFile& file,
                                    PrefixRecords* validated = nullptr)
    {
      ANGARIA_DIAGNOSTIC_SCOPE(Snapshot);
      const auto snapshot = idle_roots(state, file);
      auto records = scan_prefix(file, fixed_layout(state)._arenaBegin, snapshot._tail, snapshot._generation);
      if (snapshot._tail != snapshot._fileBytes || snapshot._accounted != snapshot._tail ||
          snapshot._manifest != records._manifest || snapshot._revision != records._revision)
      {
        fail(ErrorCode::IoError, "Canonical roots disagree with committed storage");
      }
      if (validated)
      {
        *validated = std::move(records);
      }
      return snapshot;
    }

    JournalHeader read_header(MutationJournalState& state, JournalControl observed)
    {
      JournalHeaderBytes bytes{};
      const auto prefix = prefix_bytes(state);
      std::copy_n(prefix.begin() + journal_offset + 8, journal_header_bytes - 8, bytes.begin() + 8);
      write_unsigned_le(bytes, 0, static_cast<std::uint64_t>(observed));
      const auto header = value_or_throw<JournalFailure>(decode_journal_header(bytes));
      const auto unused = prefix.subspan(journal_offset + journal_header_bytes + header._inlineBytes,
                                         inline_command_bytes - header._inlineBytes);
      if (!all_zero(unused))
      {
        fail(ErrorCode::IoError, "Unused journal command bytes are not zero");
      }
      return header;
    }

    void validate_header_context(const MutationJournalState& state, const JournalHeader& header)
    {
      const auto arena = fixed_layout(state)._arenaBegin;
      const auto limit = state._controller->configuration()._maxLogBytes;
      if (header._fileLogicalStart != 0 || header._newExtentBytes != 0 || header._baseTail < arena ||
          header._baseTail % 64 != 0 || header._newTail % 64 != 0 || header._newTail < header._baseTail ||
          header._baseAccounted != header._baseTail || header._newAccounted != header._newTail ||
          header._originalFileBytes != header._baseTail || header._newFileBytes != header._newTail ||
          header._newTail > limit || static_cast<std::uint16_t>(header._actor) >
                                        state._controller->configuration()._limits._tasks)
      {
        fail(ErrorCode::IoError, "Journal plan is incompatible with two-plane storage or its recorded limits");
      }
      if (header._kind == JournalKind::Append &&
          (header._recordCount < 2 || header._firstRecord != header._baseTail ||
           header._manifest % 64 != 0 || header._manifest < header._baseTail || header._manifest >= header._newTail))
      {
        fail(ErrorCode::IoError, "Append journal geometry is invalid");
      }
      if (header._kind == JournalKind::Append)
      {
        const auto members = std::uint64_t{header._recordCount - 1};
        const auto minimumManifest = *checked_align(128 + members * 8 + 24, 64);
        if (members > (header._manifest - header._baseTail) / 128 ||
            header._newTail - header._manifest < minimumManifest ||
            (header._newRevision != header._baseRevision &&
             (header._baseRevision == std::numeric_limits<std::uint64_t>::max() ||
              header._newRevision != header._baseRevision + 1)))
        {
          fail(ErrorCode::IoError, "Append record count, manifest space, or revision is impossible");
        }
      }
      if (header._kind == JournalKind::FixedState && header._inlineBytes == 0)
      {
        fail(ErrorCode::IoError, "FixedState journal has no commands");
      }
    }

    bool old_or_new(std::uint64_t value, std::uint64_t oldValue, std::uint64_t newValue)
    {
      return value == oldValue || value == newValue;
    }

    void validate_recovery_roots(const JournalHeader& header, const JournalSnapshot& current,
                                 const PrefixRecords& prefix, bool prepared)
    {
      if (prefix._revision != header._baseRevision)
      {
        fail(ErrorCode::IoError, "Journal base revision disagrees with retained history");
      }
      if (prepared)
      {
        if (current._generation != header._generation - 1 || current._tail != header._baseTail ||
            current._accounted != header._baseAccounted || current._revision != header._baseRevision ||
            current._manifest != prefix._manifest || current._fileBytes < header._originalFileBytes ||
            current._fileBytes > header._newFileBytes)
        {
          fail(ErrorCode::IoError, "Prepared journal has unexpected canonical roots or file length");
        }
      }
      else if (!old_or_new(current._generation, header._generation - 1, header._generation) ||
               !old_or_new(current._tail, header._baseTail, header._newTail) ||
               !old_or_new(current._accounted, header._baseAccounted, header._newAccounted) ||
               !old_or_new(current._revision, header._baseRevision, header._newRevision) ||
               !old_or_new(current._manifest, prefix._manifest,
                            header._kind == JournalKind::Append ? header._manifest : prefix._manifest) ||
               current._fileBytes != header._newFileBytes)
      {
        fail(ErrorCode::IoError, "Committed journal has unexpected roots or an incomplete allocation");
      }
    }

    struct AcceptedBatch
    {
      std::vector<JournalRecordRange> _records;
      std::vector<RedoCommand> _commands;
    };

    bool has_compact(std::span<const RedoCommand> commands)
    {
      return std::any_of(commands.begin(), commands.end(), [](const auto& command)
      {
        return command._opcode != RedoOpcode::WriteRange;
      });
    }

    void require_fixed_compacts(std::span<const RedoCommand> commands, JournalKind kind, ErrorCode code)
    {
      if (kind != JournalKind::FixedState && std::any_of(commands.begin(), commands.end(), [](const auto& command)
          {
            return command._opcode == RedoOpcode::ReleaseClaim || command._opcode == RedoOpcode::LoseRange;
          }))
      {
        fail(code == ErrorCode::InvalidArgument ? ErrorCode::UnsupportedPolicy : code,
             "Claim cleanup and loss require FixedState so replay discovery remains stable");
      }
    }

    void validate_domain_targets(MutationJournalState& state, NativeFile& file, const FixedLayout& layout,
                                 std::span<const JournalRecordRange> records,
                                 std::span<const RedoCommand> commands, JournalKind kind,
                                 TaskId actor, std::uint64_t incarnation,
                                 ErrorCode code = ErrorCode::IoError)
    {
      require_fixed_compacts(commands, kind, code);
      const auto features = state._controller->configuration()._requiredFeatures;
      const bool allowLoss = (features & allow_loss_feature) != 0;
      if (has_loss_command(commands))
      {
        value_or_throw<JournalFailure>(validate_loss_commands(file, layout, records, commands,
            actor, incarnation, allowLoss, false, code));
        value_or_throw<JournalFailure>(validate_redo_targets(commands.subspan(1), layout, records, code));
      }
      else if (has_compact(commands))
      {
        const bool nativeDelivery = (state._controller->configuration()._requiredFeatures & native_delivery_feature) != 0;
        const auto expanded = value_or_throw<JournalFailure>(expand_task_claim_commands(
            file, layout, records, commands, code, nativeDelivery, allowLoss));
        value_or_throw<JournalFailure>(validate_redo_targets(expanded, layout, records, code));
      }
      else
      {
        value_or_throw<JournalFailure>(validate_redo_targets(commands, layout, records, code));
      }
    }

    AcceptedBatch validate_accepted(MutationJournalState& state, NativeFile& file,
                                    const JournalHeader& header, const PrefixRecords& prefix)
    {
      ANGARIA_DIAGNOSTIC_SCOPE(Materialize);
      AcceptedBatch batch;
      auto allRecords = prefix._ranges;
      if (header._kind == JournalKind::FixedState)
      {
        const auto bytes = prefix_bytes(state).subspan(journal_offset + journal_header_bytes, header._inlineBytes);
        batch._commands = value_or_throw<JournalFailure>(decode_commands(bytes));
      }
      else
      {
        for (auto offset = header._baseTail; offset < header._newTail;)
        {
          auto record = read_range(file, offset, header._newTail, fixed_layout(state)._arenaBegin);
          const auto& envelope = record._envelope;
          if ((envelope._readiness != 1 && envelope._readiness != 2) ||
              envelope._generation != header._generation || envelope._registryRevision != header._newRevision ||
              (envelope._kind == 11 && offset != header._manifest))
          {
            fail(ErrorCode::IoError, "Accepted batch contains an invalid record");
          }
          validate_previous(record, allRecords, ErrorCode::IoError);
          batch._records.push_back(record);
          allRecords.push_back(record);
          offset += envelope._totalBytes;
        }
        if (batch._records.size() != header._recordCount || batch._records.empty() ||
            batch._records.back()._offset != header._manifest || batch._records.back()._envelope._kind != 11 ||
            batch._records.back()._envelope._previous != prefix._manifest)
        {
          fail(ErrorCode::IoError, "Accepted manifest does not terminate the complete batch");
        }
        const auto manifest = read_manifest(file, batch._records.back());
        if (manifest._actor != header._actor || manifest._actorIncarnation != header._actorIncarnation ||
            manifest._baseRevision != header._baseRevision || manifest._newRevision != header._newRevision)
        {
          fail(ErrorCode::IoError, "Accepted manifest disagrees with its journal");
        }
        check_manifest_records(manifest._records.size(), [&](std::size_t index)
        {
          return manifest._records[index];
        }, std::span{batch._records}.first(batch._records.size() - 1));
        batch._commands = manifest._commands;
      }
      require_fixed_compacts(batch._commands, header._kind, ErrorCode::IoError);
      const bool loss = has_loss_command(batch._commands);
      const auto features = state._controller->configuration()._requiredFeatures;
      const bool allowLoss = (features & allow_loss_feature) != 0;
      if (loss)
      {
        value_or_throw<JournalFailure>(validate_loss_commands(file, fixed_layout(state), allRecords,
            batch._commands, header._actor, header._actorIncarnation, allowLoss,
            header._control != JournalControl::Prepared));
      }
      else if (has_compact(batch._commands))
      {
        const bool nativeDelivery = (state._controller->configuration()._requiredFeatures & native_delivery_feature) != 0;
        batch._commands = value_or_throw<JournalFailure>(expand_task_claim_commands(
            file, fixed_layout(state), allRecords, batch._commands, ErrorCode::IoError, nativeDelivery, allowLoss));
      }
      value_or_throw<JournalFailure>(validate_redo_targets(
          std::span{batch._commands}.subspan(loss ? 1 : 0), fixed_layout(state), allRecords));
      return batch;
    }

    void apply_accepted(MutationJournalState& state, NativeFile& file, const JournalHeader& header,
                         const PrefixRecords& prefix, const AcceptedBatch& batch)
    {
      ANGARIA_DIAGNOSTIC_SCOPE(CommitApply);
      std::uint64_t word{};
      for (const auto& command : batch._commands)
      {
        if (command._opcode == RedoOpcode::LoseRange)
        {
          value_or_throw<JournalFailure>(apply_loss_command(file, fixed_layout(state), prefix._ranges,
              command, [&]
              {
                checkpoint(state, JournalCheckpoint::RedoWord, word++);
              }));
          continue;
        }
        // Retain one checked mapping for the command, including across fault
        // checkpoints, rather than mapping and unmapping every eight-byte word.
        auto mapping = value_or_throw<JournalFailure>(file.map_read_write(command._target, command._payload.size()));
        const auto bytes = value_or_throw<JournalFailure>(mapping.writable_bytes());
        for (std::size_t offset = 0; offset < command._payload.size(); offset += 8)
        {
          value_or_throw<JournalFailure>(AtomicWord::bind(bytes, offset)).store_release(read_unsigned_le<std::uint64_t>(command._payload, offset));
          checkpoint(state, JournalCheckpoint::RedoWord, word++);
        }
      }
      for (std::size_t index = 0; index != batch._records.size(); ++index)
      {
        store_at(file, batch._records[index]._offset + 48, 2);
        checkpoint(state, JournalCheckpoint::RecordCommitted, index);
      }
      const std::array<std::pair<std::size_t, std::uint64_t>, 5> epilogue{{
          {80, header._newRevision}, {88, header._newTail}, {176, header._newAccounted},
          {96, header._kind == JournalKind::Append ? header._manifest : prefix._manifest},
          {120, header._generation},
      }};
      for (std::size_t index = 0; index != epilogue.size(); ++index)
      {
        value_or_throw<JournalFailure>(AtomicWord::bind(prefix_bytes(state), epilogue[index].first)).store_release(epilogue[index].second);
        checkpoint(state, JournalCheckpoint::EpilogueWord, index);
      }
      publish(state, JournalControl::Applied, JournalCheckpoint::Applied);
      publish(state, JournalControl::Idle, JournalCheckpoint::Idle);
    }

    // True means recovery changed or replayed journal state. A caller may not
    // reuse validation captured before that repair, even if roots look equal.
    bool recover_journal(MutationJournalState& state, NativeFile& file)
    {
      ANGARIA_DIAGNOSTIC_SCOPE(Recovery);
      ANGARIA_DIAGNOSTIC_COUNT(RecoveryChecks, 1);
      const auto observed = control(state);
      if (observed == JournalControl::Idle)
      {
        return false;
      }
      // Count non-idle recovery attempts, including Prepared rollback. This is
      // distinct from the inexpensive Idle check made by healthy operations.
      ANGARIA_DIAGNOSTIC_COUNT(RecoveryReplays, 1);
      const auto header = read_header(state, observed);
      validate_header_context(state, header);
      const auto prefix = scan_prefix(file, fixed_layout(state)._arenaBegin, header._baseTail, header._generation - 1);
      validate_recovery_roots(header, roots(state, file), prefix, observed == JournalControl::Prepared);
      if (observed == JournalControl::Prepared)
      {
        if (header._kind == JournalKind::Append)
        {
          value_or_throw<JournalFailure>(file.resize(header._originalFileBytes));
        }
        else
        {
          const auto commands = value_or_throw<JournalFailure>(decode_commands(prefix_bytes(state).subspan(
              journal_offset + journal_header_bytes, header._inlineBytes)));
          validate_domain_targets(state, file, fixed_layout(state), prefix._ranges, commands, header._kind,
                                  header._actor, header._actorIncarnation);
        }
        checkpoint(state, JournalCheckpoint::Rollback);
        publish(state, JournalControl::Idle, JournalCheckpoint::Idle);
        return true;
      }
      const auto batch = validate_accepted(state, file, header, prefix);
      apply_accepted(state, file, header, prefix, batch);
      return true;
    }

    struct RecordImage
    {
      std::uint64_t _offset{};
      std::vector<std::byte> _bytes;
      JournalRecordRange _range;
    };

    RecordImage make_image(const JournalRecord& draft, std::uint64_t offset,
                            std::uint64_t generation, std::uint64_t revision, std::uint64_t arena,
                            std::uint64_t limit)
    {
      if (draft._kind == 8 && (draft._mutable.empty() || draft._mutable.size() % ring_slot_bytes != 0))
      {
        fail(ErrorCode::InvalidArgument, "Ring initialization requires complete slots");
      }
      if (draft._kind == 9)
      {
        const auto receipts = draft._immutable.size() >= 128 ?
            checked_multiply(read_unsigned_le<std::uint64_t>(draft._immutable, 104), 128) : std::nullopt;
        if (!receipts || *receipts == 0 || *receipts != draft._mutable.size())
        {
          fail(ErrorCode::InvalidArgument, "Publication initialization must contain its complete receipt array");
        }
        for (std::size_t slot = 0; slot < draft._mutable.size(); slot += 128)
        {
          if (!all_zero(std::span{draft._mutable}.subspan(slot + 56, 72)))
          {
            fail(ErrorCode::InvalidArgument, "Publication initialization sets reserved receipt bytes");
          }
        }
      }
      const auto layout = record_storage_layout(draft._immutable.size(), draft._mutable.size());
      if (!layout)
      {
        fail(ErrorCode::InvalidArgument, "Proposed record size overflows");
      }
      if (!contains_range(limit, offset, layout->_totalBytes))
      {
        fail(ErrorCode::LogFull, "Proposed record exceeds the remaining session byte budget");
      }
      RecordEnvelope envelope;
      envelope._kind = draft._kind;
      envelope._encoding = draft._encoding;
      envelope._totalBytes = layout->_totalBytes;
      envelope._immutableBytes = layout->_immutableBytes;
      envelope._generation = generation;
      envelope._previous = draft._previous;
      envelope._registryRevision = revision;
      const auto encoded = value_or_throw<JournalFailure>(encode_record_envelope(envelope));
      RecordImage image;
      image._offset = offset;
      image._range = value_or_throw<JournalFailure>(validate_journal_record(offset, envelope, draft._immutable,
                                                 arena, ErrorCode::InvalidArgument));
      image._bytes.resize(host_size(layout->_totalBytes));
      std::copy(encoded.begin(), encoded.end(), image._bytes.begin());
      std::copy(draft._immutable.begin(), draft._immutable.end(), image._bytes.begin() + record_envelope_bytes);
      std::copy(draft._mutable.begin(), draft._mutable.end(),
                image._bytes.begin() + host_size(layout->_mutableOffset));
      return image;
    }

    struct PreparedOperation
    {
      JournalHeader _header;
      JournalHeaderBytes _headerBytes;
      std::vector<std::byte> _commands;
      std::vector<RecordImage> _images;
      PrefixRecords _prefix;
      TransactionResult _result;
    };

    PreparedOperation preflight(MutationJournalState& state, NativeFile& file, const TransactionPlan& plan,
                                 const JournalSnapshot* validated,
                                 std::span<const JournalRecordRange> records)
    {
      ANGARIA_DIAGNOSTIC_SCOPE(JournalPreflight);
      PrefixRecords prefix;
      JournalSnapshot current;
      if (validated && idle_roots(state, file) == *validated)
      {
        // Only RuntimeAccess can offer this first-attempt proof. Its captured
        // prefix remains immutable under the same held gate. Check live roots
        // and physical length, but do not walk that unchanged prefix twice.
        current = *validated;
        prefix._ranges.assign(records.begin(), records.end());
        prefix._manifest = validated->_manifest;
        prefix._revision = validated->_revision;
      }
      else
      {
        // A changed state still receives full validation before stale-plan
        // classification, preserving corruption/recovery error precedence.
        current = clean_snapshot(state, file, &prefix);
      }
      if (plan._base != current)
      {
        fail(ErrorCode::Conflict, "Transaction plan was captured before another completed operation");
      }
      if (current._generation == std::numeric_limits<std::uint64_t>::max())
      {
        fail(ErrorCode::CapacityExceeded, "Operation generation is exhausted");
      }
      if (static_cast<std::uint16_t>(plan._actor) > state._controller->configuration()._limits._tasks ||
          ((plan._actor == TaskId::Invalid) != (plan._actorIncarnation == 0)))
      {
        fail(ErrorCode::InvalidArgument, "Invalid transaction actor identity");
      }
      if (plan._commands.empty())
      {
        fail(ErrorCode::InvalidArgument, "An empty operation must not allocate or consume a generation");
      }
      if (plan._kind != JournalKind::FixedState && plan._kind != JournalKind::Append)
      {
        fail(ErrorCode::InvalidArgument, "Unknown journal operation kind");
      }
      require_fixed_compacts(plan._commands, plan._kind, ErrorCode::InvalidArgument);
      std::uint64_t commandBytes{};
      for (const auto& command : plan._commands)
      {
        commandBytes = add(commandBytes, add(16, command._payload.size(), ErrorCode::InvalidArgument),
                            ErrorCode::InvalidArgument);
      }
      const auto commandBudget = plan._kind == JournalKind::FixedState ? inline_command_bytes :
          state._controller->configuration()._maxLogBytes - current._tail;
      if (commandBytes > commandBudget)
      {
        fail(plan._kind == JournalKind::FixedState ? ErrorCode::CapacityExceeded : ErrorCode::LogFull,
             "Command stream exceeds its transaction byte budget");
      }
      std::optional<AppendStorageLayout> appendLayout;
      if (plan._kind == JournalKind::Append)
      {
        if (plan._subtype == TransactionSubtype::Registry &&
            current._revision == std::numeric_limits<std::uint64_t>::max())
        {
          fail(ErrorCode::CapacityExceeded, "Registry revision is exhausted");
        }
        if (plan._records.empty() || plan._records.size() >= std::numeric_limits<std::uint32_t>::max())
        {
          fail(ErrorCode::InvalidArgument, "Append requires a representable nonempty record batch");
        }
        // Account for the COMPLETE batch before encoding redo or copying the first
        // payload. A record may fit while its references/manifest/redo do not.
        AppendLayoutPlanner planner(current._tail);
        for (const auto& draft : plan._records)
        {
          if (draft._kind == 11)
          {
            fail(ErrorCode::InvalidArgument, "The journal owns the batch's sole final manifest");
          }
          const auto storage = record_storage_layout(draft._immutable.size(), draft._mutable.size());
          if (!storage || !fits_host_size(storage->_totalBytes))
          {
            fail(ErrorCode::InvalidArgument, "Proposed record size exceeds addressable bounds");
          }
          (void)planner.add_record(draft._immutable.size(), draft._mutable.size());
        }
        appendLayout = planner.finish(commandBytes, state._controller->configuration()._maxLogBytes);
        if (!appendLayout)
        {
          fail(ErrorCode::LogFull, "Complete append batch and manifest exceed the session byte ceiling");
        }
        if (!fits_host_size(appendLayout->_manifestBytes))
        {
          fail(ErrorCode::InvalidArgument, "Proposed manifest size exceeds addressable bounds");
        }
      }
      PreparedOperation operation;
      // These exact records were validated under the continuously held gate,
      // either at runtime entry or by preflight's fresh scan. Only local images
      // are built before execution begins.
      operation._prefix = std::move(prefix);
      operation._commands = value_or_throw<JournalFailure>(encode_commands(plan._commands));
      auto& header = operation._header;
      header._control = JournalControl::Prepared;
      header._generation = current._generation + 1;
      header._kind = plan._kind;
      header._baseTail = header._newTail = current._tail;
      header._baseAccounted = header._newAccounted = current._accounted;
      header._actor = plan._actor;
      header._actorIncarnation = plan._actorIncarnation;
      header._originalFileBytes = header._newFileBytes = current._fileBytes;
      header._baseRevision = current._revision;
      header._newRevision = plan._newRevision;
      auto allRecords = operation._prefix._ranges;
      if (plan._kind == JournalKind::FixedState)
      {
        if (!plan._records.empty() || plan._newRevision != current._revision)
        {
          fail(ErrorCode::InvalidArgument, "FixedState cannot allocate records or change Registry revision");
        }
        if (operation._commands.size() > inline_command_bytes)
        {
          fail(ErrorCode::CapacityExceeded, "FixedState commands exceed the reserved inline journal");
        }
        header._inlineBytes = static_cast<std::uint32_t>(operation._commands.size());
      }
      else if (plan._kind == JournalKind::Append)
      {
        std::uint64_t offset = current._tail;
        const auto arena = fixed_layout(state)._arenaBegin;
        for (const auto& draft : plan._records)
        {
          auto image = make_image(draft, offset, header._generation, header._newRevision, arena,
                                   state._controller->configuration()._maxLogBytes);
          validate_previous(image._range, allRecords, ErrorCode::InvalidArgument);
          allRecords.push_back(image._range);
          operation._result._records.push_back(offset);
          offset = add(offset, image._bytes.size(), ErrorCode::LogFull);
          operation._images.push_back(std::move(image));
        }
        Manifest manifest;
        manifest._subtype = plan._subtype;
        manifest._actor = plan._actor;
        manifest._actorIncarnation = plan._actorIncarnation;
        manifest._baseRevision = current._revision;
        manifest._newRevision = plan._newRevision;
        manifest._records = operation._result._records;
        manifest._commands = plan._commands;
        JournalRecord draft;
        draft._kind = 11;
        draft._previous = current._manifest;
        draft._immutable = value_or_throw<JournalFailure>(encode_manifest_body(manifest, offset));
        auto image = make_image(draft, offset, header._generation, header._newRevision, arena,
                                 state._controller->configuration()._maxLogBytes);
        allRecords.push_back(image._range);
        header._manifest = offset;
        header._newTail = add(offset, image._bytes.size(), ErrorCode::LogFull);
        header._newAccounted = header._newTail;
        header._newFileBytes = header._newTail;
        header._firstRecord = current._tail;
        header._recordCount = static_cast<std::uint32_t>(operation._images.size() + 1);
        operation._images.push_back(std::move(image));
        if (offset != appendLayout->_manifestOffset || header._newTail != appendLayout->_tail)
        {
          fail(ErrorCode::InvalidState, "Materialized append disagrees with its complete allocation plan");
        }
      }
      else
      {
        fail(ErrorCode::InvalidArgument, "Unknown journal operation kind");
      }
      validate_domain_targets(state, file, fixed_layout(state), allRecords, plan._commands,
                              plan._kind, plan._actor, plan._actorIncarnation, ErrorCode::InvalidArgument);
      operation._headerBytes = value_or_throw<JournalFailure>(encode_journal_header(header));
      operation._result._generation = header._generation;
      operation._result._tail = header._newTail;
      operation._result._manifest = header._kind == JournalKind::Append ? header._manifest : current._manifest;
      return operation;
    }

    void execute_prepared(MutationJournalState& state, NativeFile& file, const PreparedOperation& operation)
    {
      ANGARIA_DIAGNOSTIC_SCOPE(JournalBody);
      std::array<std::byte, journal_bytes> bytes{};
      std::copy(operation._headerBytes.begin(), operation._headerBytes.end(), bytes.begin());
      if (operation._header._kind == JournalKind::FixedState)
      {
        std::copy(operation._commands.begin(), operation._commands.end(), bytes.begin() + journal_header_bytes);
      }
      const auto prefix = prefix_bytes(state);
      for (std::size_t offset = 8; offset < bytes.size(); offset += 8)
      {
        std::copy_n(bytes.begin() + offset, 8, prefix.begin() + journal_offset + offset);
        checkpoint(state, JournalCheckpoint::JournalBodyWord, offset / 8 - 1);
      }
      publish(state, JournalControl::Prepared, JournalCheckpoint::Prepared);
      if (operation._header._kind == JournalKind::Append)
      {
        ANGARIA_DIAGNOSTIC_SCOPE(Materialize);
        value_or_throw<JournalFailure>(file.resize(operation._header._newFileBytes));
        checkpoint(state, JournalCheckpoint::FileGrowth);
        // This mapping is released on every return/unwind before rollback can shrink.
        auto mapping = value_or_throw<JournalFailure>(file.map_read_write(operation._header._baseTail,
                                                operation._header._newTail - operation._header._baseTail));
        const auto appendBytes = value_or_throw<JournalFailure>(mapping.writable_bytes());
        std::uint64_t word{};
        for (std::size_t index = 0; index != operation._images.size(); ++index)
        {
          const auto& image = operation._images[index];
          const auto start = host_size(image._offset - operation._header._baseTail);
          auto readiness = value_or_throw<JournalFailure>(AtomicWord::bind(appendBytes, start + 48));
          readiness.store_release(0);
          for (std::size_t offset = 0; offset != image._bytes.size(); offset += 8)
          {
            if (offset != 48)
            {
              std::copy_n(image._bytes.begin() + offset, 8, appendBytes.begin() + start + offset);
              checkpoint(state, JournalCheckpoint::RecordBodyWord, word++);
            }
          }
          readiness.store_release(1);
          checkpoint(state, JournalCheckpoint::RecordReady, index);
        }
      }
      // Re-read the materialized bytes and validate the ENTIRE redo stream before
      // accepting. The same validation is repeated after an interrupted commit.
      const auto batch = validate_accepted(state, file, operation._header, operation._prefix);
      publish(state, JournalControl::Committed, JournalCheckpoint::Committed);
      apply_accepted(state, file, operation._header, operation._prefix, batch);
    }
  } // namespace

  MutationJournal::MutationJournal(std::unique_ptr<MutationJournalState> state) noexcept
      : _state(std::move(state))
  {
  }

  MutationJournal::~MutationJournal() = default;
  MutationJournal::MutationJournal(MutationJournal&&) noexcept = default;
  MutationJournal& MutationJournal::operator=(MutationJournal&&) noexcept = default;

  Result<MutationJournal> MutationJournal::open(std::shared_ptr<SessionController> controller,
                                               const MutationGuard& guard, JournalFaultInjector* injector)
  {
    ANGARIA_DIAGNOSTIC_SCOPE(JournalOpen);
    try
    {
      if (!controller)
      {
        fail(ErrorCode::InvalidArgument, "Journal requires a controller");
      }
      auto* file = value_or_throw<JournalFailure>(controller->file(guard));
      auto state = std::make_unique<MutationJournalState>();
      state->_prefix = value_or_throw<JournalFailure>(file->map_read_write(0, superblock_bytes));
      state->_controller = std::move(controller);
      state->_injector = injector;
      return MutationJournal(std::move(state));
    }
    catch (...)
    {
      return caught_error();
    }
  }

  OperationResult MutationJournal::recover(const MutationGuard& guard)
  {
    try
    {
      if (!_state)
      {
        fail(ErrorCode::InvalidState, "Journal is moved-from");
      }
      recover_journal(*_state, *value_or_throw<JournalFailure>(_state->_controller->file(guard)));
      return std::monostate{};
    }
    catch (...)
    {
      return caught_error();
    }
  }

  Result<JournalSnapshot> MutationJournal::snapshot(const MutationGuard& guard)
  {
    std::vector<JournalRecordRange> records;
    return snapshot(guard, records);
  }

  Result<JournalSnapshot> MutationJournal::snapshot(const MutationGuard& guard,
                                                    std::vector<JournalRecordRange>& records)
  {
    records.clear();
    try
    {
      if (!_state)
      {
        fail(ErrorCode::InvalidState, "Journal is moved-from");
      }
      auto& file = *value_or_throw<JournalFailure>(_state->_controller->file(guard));
      recover_journal(*_state, file);
      PrefixRecords prefix;
      const auto snapshot = clean_snapshot(*_state, file, &prefix);
      records = std::move(prefix._ranges);
      return snapshot;
    }
    catch (...)
    {
      return caught_error();
    }
  }

  Result<TransactionResult> MutationJournal::execute(const MutationGuard& guard, const TransactionPlan& plan)
  {
    return execute_impl(guard, plan, nullptr, {});
  }

  Result<TransactionResult> MutationJournal::execute_validated(const MutationGuard& guard,
                                                               const TransactionPlan& plan,
                                                               const JournalSnapshot& validated,
                                                               std::span<const JournalRecordRange> records)
  {
    return execute_impl(guard, plan, &validated, records);
  }

  Result<TransactionResult> MutationJournal::execute_impl(const MutationGuard& guard,
                                                          const TransactionPlan& plan,
                                                          const JournalSnapshot* validated,
                                                          std::span<const JournalRecordRange> records)
  {
    try
    {
      if (!_state)
      {
        fail(ErrorCode::InvalidState, "Journal is moved-from");
      }
      auto& file = *value_or_throw<JournalFailure>(_state->_controller->file(guard));
      const bool recovered = recover_journal(*_state, file);
      auto operation = preflight(*_state, file, plan, recovered ? nullptr : validated, records);
      try
      {
        execute_prepared(*_state, file, operation);
        return std::move(operation._result);
      }
      catch (...)
      {
        const auto error = caught_error();
        try
        {
          // The control store may have completed before an I/O fault became
          // visible to this frame. Persisted authority, not a local flag, wins.
          const auto observed = control(*_state);
          const bool accepted = observed == JournalControl::Committed || observed == JournalControl::Applied ||
                                prefix_word(*_state, 120) == operation._header._generation;
          recover_journal(*_state, file);
          const auto settled = clean_snapshot(*_state, file);
          if (accepted && settled._generation == operation._header._generation)
          {
            return std::move(operation._result);
          }
          if (!accepted && settled == plan._base)
          {
            return error;
          }
        }
        catch (...)
        {
          // Preserve journal evidence when a failed repair cannot prove either
          // rollback or complete acceptance. The next gate owner can retry.
        }
        return Error{ErrorCode::OutcomeUnknown, "Interrupted journal operation could not be settled", "journal"};
      }
    }
    catch (...)
    {
      return caught_error();
    }
  }
} // namespace angaria::detail::mapped
