#include "task_claim_redo.h"

#include "native_atomic.h"
#include "session_metadata.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
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

    bool any_words(NativeFile& file, const SectionLayout& section)
    {
      for (std::uint64_t row = 0; row != section._count; ++row)
      {
        const auto words = read_words(file, section._offset + row * section._stride, section._stride / 8);
        if (std::any_of(words.begin(), words.end(), [](auto word)
            {
              return word != 0;
            }))
        {
          return true;
        }
      }
      return false;
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

  Result<std::vector<RedoCommand>> expand_task_claim_commands(
      NativeFile& file, const FixedLayout& layout,
      std::span<const JournalRecordRange> records,
      std::span<const RedoCommand> commands, ErrorCode errorCode)
  {
    try
    {
      const auto unsupported = errorCode == ErrorCode::InvalidArgument ? ErrorCode::UnsupportedPolicy : errorCode;
      bool hasRelease{};
      for (const auto& command : commands)
      {
        if (command._opcode == RedoOpcode::ReleaseClaim)
        {
          hasRelease = true;
          if (command._payload.size() != 16 || read_word(command._payload, 0) == 0)
          {
            fail(errorCode, "ReleaseClaim requires a nonzero ending incarnation and release timestamp");
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
        for (const auto kind : {SectionKind::QueueIndex, SectionKind::SourceIndex,
                                SectionKind::PublishingIndex, SectionKind::SourceState})
        {
          const auto& delivery = section(layout, kind, errorCode);
          if (command._opcode == RedoOpcode::WriteRange && delivery._byteLength != 0 &&
              command._target < delivery._offset + delivery._byteLength && *end > delivery._offset)
          {
            fail(unsupported, "ReleaseClaim cannot introduce delivery state before its cleanup is implemented");
          }
        }
      }
      for (const auto kind : {SectionKind::QueueIndex, SectionKind::SourceIndex,
                              SectionKind::PublishingIndex, SectionKind::SourceState})
      {
        if (any_words(file, section(layout, kind, errorCode)))
        {
          fail(unsupported, "ReleaseClaim delivery cleanup is not implemented for queue or Source state");
        }
      }
      if (std::any_of(records.begin(), records.end(), [](const auto& record)
          {
            return record._envelope._kind == 9;
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
