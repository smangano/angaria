#include "task_claim_redo.h"

#include "angaria/detail/byte_operations.h"
#include "angaria/detail/checked_arithmetic.h"
#include "angaria/detail/error_support.h"
#include "delivery_words.h"
#include "journal_atomic.h"
#include "journal_metadata.h"
#include "native_atomic.h"
#include "publication_metadata.h"
#include "session_metadata.h"
#include "topology_metadata.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace angaria::detail::mapped
{
  namespace
  {
    struct Failure
    {
      Error _error;
    };

    [[noreturn]] void fail(ErrorCode code, std::string message)
    {
      throw Failure{Error{code, std::move(message), "journal.claims"}};
    }

    RedoCommand write_word(std::uint64_t offset, std::uint64_t value)
    {
      RedoCommand result{RedoOpcode::WriteRange, offset, std::vector<std::byte>(8)};
      write_unsigned_le(result._payload, 0, value);
      return result;
    }

    std::vector<std::uint64_t> read_words(NativeFile& file, std::uint64_t offset, std::size_t count)
    {
      return value_or_throw<Failure>(read_journal_words(file, offset, count));
    }

    const SectionLayout& section(const FixedLayout& layout, SectionKind kind, ErrorCode code)
    {
      const auto found = std::find_if(layout._sections.begin(), layout._sections.end(), [kind](const auto& value)
      {
        return value._kind == kind;
      });
      if (found == layout._sections.end())
      {
        fail(code, "Required Task cleanup section is absent");
      }
      return *found;
    }

    void check_idle_topology(NativeFile& file, const FixedLayout& layout,
                              std::span<const JournalRecordRange> records, ErrorCode code)
    {
      const auto unsupported = code == ErrorCode::IoError ? code : ErrorCode::UnsupportedPolicy;
      std::map<std::uint64_t, const JournalRecordRange*> rings;
      std::map<std::uint64_t, const JournalRecordRange*> queueDefinitions;
      for (const auto& record : records)
      {
        if (record._envelope._kind == 8 && !rings.emplace(record._offset, &record).second)
        {
          fail(code, "Duplicate ring record boundary");
        }
        if (record._envelope._kind == 3)
        {
          queueDefinitions.emplace(record._offset, &record);
        }
      }
      std::set<std::uint64_t> ownedRings;
      const auto& membership = section(layout, SectionKind::MembershipState, code);
      const auto membershipWords = read_words(file, membership._offset, membership._byteLength / 8);
      const auto sets = section(layout, SectionKind::SourceIndex, code)._count +
                        section(layout, SectionKind::PublishingIndex, code)._count;
      const auto perSet = sets == 0 ? 0 : membership._count / sets;
      std::uint64_t group{};
      for (const auto kind : {SectionKind::QueueIndex, SectionKind::SourceIndex, SectionKind::PublishingIndex})
      {
        const auto& index = section(layout, kind, code);
        const auto stateKind = kind == SectionKind::QueueIndex ? SectionKind::QueueState :
            kind == SectionKind::SourceIndex ? SectionKind::SourceState : SectionKind::PublishingState;
        const auto& state = section(layout, stateKind, code);
        const auto indexes = read_words(file, index._offset, index._byteLength / 8);
        const auto states = read_words(file, state._offset, state._byteLength / 8);
        for (std::uint64_t slot = 0; slot != index._count; ++slot)
        {
          const auto entry = std::span{indexes}.subspan(slot * index._stride / 8, index._stride / 8);
          const auto words = std::span{states}.subspan(slot * state._stride / 8, state._stride / 8);
          if (entry[0] == 0)
          {
            if (!all_zero(entry) || !all_zero(words))
            {
              fail(code, "Empty topology slot contains index or runtime state");
            }
          }
          else
          {
            if (entry[3] != 1 && entry[3] != 2)
            {
              fail(unsupported, "Task-only cleanup requires known topology state");
            }
            if (entry[0] > 65535 || entry[1] == 0 || entry[2] == 0 ||
                !all_zero(std::span{entry}.subspan(4)))
            {
              fail(code, "Topology index is malformed");
            }
            std::optional<std::uint64_t> queueCapacity;
            if (kind == SectionKind::QueueIndex)
            {
              const auto record = queueDefinitions.find(entry[1]);
              if (record == queueDefinitions.end())
              {
                fail(code, "Queue index does not reference its committed definition");
              }
              std::vector<std::byte> body(static_cast<std::size_t>(record->second->_envelope._immutableBytes));
              value_or_throw<Failure>(file.read_exact(entry[1] + record_envelope_bytes, body));
              const auto definition = value_or_throw<Failure>(decode_queue_definition(body, entry[1]));
              if (static_cast<std::uint16_t>(definition._id) != entry[0] ||
                  definition._registration._revision != entry[2] ||
                  (definition._state == EntityState::Draft ? 1u : 2u) != entry[3])
              {
                fail(code, "Queue definition and index disagree during Task cleanup");
              }
              queueCapacity = definition._descriptor._ringCapacity;
            }
            if (kind == SectionKind::QueueIndex && entry[3] == 2)
            {
              const auto ring = rings.find(words[2]);
              const auto required = ring_storage_bytes(words[3]);
              if (!all_zero(words.first(2)) || !all_zero(words.subspan(4)) ||
                  ring == rings.end() || !required || queueCapacity != words[3] || !ownedRings.insert(words[2]).second ||
                  ring->second->_envelope._immutableBytes != 0 ||
                  ring->second->_envelope._encoding != 1 || ring->second->_envelope._previous != 0 ||
                  ring->second->_envelope._totalBytes != *required)
              {
                fail(unsupported, "Active Queue contains invalid ring geometry or delivery progress");
              }
              // Check mutable slots atomically in bounded chunks, including
              // allocation padding, without allocating a copy of the ring.
              for (auto offset = words[2] + record_envelope_bytes; offset < words[2] + *required;)
              {
                const auto count = std::min<std::uint64_t>(512, (words[2] + *required - offset) / 8);
                if (!all_zero(read_words(file, offset, count)))
                {
                  fail(unsupported, "Active Queue ring contains delivery state");
                }
                offset += count * 8;
              }
            }
            else if (kind == SectionKind::PublishingIndex)
            {
              if (words[0] == 0 || words[0] > entry[2] || words[1] != entry[1] ||
                  !all_zero(std::span{words}.subspan(2)))
              {
                fail(unsupported, "Publishing state requires delivery cleanup");
              }
            }
            else if (!all_zero(words))
            {
              fail(unsupported, "Queue or Source state requires delivery cleanup");
            }
          }
          if (kind != SectionKind::QueueIndex)
          {
            for (std::uint64_t member = 0; member != perSet; ++member)
            {
              const auto row = std::span{membershipWords}.subspan((group * perSet + member) * 4, 4);
              if (row[0] == 0)
              {
                if (!all_zero(row))
                {
                  fail(code, "Empty membership contains state");
                }
              }
              else if (entry[0] == 0 || row[0] > 0xffffff || (row[0] & 0xffff) == 0 || row[1] == 0 ||
                       row[2] != 0 || row[3] != 0)
              {
                fail(unsupported, "Membership contains scheduling or delivery state");
              }
            }
            ++group;
          }
        }
      }
      if (ownedRings.size() != rings.size())
      {
        fail(unsupported, "A retained ring has no Active Queue owner");
      }
    }

    struct TaskRow
    {
      TaskDefinition _definition;
      std::uint64_t _offset{};
      std::vector<std::uint64_t> _words;
    };

    std::vector<TaskRow> read_tasks(NativeFile& file, const FixedLayout& layout,
                                    std::span<const JournalRecordRange> records,
                                    std::span<const RedoCommand> commands, ErrorCode code)
    {
      const auto& index = section(layout, SectionKind::TaskIndex, code);
      const auto& state = section(layout, SectionKind::TaskState, code);
      if (index._stride != index_entry_bytes || state._stride != task_state_bytes ||
          index._count != state._count)
      {
        fail(code, "Task cleanup requires parallel Task index and state tables");
      }
      std::vector<TaskRow> tasks;
      std::set<std::uint64_t> ids;
      for (std::uint64_t slot = 0; slot != index._count; ++slot)
      {
        const auto entry = read_words(file, index._offset + slot * index._stride, 8);
        auto words = read_words(file, state._offset + slot * state._stride, 32);
        if (entry[0] == 0)
        {
          if (!all_zero(entry) ||
              !all_zero(words))
          {
            fail(code, "Empty Task slot contains state or index metadata");
          }
          continue;
        }
        if (entry[0] > max_entity_id || !ids.insert(entry[0]).second || entry[1] == 0 ||
            entry[2] == 0 || !all_zero(std::span{entry}.subspan(3)))
        {
          fail(code, "Task index contains an invalid identity, duplicate, revision, or reserved word");
        }
        const auto* record = find_journal_record(records, entry[1]);
        if (!record || record->_envelope._kind != 2 ||
            record->_envelope._readiness != 2 || record->_envelope._registryRevision != entry[2] ||
            !fits_size_t(record->_envelope._immutableBytes))
        {
          fail(code, "Task index does not reference its committed definition boundary");
        }
        std::vector<std::byte> body(static_cast<std::size_t>(record->_envelope._immutableBytes));
        value_or_throw<Failure>(file.read_exact(record->_offset + record_envelope_bytes, body));
        auto decoded = decode_task_definition(body, record->_offset);
        if (std::holds_alternative<Error>(decoded))
        {
          fail(code, "Task definition is invalid during claim cleanup");
        }
        auto definition = std::get<TaskDefinition>(std::move(decoded));
        if (static_cast<std::uint64_t>(definition._id) != entry[0] ||
            definition._registration._revision != entry[2])
        {
          fail(code, "Task definition identity or revision disagrees with its index");
        }
        const auto rowOffset = state._offset + slot * state._stride;
        const bool replacing = std::any_of(commands.begin(), commands.end(), [&](const auto& command)
        {
          return command._opcode == RedoOpcode::ReleaseClaim && command._target == rowOffset &&
                 read_unsigned_le<std::uint64_t>(command._payload, 0) != std::numeric_limits<std::uint64_t>::max() &&
                 words[0] == read_unsigned_le<std::uint64_t>(command._payload, 0) + 1;
        });
        // A successor attachment writes incarnation before resetting issuance
        // and the retirement floor. Recovery must finish that absolute write
        // even if it stopped between the latter two words.
        if ((words[6] & ~std::uint64_t{7}) != 0 || (words[8] > words[1] && !replacing) ||
            !all_zero(std::span{words}.subspan(10)))
        {
          fail(code, "Task state has unknown flags, an invalid retirement floor, or reserved data");
        }
        if (words[0] == 0)
        {
          if (!all_zero(words))
          {
            fail(code, "Never-attached Task contains claim state");
          }
        }
        else if (words[3] == 0 || words[4] == 0 || (words[6] & 2) == 0 ||
                 (definition._descriptor._kind == TaskKind::Process ? words[7] != 0 : words[7] == 0))
        {
          fail(code, "Task claim diagnostics or parent incarnation are invalid");
        }
        tasks.push_back(TaskRow{std::move(definition), rowOffset,
                                std::move(words)});
      }
      for (const auto& task : tasks)
      {
        if (task._definition._descriptor._kind != TaskKind::Thread)
        {
          continue;
        }
        const auto parent = std::find_if(tasks.begin(), tasks.end(), [&](const auto& candidate)
        {
          return candidate._definition._id == *task._definition._descriptor._process;
        });
        if (parent == tasks.end() || parent->_definition._descriptor._kind != TaskKind::Process)
        {
          fail(code, "Thread Task parent does not identify a registered Process Task");
        }
      }
      return tasks;
    }

    struct SourceRow
    {
      SourceDefinition _definition;
      std::uint64_t _offset{};
      std::vector<std::uint64_t> _words;
    };

    std::vector<SourceRow> read_sources(NativeFile& file, const FixedLayout& layout,
                                       std::span<const JournalRecordRange> records,
                                       std::span<const TaskRow> tasks, ErrorCode code)
    {
      const auto& index = section(layout, SectionKind::SourceIndex, code);
      const auto& state = section(layout, SectionKind::SourceState, code);
      if (index._stride != index_entry_bytes || state._stride != source_state_bytes || index._count != state._count)
      {
        fail(code, "Source cleanup requires parallel Source index and state tables");
      }
      std::vector<SourceRow> sources;
      std::set<std::uint64_t> ids;
      for (std::uint64_t slot = 0; slot < index._count; ++slot)
      {
        const auto entry = read_words(file, index._offset + slot * index._stride, 8);
        auto words = read_words(file, state._offset + slot * state._stride, 32);
        if (entry[0] == 0)
        {
          if (!all_zero(entry) || !all_zero(words))
          {
            fail(code, "Empty Source cleanup slot contains state");
          }
          continue;
        }
        const auto* record = find_journal_record(records, entry[1]);
        if (entry[0] > max_entity_id || !ids.insert(entry[0]).second || entry[2] == 0 ||
            (entry[3] != 1 && entry[3] != 2) || !all_zero(std::span{entry}.subspan(4)) ||
            !record || record->_envelope._kind != 4 || record->_envelope._readiness != 2 ||
            record->_envelope._registryRevision != entry[2] || !fits_host_size(record->_envelope._immutableBytes))
        {
          fail(code, "Source cleanup index does not name its committed definition");
        }
        std::vector<std::byte> body(static_cast<std::size_t>(record->_envelope._immutableBytes));
        value_or_throw<Failure>(file.read_exact(record->_offset + record_envelope_bytes, body));
        auto definition = value_or_throw<Failure>(decode_source_definition(body, record->_offset));
        if (static_cast<std::uint64_t>(definition._id) != entry[0] || definition._registration._revision != entry[2] ||
            (definition._state == EntityState::Draft ? 1u : 2u) != entry[3] || !all_zero(std::span{words}.subspan(8)) ||
            (words[1] == 0 && words[0] != 0))
        {
          fail(code, "Source cleanup definition or reserved state is inconsistent");
        }
        const auto owner = std::find_if(tasks.begin(), tasks.end(), [&](const auto& task)
        {
          return task._definition._id == definition._descriptor._owner;
        });
        if ((definition._descriptor._owner != TaskId::Invalid && owner == tasks.end()) ||
            (owner != tasks.end() && words[1] > owner->_words[0]) ||
            (owner == tasks.end() && !all_zero(words)))
        {
          fail(code, "Source cleanup owner or issuance context is invalid");
        }
        sources.push_back(SourceRow{std::move(definition), state._offset + slot * state._stride, std::move(words)});
      }
      return sources;
    }

    struct LossQueueContext
    {
      std::uint64_t _terminal{};
      std::uint64_t _ringFloor{};
      SourceQueueSetId _source{};
      TaskId _producer{};
    };

    LossQueueContext read_loss_queue(NativeFile& file, const FixedLayout& layout,
                            std::span<const JournalRecordRange> records,
                            const PublicationMetadata& publication,
                            std::span<const SourceRow> sources, ErrorCode code)
    {
      const auto& destination = publication._destinations.front();
      const auto& index = section(layout, SectionKind::QueueIndex, code);
      const auto& state = section(layout, SectionKind::QueueState, code);
      if (index._stride != index_entry_bytes || state._stride != queue_state_bytes || state._count != index._count)
      {
        fail(code, "Lost cleanup requires parallel Queue index and state tables");
      }
      bool found{};
      LossQueueContext result;
      for (std::uint64_t slot = 0; slot != index._count; ++slot)
      {
        const auto entry = read_words(file, index._offset + slot * index._stride, 8);
        if (entry[0] != static_cast<std::uint16_t>(destination._queue))
        {
          continue;
        }
        if (found || entry[1] == 0 || entry[2] == 0 || entry[3] != 2 || !all_zero(std::span{entry}.subspan(4)))
        {
          fail(code, "Lost cleanup Queue index is not a unique Active definition");
        }
        found = true;
        const auto* record = find_journal_record(records, entry[1]);
        if (!record || record->_envelope._kind != 3 || record->_envelope._readiness != 2 ||
            record->_envelope._registryRevision != entry[2] || !fits_host_size(record->_envelope._immutableBytes))
        {
          fail(code, "Lost cleanup Queue index does not name its exact committed definition");
        }
        std::vector<std::byte> body(static_cast<std::size_t>(record->_envelope._immutableBytes));
        value_or_throw<Failure>(file.read_exact(record->_offset + record_envelope_bytes, body));
        const auto queue = value_or_throw<Failure>(decode_queue_definition(body, record->_offset));
        const auto words = read_words(file, state._offset + slot * state._stride, 32);
        const auto capacity = words[3];
        const auto source = std::find_if(sources.begin(), sources.end(), [&](const auto& candidate)
        {
          return candidate._definition._id == queue._descriptor._sourceSet;
        });
        if (queue._id != destination._queue || queue._registration._revision != entry[2] ||
            queue._state != EntityState::Active || queue._descriptor._delivery != DeliveryPolicy::AllowLoss ||
            queue._descriptor._producer != publication._producer || queue._descriptor._ringCapacity != capacity ||
            capacity == 0 || words[0] <= capacity || words[8] > words[0] ||
            source == sources.end() || source->_definition._state != EntityState::Active ||
            std::find(source->_definition._members.begin(), source->_definition._members.end(), queue._id) ==
                source->_definition._members.end())
        {
          fail(code, "Lost cleanup receipt is outside its allowed-loss Queue's overwritten terminal prefix");
        }
        result = {words[8], words[0] - capacity + 1, queue._descriptor._sourceSet, queue._descriptor._producer};
        const auto* ring = find_journal_record(records, words[2]);
        const auto ringBytes = ring_storage_bytes(capacity);
        if (!ringBytes || !ring || ring->_envelope._kind != 8 ||
            ring->_envelope._readiness != 2 || ring->_envelope._immutableBytes != 0 ||
            ring->_envelope._totalBytes != *ringBytes)
        {
          fail(code, "Lost cleanup Queue does not name its exact committed ring");
        }
        for (std::uint64_t index = 0; index != capacity; ++index)
        {
          const auto slot = read_words(file, words[2] + record_envelope_bytes + index * ring_slot_bytes, 4);
          const auto lastIndex = (words[0] - 1) % capacity;
          const auto distance = lastIndex >= index ? lastIndex - index : capacity - (index - lastIndex);
          const auto expected = words[0] - distance;
          const auto* publicationRecord = find_journal_record(records, slot[2]);
          if (slot[0] != expected || slot[3] != 1 + (expected - 1) / capacity ||
              !publicationRecord || publicationRecord->_envelope._kind != 9)
          {
            fail(code, "Lost cleanup Queue ring is outside its authoritative current window");
          }
          const auto retained = value_or_throw<Failure>(read_journal_publication(file, *publicationRecord, code));
          if (retained._destinations.front()._queue != queue._id ||
              retained._destinations.front()._queueSequence != expected ||
              retained._layout._destinationsOffset != slot[1] ||
              retained._producer != queue._descriptor._producer)
          {
            fail(code, "Lost cleanup Queue ring does not resolve its exact retained destination");
          }
        }
      }
      if (!found)
      {
        fail(code, "Lost cleanup receipt has no registered Queue");
      }
      return result;
    }

    void append_delivery_cleanup(NativeFile& file, const FixedLayout& layout,
                                   std::span<const JournalRecordRange> records,
                                   std::span<const TaskRow> tasks,
                                   const std::map<TaskId, std::uint64_t>& endingClaims,
                                   std::vector<RedoCommand>& expanded, ErrorCode code, bool allowLoss)
    {
      const auto sources = read_sources(file, layout, records, tasks, code);
      std::set<SourceQueueSetId> issuedSources;
      std::map<QueueId, LossQueueContext> lossQueues;
      for (const auto& record : records)
      {
        if (record._envelope._kind == 10)
        {
          fail(code, "Retry receipt cleanup requires a later capability");
        }
        if (record._envelope._kind != 9)
        {
          continue;
        }
        const auto metadata = value_or_throw<Failure>(read_journal_publication(file, record, code));
        const auto receiptOffset = metadata._layout._receiptsOffset;
        const auto receipt = value_or_throw<Failure>(decode_receipt_words(read_words(file, receiptOffset, 16),
            allowLoss ? ReceiptProfile::AllowLoss : ReceiptProfile::Core));
        if (receipt._state == ReceiptState::Unused)
        {
          fail(code, "Accepted direct destination has no receipt obligation");
        }
        if (receipt._state == ReceiptState::Lost)
        {
          const auto& destination = metadata._destinations.front();
          auto context = lossQueues.find(destination._queue);
          if (context == lossQueues.end())
          {
            context = lossQueues.emplace(destination._queue,
                read_loss_queue(file, layout, records, metadata, sources, code)).first;
          }
          if (destination._queueSequence > context->second._terminal ||
              destination._queueSequence >= context->second._ringFloor ||
              metadata._producer != context->second._producer ||
              ((receipt._presence & 1) != 0 && receipt._sourceSet != context->second._source))
          {
            fail(code, "Lost cleanup receipt is outside its allowed-loss Queue's overwritten terminal prefix");
          }
        }
        if ((receipt._presence & 1) == 0)
        {
          continue;
        }
        const auto source = std::find_if(sources.begin(), sources.end(), [&](const auto& candidate)
        {
          return candidate._definition._id == receipt._sourceSet;
        });
        const auto task = std::find_if(tasks.begin(), tasks.end(), [&](const auto& candidate)
        {
          return candidate._definition._id == receipt._consumerTask;
        });
        if (source == sources.end() || source->_definition._descriptor._owner != receipt._consumerTask ||
            task == tasks.end() || receipt._consumerIncarnation > task->_words[0] ||
            receipt._consumerIncarnation > source->_words[1] ||
            (receipt._consumerIncarnation == source->_words[1] && receipt._issuance > source->_words[0]))
        {
          fail(code, "Receipt cleanup tags disagree with retained ownership or issuance context");
        }
        const auto ending = endingClaims.find(receipt._consumerTask);
        if (receipt._state == ReceiptState::Issued)
        {
          if (!issuedSources.insert(receipt._sourceSet).second)
          {
            fail(code, "Source has more than one Issued receipt during cleanup");
          }
          if (ending != endingClaims.end() && receipt._consumerIncarnation == ending->second)
          {
            // State is the only changed receipt word. Preserved tags permit a
            // later replay to rediscover this already Pending historical row.
            expanded.push_back(write_word(receiptOffset, 1));
          }
        }
      }
      for (const auto& source : sources)
      {
        const auto ending = endingClaims.find(source._definition._descriptor._owner);
        if (ending == endingClaims.end())
        {
          continue;
        }
        if (source._words[1] > ending->second ||
            (source._words[1] < ending->second && !all_zero(std::span{source._words}.subspan(2, 6))))
        {
          fail(code, "Claim cleanup cannot clear a different Source incarnation");
        }
        if (source._words[1] == ending->second)
        {
          // Ownership is immutable; clearing must not depend on a slot pointer
          // that an earlier partial pass may already have erased. Counter and
          // incarnation remain, fencing retained receipt history on reopen.
          for (std::uint64_t word = 2; word != 8; ++word)
          {
            expanded.push_back(write_word(source._offset + word * 8, 0));
          }
        }
      }
    }
  } // namespace

  OperationResult validate_task_only_topology(NativeFile& file, const FixedLayout& layout,
                                               std::span<const JournalRecordRange> records, ErrorCode errorCode)
  {
    try
    {
      check_idle_topology(file, layout, records, errorCode);
      return std::monostate{};
    }
    catch (const Failure& failure)
    {
      return failure._error;
    }
    catch (...)
    {
      return Error{ErrorCode::IoError, "Unable to inspect draft Task cleanup state", "journal.claims"};
    }
  }

  Result<std::vector<RedoCommand>> expand_task_claim_commands(
      NativeFile& file, const FixedLayout& layout,
      std::span<const JournalRecordRange> records,
      std::span<const RedoCommand> commands, ErrorCode errorCode, bool nativeDelivery, bool allowLoss)
  {
    try
    {
      const auto unsupported = errorCode == ErrorCode::InvalidArgument ? ErrorCode::UnsupportedPolicy : errorCode;
      bool hasRelease{};
      std::map<std::uint64_t, std::uint64_t> releaseTargets;
      for (const auto& command : commands)
      {
        if (command._opcode == RedoOpcode::ReleaseClaim)
        {
          hasRelease = true;
          if (command._payload.size() != 16 || read_unsigned_le<std::uint64_t>(command._payload, 0) == 0)
          {
            fail(errorCode, "ReleaseClaim requires a nonzero ending incarnation and release timestamp");
          }
          // Replay recognizes only the ending incarnation or its immediate
          // successor. Chaining another replacement of the same row could leave
          // that first intent two incarnations behind after a partial apply.
          if (!releaseTargets.emplace(command._target, read_unsigned_le<std::uint64_t>(command._payload, 0)).second)
          {
            fail(errorCode, "ReleaseClaim may target each Task only once per transaction");
          }
        }
        else if (command._opcode != RedoOpcode::WriteRange)
        {
          fail(unsupported, "This compact domain redo command is not implemented");
        }
        else
        {
          value_or_throw<Failure>(validate_redo_targets(std::span{&command, 1}, layout, records, errorCode));
        }
      }
      if (!hasRelease)
      {
        return std::vector<RedoCommand>(commands.begin(), commands.end());
      }
      const auto& taskIndex = section(layout, SectionKind::TaskIndex, errorCode);
      for (const auto& command : commands)
      {
        const auto end = checked_add(command._target, command._payload.size());
        if (!end)
        {
          fail(errorCode, "Task cleanup command range overflows");
        }
        if (command._opcode == RedoOpcode::WriteRange &&
            command._target < taskIndex._offset + taskIndex._byteLength &&
            *end > taskIndex._offset)
        {
          fail(errorCode, "ReleaseClaim cannot change the Task index used for replay discovery");
        }
        if (command._opcode == RedoOpcode::WriteRange)
        {
          for (const auto& [target, ending] : releaseTargets)
          {
            if (target >= command._target && target - command._target < command._payload.size())
            {
              const auto incarnation = read_unsigned_le<std::uint64_t>(command._payload,
                                                   static_cast<std::size_t>(target - command._target));
              // Every possible interrupted prefix must remain inside the same
              // replay window, not just the stream's final simulated state.
              if (incarnation != ending && (ending == std::numeric_limits<std::uint64_t>::max() ||
                  incarnation != ending + 1))
              {
                fail(errorCode, "Task incarnation write leaves its ReleaseClaim replay window");
              }
            }
          }
        }
        for (const auto kind : {SectionKind::QueueIndex, SectionKind::SourceIndex,
                                SectionKind::PublishingIndex, SectionKind::QueueState, SectionKind::SourceState,
                                SectionKind::PublishingState, SectionKind::MembershipState})
        {
          const auto& delivery = section(layout, kind, errorCode);
          if (command._opcode == RedoOpcode::WriteRange && delivery._byteLength != 0 &&
              command._target < delivery._offset + delivery._byteLength && *end > delivery._offset)
          {
            fail(nativeDelivery ? errorCode : unsupported,
                 "ReleaseClaim cannot overwrite topology or delivery roots used for replay discovery");
          }
        }
        for (const auto& record : records)
        {
          if (command._opcode == RedoOpcode::WriteRange &&
              (record._envelope._kind == 8 || record._envelope._kind == 9 || record._envelope._kind == 10) &&
              command._target < record._offset + record._envelope._totalBytes && *end > record._offset)
          {
            fail(nativeDelivery ? errorCode : unsupported,
                 "ReleaseClaim cannot overwrite the receipt or ring state used for replay discovery");
          }
        }
      }
      if (!nativeDelivery)
      {
        check_idle_topology(file, layout, records, errorCode);
        if (std::any_of(records.begin(), records.end(), [](const auto& record)
          {
            return record._envelope._kind == 9 || record._envelope._kind == 10;
          }))
        {
          fail(unsupported, "Legacy ReleaseClaim cannot clean native delivery records");
        }
      }
      auto tasks = read_tasks(file, layout, records, commands, errorCode);
      // A parent cleanup must keep each child's captured parent-incarnation tag
      // stable throughout every interrupted prefix, not merely its final image.
      for (const auto& [target, ending] : releaseTargets)
      {
        const auto parent = std::find_if(tasks.begin(), tasks.end(), [&](const auto& task)
        {
          return task._offset == target;
        });
        if (parent == tasks.end() || parent->_definition._descriptor._kind != TaskKind::Process)
        {
          continue;
        }
        for (const auto& child : tasks)
        {
          if (child._definition._descriptor._kind != TaskKind::Thread ||
              child._definition._descriptor._process != parent->_definition._id)
          {
            continue;
          }
          for (const auto& command : commands)
          {
            for (const auto word : {std::size_t{0}, std::size_t{7}})
            {
              const auto identity = child._offset + word * 8;
              if (command._opcode == RedoOpcode::WriteRange && identity >= command._target &&
                  identity - command._target < command._payload.size() &&
                  read_unsigned_le<std::uint64_t>(command._payload,
                      static_cast<std::size_t>(identity - command._target)) != child._words[word])
              {
                fail(errorCode, "ReleaseClaim cannot change child incarnation tags used for parent replay");
              }
            }
          }
        }
        (void)ending;
      }
      std::vector<RedoCommand> expanded;
      for (const auto& command : commands)
      {
        if (command._opcode == RedoOpcode::WriteRange)
        {
          expanded.push_back(command);
          // Compact commands execute in recorded order. Earlier absolute
          // writes therefore participate in later claim cleanup decisions.
          for (auto& task : tasks)
          {
            for (std::size_t word = 0; word != task._words.size(); ++word)
            {
              const auto offset = task._offset + word * 8;
              if (offset >= command._target && offset - command._target < command._payload.size())
              {
                task._words[word] = read_unsigned_le<std::uint64_t>(command._payload, static_cast<std::size_t>(offset - command._target));
              }
            }
          }
          continue;
        }
        const auto ending = read_unsigned_le<std::uint64_t>(command._payload, 0);
        const auto timestamp = read_unsigned_le<std::uint64_t>(command._payload, 8);
        const auto target = std::find_if(tasks.begin(), tasks.end(), [&](const auto& task)
        {
          return task._offset == command._target;
        });
        if (target == tasks.end() || (target->_words[0] != ending &&
            (ending == std::numeric_limits<std::uint64_t>::max() || target->_words[0] != ending + 1)))
        {
          fail(errorCode, "ReleaseClaim target is not the ending or immediately replacing Task incarnation");
        }
        if (nativeDelivery)
        {
          std::map<TaskId, std::uint64_t> endingClaims{{target->_definition._id, ending}};
          if (target->_definition._descriptor._kind == TaskKind::Process)
          {
            for (const auto& child : tasks)
            {
              if (child._definition._descriptor._kind == TaskKind::Thread &&
                  child._definition._descriptor._process == target->_definition._id && child._words[7] == ending)
              {
                endingClaims.emplace(child._definition._id, child._words[0]);
              }
            }
          }
          append_delivery_cleanup(file, layout, records, tasks, endingClaims, expanded, errorCode, allowLoss);
        }
        const auto detach = [&](TaskRow& task)
        {
          if ((task._words[6] & 1) == 0 && (task._words[6] & 4) != 0)
          {
            return;
          }
          // Timestamp first makes a repeated pass safe after the flags word was
          // installed: an already detached row retains its earlier detach time.
          expanded.push_back(write_word(task._offset + 72, timestamp));
          expanded.push_back(write_word(task._offset + 48, (task._words[6] & ~std::uint64_t{1}) | 4));
          task._words[9] = timestamp;
          task._words[6] = (task._words[6] & ~std::uint64_t{1}) | 4;
        };
        if (target->_words[0] == ending)
        {
          detach(*target);
        }
        if (target->_definition._descriptor._kind == TaskKind::Process)
        {
          for (auto& task : tasks)
          {
            if (task._definition._descriptor._kind == TaskKind::Thread &&
                task._definition._descriptor._process == target->_definition._id && task._words[7] == ending)
            {
              detach(task);
            }
          }
        }
      }
      return expanded;
    }
    catch (const Failure& failure)
    {
      return failure._error;
    }
    catch (...)
    {
      return Error{ErrorCode::IoError, "Unable to expand Task claim cleanup", "journal.claims"};
    }
  }
} // namespace angaria::detail::mapped
