#include "loss_redo.h"

#include "angaria/detail/byte_operations.h"
#include "angaria/detail/checked_arithmetic.h"
#include "angaria/detail/error_support.h"
#include "delivery_words.h"
#include "journal_atomic.h"
#include "journal_metadata.h"
#include "publication_metadata.h"
#include "session_metadata.h"
#include "topology_metadata.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>
#include <new>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace angaria::detail::mapped
{
  namespace
  {
    struct LossFailure
    {
      Error _error;
    };

    void require(bool condition, ErrorCode code, const char* message)
    {
      if (!condition)
      {
        throw LossFailure{Error{code, message, "journal.loss"}};
      }
    }

    Error caught_error()
    {
      try
      {
        throw;
      }
      catch (const LossFailure& failure)
      {
        return failure._error;
      }
      catch (const std::bad_alloc&)
      {
        return Error{ErrorCode::CapacityExceeded, "Insufficient memory for loss metadata", "journal.loss"};
      }
      catch (...)
      {
        return Error{ErrorCode::IoError, "Unable to inspect or apply compact loss", "journal.loss"};
      }
    }

    template <typename T>
    T value(Result<T> result)
    {
      return value_or_throw<LossFailure>(std::move(result));
    }

    const SectionLayout& section(const FixedLayout& layout, SectionKind kind, ErrorCode code)
    {
      const auto found = std::find_if(layout._sections.begin(), layout._sections.end(), [kind](const auto& entry)
      {
        return entry._kind == kind;
      });
      require(found != layout._sections.end(), code, "Loss section is absent");
      return *found;
    }

    const JournalRecordRange& record_at(std::span<const JournalRecordRange> records,
                                           std::uint64_t reference, std::uint16_t kind, ErrorCode code)
    {
      const auto* found = find_journal_record(records, reference);
      require(found && found->_envelope._kind == kind && found->_envelope._readiness == 2,
              code, "Loss reference is not an exact committed record of the required kind");
      return *found;
    }

    struct EntityRow
    {
      std::vector<std::uint64_t> _index;
      std::vector<std::uint64_t> _state;
      std::vector<std::byte> _definition;
      std::uint64_t _reference{};
      std::uint64_t _offset{};
    };

    EntityRow entity_row(NativeFile& file, const FixedLayout& layout,
                            std::span<const JournalRecordRange> records, SectionKind indexKind,
                            SectionKind stateKind, std::uint16_t recordKind, std::uint64_t id,
                            ErrorCode code, std::optional<std::uint64_t> target = {})
    {
      const auto& index = section(layout, indexKind, code);
      const auto& state = section(layout, stateKind, code);
      require(index._stride == index_entry_bytes && state._count == index._count && state._stride == 256,
              code, "Loss participant tables have invalid geometry");
      std::optional<EntityRow> found;
      for (std::uint64_t slot = 0; slot != index._count; ++slot)
      {
        const auto offset = state._offset + slot * state._stride;
        if (target && offset != *target)
        {
          continue;
        }
        auto words = value(read_journal_words(file, index._offset + slot * index._stride, 8));
        if (words[0] == 0 || (!target && words[0] != id))
        {
          continue;
        }
        require(!found && words[0] <= max_entity_id && words[1] != 0 && words[2] != 0 &&
                    all_zero(std::span{words}.subspan(indexKind == SectionKind::TaskIndex ? 3 : 4)),
                code, "Loss participant index is malformed or duplicated");
        const auto& record = record_at(records, words[1], recordKind, code);
        require(record._envelope._registryRevision == words[2] && fits_host_size(record._envelope._immutableBytes),
                code, "Loss participant definition does not match its index");
        std::vector<std::byte> body(static_cast<std::size_t>(record._envelope._immutableBytes));
        value(file.read_exact(record._offset + record_envelope_bytes, body));
        found = EntityRow{std::move(words), value(read_journal_words(file, offset, 32)),
                           std::move(body), record._offset, offset};
      }
      require(found.has_value(), code, "Loss participant is not registered");
      return std::move(*found);
    }

    struct LossIntent
    {
      std::uint64_t _first{};
      std::uint64_t _last{};
      std::uint64_t _time{};
      SourceQueueSetId _source{};
    };

    LossIntent loss_intent(const RedoCommand& command, ErrorCode code)
    {
      require(command._opcode == RedoOpcode::LoseRange && command._payload.size() == 32,
              code, "LoseRange requires its complete 32-byte payload");
      LossIntent result{read_unsigned_le<std::uint64_t>(command._payload, 0),
                        read_unsigned_le<std::uint64_t>(command._payload, 8),
                        read_unsigned_le<std::uint64_t>(command._payload, 16),
                        SourceQueueSetId{read_unsigned_le<std::uint16_t>(command._payload, 24)}};
      require(result._first != 0 && result._first <= result._last && result._source != SourceQueueSetId::Invalid &&
                  all_zero(std::span{command._payload}.subspan(26)),
              code, "LoseRange sequence, Source, or reserved bytes are invalid");
      return result;
    }

    void check_receipt_identity(std::span<const std::uint64_t> words, const LossIntent& intent,
                                   TaskId actor, std::uint64_t incarnation,
                                   std::span<const std::uint64_t> sourceWords, ErrorCode code)
    {
      if (((words[1] >> 32) & 1) == 0)
      {
        require((words[1] & 0xffffffffu) == 0 && words[2] == 0 && words[3] == 0 && words[4] == 0,
                code, "Loss receipt has unmarked historical dequeue identity");
      }
      else
      {
        require((words[1] & 0xffff) == static_cast<std::uint16_t>(actor) &&
                    ((words[1] >> 16) & 0xffff) == static_cast<std::uint16_t>(intent._source) &&
                    words[2] != 0 && words[2] <= incarnation && words[3] != 0 &&
                    words[2] <= sourceWords[1] &&
                    (words[2] != sourceWords[1] || words[3] <= sourceWords[0]),
                code, "Loss receipt historical consumer tags disagree with its Source");
      }
    }

    void check_loss_receipt(std::span<const std::uint64_t> words, const LossIntent& intent,
                               bool replay, TaskId actor, std::uint64_t incarnation,
                               std::span<const std::uint64_t> sourceWords, ErrorCode code)
    {
      require(words.size() == 16 && all_zero(words.subspan(7)), code, "Loss receipt reserved words are nonzero");
      check_receipt_identity(words, intent, actor, incarnation, sourceWords, code);
      if (words[0] == 3)
      {
        (void)value(decode_receipt_words(words, ReceiptProfile::AllowLoss));
        return;
      }
      require(words[0] == 1 || (replay && words[0] == 4), code,
              "Loss cannot revoke an Issued pin, invent a receipt, or reuse an unrelated terminal result");
      const auto presence = words[1] >> 32;
      require((presence & ~std::uint64_t{5}) == 0 && words[5] == 0, code,
              "Loss receipt contains acknowledgment or unknown presence state");
      if (words[0] == 1)
      {
        (void)value(decode_receipt_words(words, ReceiptProfile::AllowLoss));
      }
      else
      {
        // The state is installed before presence/time words. A committed pass
        // may stop at any one of these stores; accept only this intent's old or
        // new values and repair all fields again, even when state is Lost.
        require(words[6] == 0 || words[6] == intent._time, code,
                "Interrupted Lost receipt contains another operation's terminal time");
      }
    }
  } // namespace

  bool has_loss_command(std::span<const RedoCommand> commands) noexcept
  {
    return std::any_of(commands.begin(), commands.end(), [](const auto& command)
    {
      return command._opcode == RedoOpcode::LoseRange;
    });
  }

  OperationResult validate_loss_commands(NativeFile& file, const FixedLayout& layout,
      std::span<const JournalRecordRange> records, std::span<const RedoCommand> commands,
      TaskId actor, std::uint64_t incarnation, bool allowLoss, bool replay, ErrorCode code)
  {
    try
    {
      require(allowLoss, code == ErrorCode::InvalidArgument ? ErrorCode::UnsupportedPolicy : code,
              "Session does not admit compact allowed-loss redo");
      require(commands.size() == 2 && commands[1]._opcode == RedoOpcode::WriteRange &&
                  commands[1]._target == commands[0]._target + 64 && commands[1]._payload.size() == 16,
              code, "Loss requires only LoseRange followed by its two absolute Queue progress words");
      const auto intent = loss_intent(commands[0], code);
      require(actor != TaskId::Invalid && incarnation != 0, code, "Loss requires a captured consumer claim");
      const auto queueRow = entity_row(file, layout, records, SectionKind::QueueIndex,
          SectionKind::QueueState, 3, 0, code, commands[0]._target);
      const auto queue = value(decode_queue_definition(queueRow._definition, queueRow._reference));
      const auto sourceRow = entity_row(file, layout, records, SectionKind::SourceIndex,
          SectionKind::SourceState, 4, static_cast<std::uint16_t>(intent._source), code);
      const auto source = value(decode_source_definition(sourceRow._definition, sourceRow._reference));
      const auto taskRow = entity_row(file, layout, records, SectionKind::TaskIndex,
          SectionKind::TaskState, 2, static_cast<std::uint16_t>(actor), code);
      const auto task = value(decode_task_definition(taskRow._definition, taskRow._reference));
      require(queue._id == QueueId{static_cast<std::uint16_t>(queueRow._index[0])} &&
                  queue._state == EntityState::Active && queueRow._index[3] == 2 &&
                  queue._registration._revision == queueRow._index[2] &&
                  queue._descriptor._delivery == DeliveryPolicy::AllowLoss &&
                  queue._descriptor._sourceSet == intent._source,
              code, "Loss target is not an Active allowed-loss Queue of this Source");
      require(source._id == intent._source && source._state == EntityState::Active && sourceRow._index[3] == 2 &&
                  source._registration._revision == sourceRow._index[2] &&
                  source._descriptor._owner == actor && source._descriptor._selection == SelectionPolicy::Strict &&
                  source._descriptor._wait == WaitPolicy::Spin &&
                  std::find(source._members.begin(), source._members.end(), queue._id) != source._members.end(),
              code, "Loss Source ownership, membership, state, or policy is invalid");
      require(task._id == actor && task._registration._revision == taskRow._index[2] &&
                  taskRow._state[0] == incarnation && (taskRow._state[6] & 3) == 3 &&
                  (taskRow._state[6] & ~std::uint64_t{7}) == 0 && taskRow._state[3] != 0 &&
                  taskRow._state[4] != 0 && taskRow._state[8] <= taskRow._state[1] &&
                  all_zero(std::span{taskRow._state}.subspan(10)) &&
                  (task._descriptor._kind != TaskKind::Process || taskRow._state[7] == 0),
              code, "Loss actor does not own the captured live Task claim");
      if (task._descriptor._kind == TaskKind::Thread)
      {
        const auto parent = entity_row(file, layout, records, SectionKind::TaskIndex,
            SectionKind::TaskState, 2, static_cast<std::uint16_t>(*task._descriptor._process), code);
        const auto definition = value(decode_task_definition(parent._definition, parent._reference));
        require(definition._descriptor._kind == TaskKind::Process && parent._state[0] == taskRow._state[7] &&
                    (parent._state[6] & 1) != 0 && parent._state[3] == taskRow._state[3],
                code, "Loss Thread claim belongs to an ended parent Process");
      }
      require(all_zero(std::span{sourceRow._state}.subspan(2)) && sourceRow._state[1] <= incarnation &&
                  (sourceRow._state[1] != 0 || sourceRow._state[0] == 0),
              code, "Loss cannot run while its Source is pinned or has invalid issuance context");
      const auto& queues = section(layout, SectionKind::QueueIndex, code);
      for (std::uint64_t slot = 0; slot != queues._count; ++slot)
      {
        const auto index = value(read_journal_words(file, queues._offset + slot * queues._stride, 4));
        require(index[0] == queueRow._index[0] || index[3] != 2 ||
                    std::find(source._members.begin(), source._members.end(),
                        QueueId{static_cast<std::uint16_t>(index[0])}) == source._members.end(),
                code, "Loss profile permits only one Active Queue in its Source");
      }
      const auto& words = queueRow._state;
      const auto count = words[0];
      const auto capacity = words[3];
      require(capacity != 0 && queue._descriptor._ringCapacity == capacity && count > capacity &&
                  words[5] == 1 && words[7] == 0 && words[11] <= 1 &&
                  (words[11] != 0 || words[10] == 0) && all_zero(std::span{words}.subspan(12)),
              code, "Loss Queue roots or reserved words are invalid");
      const auto floor = count - capacity + 1;
      require(intent._last == floor - 1 && intent._first <= intent._last &&
                  (words[8] == intent._first - 1 || (replay && words[8] == intent._last)) &&
                  read_unsigned_le<std::uint64_t>(commands[1]._payload, 0) == intent._last,
              code, "Loss range is not the complete overwritten nonterminal prefix");
      const auto& ring = record_at(records, words[2], 8, code);
      const auto ringBytes = ring_storage_bytes(capacity);
      require(ringBytes && ring._envelope._immutableBytes == 0 && ring._envelope._totalBytes == *ringBytes,
              code, "Loss Queue ring geometry is invalid");
      std::uint64_t sequence{};
      std::uint64_t previous{};
      std::uint64_t firstLink{};
      std::uint64_t oldPending{};
      std::uint64_t newPending{};
      std::uint64_t lastIncarnation{};
      std::uint64_t lastAck{};
      bool hasAck{};
      for (const auto& record : records)
      {
        require(record._envelope._kind != 10, code, "Loss profile does not admit retry history");
        if (record._envelope._kind != 9)
        {
          continue;
        }
        const auto publication = value(read_journal_publication(file, record, code));
        const auto& destination = publication._destinations.front();
        if (destination._queue != queue._id)
        {
          continue;
        }
        require(sequence != std::numeric_limits<std::uint64_t>::max() &&
                    destination._queueSequence == sequence + 1 && destination._previousDelivery == previous &&
                    publication._producer == queue._descriptor._producer,
                code, "Loss Queue history is not a complete contiguous retained chain");
        ++sequence;
        const auto link = publication._layout._destinationsOffset;
        if (sequence == 1)
        {
          firstLink = link;
        }
        if (sequence == intent._first)
        {
          oldPending = link;
        }
        if (sequence == intent._last + 1)
        {
          newPending = link;
        }
        previous = link;
        lastIncarnation = publication._producerIncarnation;
        const auto receiptWords = value(read_journal_words(file, destination._receiptReference, 16));
        if (sequence >= intent._first && sequence <= intent._last)
        {
          check_loss_receipt(receiptWords, intent, replay, actor, incarnation, sourceRow._state, code);
        }
        else
        {
          const auto receipt = value(decode_receipt_words(receiptWords, ReceiptProfile::AllowLoss));
          check_receipt_identity(receiptWords, intent, actor, incarnation, sourceRow._state, code);
          require(sequence < intent._first ?
                      receipt._state == ReceiptState::Acknowledged || receipt._state == ReceiptState::Lost :
                      receipt._state == ReceiptState::Pending,
                  code, "Loss prefix skips a nonterminal receipt or encounters an unexpected later terminal/pin");
        }
        if (receiptWords[0] == 3)
        {
          lastAck = receiptWords[5];
          hasAck = true;
        }
        if (sequence >= floor)
        {
          const auto index = (sequence - 1) % capacity;
          const auto slot = value(read_journal_words(file, words[2] + record_envelope_bytes + index * 32, 4));
          require(slot[0] == sequence && slot[1] == link && slot[2] == record._offset &&
                      slot[3] == 1 + (sequence - 1) / capacity,
                  code, "Loss boundary is not backed by the exact current ring window");
        }
      }
      require(sequence == count && previous == words[1] && firstLink == words[6] &&
                  lastIncarnation == words[4] && oldPending != 0 && newPending != 0 &&
                  (words[9] == oldPending || (replay && words[9] == newPending)) &&
                  read_unsigned_le<std::uint64_t>(commands[1]._payload, 8) == newPending &&
                  words[10] == lastAck && words[11] == (hasAck ? 1u : 0u),
              code, "Loss final watermark/link or preserved acknowledgment context is invalid");
      const auto padding = (*ringBytes - record_envelope_bytes - capacity * ring_slot_bytes) / 8;
      require(all_zero(value(read_journal_words(file,
          words[2] + record_envelope_bytes + capacity * ring_slot_bytes, static_cast<std::size_t>(padding)))),
          code, "Loss ring allocation padding is nonzero");
      return std::monostate{};
    }
    catch (...)
    {
      return caught_error();
    }
  }

  OperationResult apply_loss_command(NativeFile& file, const FixedLayout& layout,
      std::span<const JournalRecordRange> records, const RedoCommand& command,
      const std::function<void()>& checkpoint)
  {
    try
    {
      const auto intent = loss_intent(command, ErrorCode::IoError);
      // Queue identity is immutable and the complete intent was validated before
      // apply. Match the captured receipt range by its Source/Queue state target.
      const auto& indexes = section(layout, SectionKind::QueueIndex, ErrorCode::IoError);
      const auto& states = section(layout, SectionKind::QueueState, ErrorCode::IoError);
      require(command._target >= states._offset && states._stride != 0 &&
                  (command._target - states._offset) % states._stride == 0 &&
                  (command._target - states._offset) / states._stride < states._count,
              ErrorCode::IoError, "Loss apply target is not a Queue state row");
      const auto slot = (command._target - states._offset) / states._stride;
      const auto queueId = value(read_journal_words(file, indexes._offset + slot * indexes._stride, 1))[0];
      for (const auto& record : records)
      {
        if (record._envelope._kind != 9)
        {
          continue;
        }
        const auto publication = value(read_journal_publication(file, record, ErrorCode::IoError));
        const auto& destination = publication._destinations.front();
        if (static_cast<std::uint16_t>(destination._queue) != queueId ||
            destination._queueSequence < intent._first || destination._queueSequence > intent._last)
        {
          continue;
        }
        auto words = value(read_journal_words(file, destination._receiptReference, 16));
        if (words[0] == 3)
        {
          continue;
        }
        words[0] = 4;
        words[1] = (words[1] & 0xffffffffu) | (((words[1] >> 32) & 1u) | 4u) << 32;
        words[5] = 0;
        words[6] = intent._time;
        auto mapping = value(file.map_read_write(destination._receiptReference, receipt_bytes));
        const auto bytes = value(mapping.writable_bytes());
        for (std::size_t word = 0; word != 7; ++word)
        {
          value(AtomicWord::bind(bytes, word * 8)).store_release(words[word]);
          checkpoint();
        }
      }
      return std::monostate{};
    }
    catch (...)
    {
      return caught_error();
    }
  }
} // namespace angaria::detail::mapped
