#pragma once

#include "producer_log_format.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <vector>

namespace angaria::detail::recorded
{
  struct ProducerLogPaths
  {
    std::filesystem::path _log;
    std::filesystem::path _owner;
  };

  struct ProducerLogOptions
  {
    ProducerLogHeader _header;
    // Frozen outgoing Queue identities supplied by the checked catalog owner.
    // Opening copies this bounded list; normal operations do not consult Session.
    std::vector<QueueId> _queues;
  };

  struct ProducerAppendRequest
  {
    PublicationId _publication{};
    QueueId _queue{};
    MessageView _message;
    std::uint64_t _schemaReference{};
    std::uint64_t _topologyRevision{};
    // The upper layer samples its acceptance clock. No clock or schema lookup
    // is hidden in this prepared-storage primitive; epoch zero is valid.
    // On an accepted-ID retry, the recorded timestamp wins over a new sample.
    UtcTimestamp _publishedAt{};
  };

  struct ProducerAppendResult
  {
    LogReference _reference;
    std::uint64_t _recordOrdinal{};
    std::uint64_t _queueSequence{};
    bool _duplicate{};
  };

  enum class ProducerLogCheckpoint
  {
    RecoveryBeforeScan,
    PartialMaterialization,
    BeforeCommit,
    AfterCommit
  };

  // Private qualification seam, absent from ordinary opens. RecoveryBeforeScan
  // holds the actual log claim before cold committed-prefix validation begins.
  // It adds no checkpoint to ordinary messaging. PartialMaterialization
  // deliberately writes a provisional prefix before the ordinary complete codec
  // write; it represents an interrupted suffix, not a per-word hardware proof.
  // Observers may pause/terminate the process or return a synthetic error, but
  // must not reenter the log or modify its mappings.
  class ProducerLogObserver
  {
  public:
    virtual ~ProducerLogObserver() = default;
    virtual OperationResult checkpoint(ProducerLogCheckpoint point) = 0;
  };

  struct OwnedPublicationRecord
  {
    MessageHeader _header;
    LogReference _reference;
    std::uint64_t _recordOrdinal{};
    std::uint64_t _catalogRevision{};
    std::uint64_t _schemaReference{};
    std::uint64_t _topologyRevision{};
    std::vector<DestinationRecord> _destinations;
    std::vector<std::byte> _payload;
  };

  class ProducerReplayCursor
  {
  public:
    std::uint64_t committed_end() const noexcept;
    std::size_t record_count() const noexcept;

  private:
    friend class ProducerLogReader;
    SessionId _session{};
    TaskId _producer{};
    std::uint64_t _device{};
    std::uint64_t _inode{};
    std::uint64_t _end{};
    std::size_t _index{};
    std::size_t _limit{};
  };

  class ProducerLog
  {
  public:
    ProducerLog() noexcept;
    ~ProducerLog();
    ProducerLog(ProducerLog&&) noexcept;
    ProducerLog& operator=(ProducerLog&&) noexcept;
    ProducerLog(const ProducerLog&) = delete;
    ProducerLog& operator=(const ProducerLog&) = delete;

    // Offline bootstrap, with existing parent directories. Do not race opens
    // against creation; the eventual Session bootstrap publishes these resources.
    static OperationResult create(const ProducerLogPaths& paths, const ProducerLogOptions& options);
    // An authoritative actor is supplied only by a caller retaining that live
    // Task claim and serializing its shared writer. It disables this primitive's
    // standalone thread affinity; public Thread ownership stays in TaskState.
    // Retain one log owner for the claim's entire ID-issuance lifetime. Reopening
    // the same actor supports failed setup before an owner escapes, rather than
    // promising persistence of issued-but-unaccepted IDs between log objects.
    static Result<ProducerLog> open_writer(const ProducerLogPaths& paths,
                                           const ProducerLogOptions& options,
                                           ProducerLogObserver* observer = nullptr,
                                           std::optional<TaskClaim> actor = {});

    Result<TaskClaim> claim() const;
    Result<PublicationId> next_publication_id();
    // ID-only admission for a new append, without reserving or retiring the ID.
    // Resolve accepted retries through lookup before applying this check.
    OperationResult check_new_publication(PublicationId publication) const;
    // Direct point-to-point prepared append. Assigns record/Queue sequence and
    // records one Accepted destination. Schema/payload semantic validation and
    // ring-credit admission remain the upper layer's responsibility.
    Result<ProducerAppendResult> append(const ProducerAppendRequest& request);
    // Actor-owned lookup is settled, not a query against another live writer.
    Result<std::optional<LogReference>> lookup(PublicationId publication) const;
    Result<PublicationRecordView> view(const LogReference& reference) const;
    Result<std::uint64_t> committed_end() const;
    Result<std::size_t> record_count() const;
    // Derived accepted Queue progress, rebuilt at open and advanced at commit.
    // Reading it does not inspect files or traverse retained Publications.
    Result<std::uint64_t> queue_sequence(QueueId queue) const;

  private:
    struct State;
    explicit ProducerLog(std::unique_ptr<State> state) noexcept;
    std::unique_ptr<State> _state;
  };

  class ProducerLogReader
  {
  public:
    ProducerLogReader() noexcept;
    ~ProducerLogReader();
    ProducerLogReader(ProducerLogReader&&) noexcept;
    ProducerLogReader& operator=(ProducerLogReader&&) noexcept;
    ProducerLogReader(const ProducerLogReader&) = delete;
    ProducerLogReader& operator=(const ProducerLogReader&) = delete;

    // Unbound thread affinity requires external serialization (public Task or
    // archive cursor); process/fork ownership is still checked in both modes.
    static Result<ProducerLogReader> open(const std::filesystem::path& logPath,
                                          const ProducerLogOptions& options, bool threadBound = true);
    // Validate only newly acquired committed ranges and freeze an explicit cut.
    Result<ProducerReplayCursor> capture();
    Result<std::optional<PublicationRecordView>> next(ProducerReplayCursor& cursor) const;
    // Explicit live extension of this cursor. After its current cut is
    // exhausted, include at most one additional record, validating at most one
    // newly committed record. Other cursor copies remain frozen. An empty
    // poll only acquires the tail; ordinary polls allocate no storage.
    Result<std::optional<PublicationRecordView>> poll_next(ProducerReplayCursor& cursor);
    Result<PublicationRecordView> view(const LogReference& reference) const;
    Result<OwnedPublicationRecord> copy_record(const LogReference& reference,
                                               std::uint64_t maxPayloadBytes) const;
    Result<std::uint64_t> committed_end() const;

    // Cached accepted progress for one Queue in this producer's shared log.
    // Capture/poll validation advances it; this query does not scan storage.
    Result<std::uint64_t> queue_sequence(QueueId queue) const;

  private:
    struct State;
    explicit ProducerLogReader(std::unique_ptr<State> state) noexcept;
    void check_cursor(const ProducerReplayCursor& cursor) const;
    std::unique_ptr<State> _state;
  };
} // namespace angaria::detail::recorded
