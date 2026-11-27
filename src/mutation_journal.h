#pragma once

#include "journal_format.h"
#include "journal_validation.h"
#include "session_controller.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace angaria::detail::mapped
{
  struct JournalSnapshot
  {
    std::uint64_t _generation{};
    std::uint64_t _tail{};
    std::uint64_t _accounted{};
    std::uint64_t _revision{};
    std::uint64_t _manifest{};
    std::uint64_t _fileBytes{};

    bool operator==(const JournalSnapshot&) const = default;
  };

  struct JournalRecord
  {
    std::uint16_t _kind{};
    std::uint16_t _encoding{1};
    std::uint64_t _previous{};
    std::vector<std::byte> _immutable;
    std::vector<std::byte> _mutable;
  };

  // Captured under the gate after recovery. A later operation generation makes
  // this plan stale even when it changes neither the allocation tail nor revision.
  struct TransactionPlan
  {
    JournalSnapshot _base;
    JournalKind _kind{JournalKind::FixedState};
    TransactionSubtype _subtype{TransactionSubtype::Registry};
    TaskId _actor{};
    std::uint64_t _actorIncarnation{};
    std::uint64_t _newRevision{};
    std::vector<JournalRecord> _records;
    std::vector<RedoCommand> _commands;
  };

  struct TransactionResult
  {
    std::uint64_t _generation{};
    std::uint64_t _tail{};
    std::uint64_t _manifest{};
    std::vector<std::uint64_t> _records;
  };

  enum class JournalCheckpoint
  {
    JournalBodyWord,
    Prepared,
    FileGrowth,
    RecordBodyWord,
    RecordReady,
    Committed,
    RedoWord,
    RecordCommitted,
    EpilogueWord,
    Applied,
    Idle,
    Rollback
  };

  // Private fault-injection seam. Production Session callers never supply an
  // injector. Tests may return an I/O error or stop a process at a real boundary;
  // they must not reenter the controller or mutate session state themselves.
  class JournalFaultInjector
  {
  public:
    virtual ~JournalFaultInjector() = default;
    virtual OperationResult checkpoint(JournalCheckpoint point, std::uint64_t index) = 0;
  };

  struct MutationJournalState;

  class MutationJournal
  {
  public:
    ~MutationJournal();
    MutationJournal(MutationJournal&&) noexcept;
    MutationJournal& operator=(MutationJournal&&) noexcept;
    MutationJournal(const MutationJournal&) = delete;
    MutationJournal& operator=(const MutationJournal&) = delete;

    static Result<MutationJournal> open(std::shared_ptr<SessionController> controller,
                                        const MutationGuard& guard,
                                        JournalFaultInjector* injector = nullptr);
    OperationResult recover(const MutationGuard& guard);
    Result<JournalSnapshot> snapshot(const MutationGuard& guard);
    // Export owning framing copies from this fresh validation pass. Callers can
    // resolve exact record boundaries without reading the prefix again. Failed
    // validation clears the output rather than leaving stale records available.
    Result<JournalSnapshot> snapshot(const MutationGuard& guard,
                                      std::vector<JournalRecordRange>& records);
    Result<TransactionResult> execute(const MutationGuard& guard, const TransactionPlan& plan);

  private:
    explicit MutationJournal(std::unique_ptr<MutationJournalState> state) noexcept;
    std::unique_ptr<MutationJournalState> _state;
  };
} // namespace angaria::detail::mapped
