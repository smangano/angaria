#include "task_claim_redo.h"

#include "native_atomic.h"
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

    template <typename T>
    T take(Result<T> result)
    {
      if (auto* error = std::get_if<Error>(&result))
      {
        throw Failure{std::move(*error)};
      }
      return std::get<T>(std::move(result));
    }

    std::uint64_t read_word(std::span<const std::byte> bytes, std::size_t offset)
    {
      std::uint64_t value{};
      for (std::size_t index = 0; index != 8; ++index)
      {
        value |= std::uint64_t{std::to_integer<unsigned char>(bytes[offset + index])} << (8 * index);
      }
      return value;
    }

    RedoCommand write_word(std::uint64_t offset, std::uint64_t value)
    {
      RedoCommand result{RedoOpcode::WriteRange, offset, std::vector<std::byte>(8)};
      for (std::size_t index = 0; index != 8; ++index)
      {
        result._payload[index] = static_cast<std::byte>((value >> (8 * index)) & 0xff);
      }
      return result;
    }

    std::vector<std::uint64_t> read_words(NativeFile& file, std::uint64_t offset, std::size_t count)
    {
      if (count == 0)
      {
        return {};
      }
      auto mapping = take(file.map_read_write(offset, count * 8));
      auto bytes = take(mapping.writable_bytes());
      std::vector<std::uint64_t> result;
      result.reserve(count);
      for (std::size_t index = 0; index != count; ++index)
      {
        result.push_back(take(AtomicWord::bind(bytes, index * 8)).load_acquire());
      }
      return result;
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

    bool any_words(std::span<const std::uint64_t> words)
    {
      return std::any_of(words.begin(), words.end(), [](auto word)
      {
        return word != 0;
      });
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
            if (any_words(entry) || any_words(words))
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
                any_words(std::span{entry}.subspan(4)))
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
              take(file.read_exact(entry[1] + record_envelope_bytes, body));
              const auto definition = take(decode_queue_definition(body, entry[1]));
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
              if (any_words(words.first(2)) || any_words(words.subspan(4)) ||
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
                if (any_words(read_words(file, offset, count)))
                {
                  fail(unsupported, "Active Queue ring contains delivery state");
                }
                offset += count * 8;
              }
            }
            else if (kind == SectionKind::PublishingIndex)
            {
              if (words[0] == 0 || words[0] > entry[2] || words[1] != entry[1] ||
                  any_words(std::span{words}.subspan(2)))
              {
                fail(unsupported, "Publishing state requires delivery cleanup");
              }
            }
            else if (any_words(words))
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
                if (any_words(row))
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
          if (std::any_of(entry.begin(), entry.end(), [](auto word)
              {
                return word != 0;
              }) ||
              std::any_of(words.begin(), words.end(), [](auto word)
              {
                return word != 0;
              }))
          {
            fail(code, "Empty Task slot contains state or index metadata");
          }
          continue;
        }
        if (entry[0] > max_entity_id || !ids.insert(entry[0]).second || entry[1] == 0 ||
            entry[2] == 0 || std::any_of(entry.begin() + 3, entry.end(), [](auto word)
            {
              return word != 0;
            }))
        {
          fail(code, "Task index contains an invalid identity, duplicate, revision, or reserved word");
        }
        const auto record = std::find_if(records.begin(), records.end(), [&](const auto& value)
        {
          return value._offset == entry[1];
        });
        if (record == records.end() || record->_envelope._kind != 2 ||
            record->_envelope._readiness != 2 || record->_envelope._registryRevision != entry[2] ||
            record->_envelope._immutableBytes > std::numeric_limits<std::size_t>::max())
        {
          fail(code, "Task index does not reference its committed definition boundary");
        }
        std::vector<std::byte> body(static_cast<std::size_t>(record->_envelope._immutableBytes));
        take(file.read_exact(record->_offset + record_envelope_bytes, body));
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
                 read_word(command._payload, 0) != std::numeric_limits<std::uint64_t>::max() &&
                 words[0] == read_word(command._payload, 0) + 1;
        });
        // A successor attachment writes incarnation before resetting issuance
        // and the retirement floor. Recovery must finish that absolute write
        // even if it stopped between the latter two words.
        if ((words[6] & ~std::uint64_t{7}) != 0 || (words[8] > words[1] && !replacing) ||
            std::any_of(words.begin() + 10, words.end(), [](auto word)
            {
              return word != 0;
            }))
        {
          fail(code, "Task state has unknown flags, an invalid retirement floor, or reserved data");
        }
        if (words[0] == 0)
        {
          if (std::any_of(words.begin(), words.end(), [](auto word)
              {
                return word != 0;
              }))
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
      std::span<const RedoCommand> commands, ErrorCode errorCode)
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
          if (command._payload.size() != 16 || read_word(command._payload, 0) == 0)
          {
            fail(errorCode, "ReleaseClaim requires a nonzero ending incarnation and release timestamp");
          }
          // Replay recognizes only the ending incarnation or its immediate
          // successor. Chaining another replacement of the same row could leave
          // that first intent two incarnations behind after a partial apply.
          if (!releaseTargets.emplace(command._target, read_word(command._payload, 0)).second)
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
          take(validate_redo_targets(std::span{&command, 1}, layout, records, errorCode));
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
              const auto incarnation = read_word(command._payload,
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
            fail(unsupported, "ReleaseClaim cannot introduce delivery state before its cleanup is implemented");
          }
        }
        for (const auto& record : records)
        {
          if (command._opcode == RedoOpcode::WriteRange && record._envelope._kind == 8 &&
              command._target < record._offset + record._envelope._totalBytes && *end > record._offset)
          {
            fail(unsupported, "Task-only ReleaseClaim cannot change ring slots");
          }
        }
      }
      check_idle_topology(file, layout, records, errorCode);
      if (std::any_of(records.begin(), records.end(), [](const auto& record)
          {
            return record._envelope._kind == 9 || record._envelope._kind == 10;
          }))
      {
        fail(unsupported, "ReleaseClaim receipt cleanup is not implemented");
      }
      auto tasks = read_tasks(file, layout, records, commands, errorCode);
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
                task._words[word] = read_word(command._payload, static_cast<std::size_t>(offset - command._target));
              }
            }
          }
          continue;
        }
        const auto ending = read_word(command._payload, 0);
        const auto timestamp = read_word(command._payload, 8);
        const auto target = std::find_if(tasks.begin(), tasks.end(), [&](const auto& task)
        {
          return task._offset == command._target;
        });
        if (target == tasks.end() || (target->_words[0] != ending &&
            (ending == std::numeric_limits<std::uint64_t>::max() || target->_words[0] != ending + 1)))
        {
          fail(errorCode, "ReleaseClaim target is not the ending or immediately replacing Task incarnation");
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
