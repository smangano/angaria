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
    PartialMaterialization,
    BeforeCommit,
    AfterCommit
  };

  // Private qualification seam, absent from ordinary opens. PartialMaterialization
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
    static Result<ProducerLog> open_writer(const ProducerLogPaths& paths,
                                           const ProducerLogOptions& options,
                                           ProducerLogObserver* observer = nullptr);

    Result<TaskClaim> claim() const;
    Result<PublicationId> next_publication_id();
    // Direct point-to-point prepared append. Assigns record/Queue sequence and
    // records one Accepted destination. Schema/payload semantic validation and
    // ring-credit admission remain the upper layer's responsibility.
    Result<ProducerAppendResult> append(const ProducerAppendRequest& request);
    // Actor-owned lookup is settled, not a query against another live writer.
    Result<std::optional<LogReference>> lookup(PublicationId publication) const;
    Result<PublicationRecordView> view(const LogReference& reference) const;
    Result<std::uint64_t> committed_end() const;
    Result<std::size_t> record_count() const;

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

    static Result<ProducerLogReader> open(const std::filesystem::path& logPath,
                                          const ProducerLogOptions& options);
    // Validate only newly acquired committed ranges and freeze an explicit cut.
    Result<ProducerReplayCursor> capture();
    Result<std::optional<PublicationRecordView>> next(ProducerReplayCursor& cursor) const;
    Result<PublicationRecordView> view(const LogReference& reference) const;
    Result<OwnedPublicationRecord> copy_record(const LogReference& reference,
                                               std::uint64_t maxPayloadBytes) const;
    Result<std::uint64_t> committed_end() const;

  private:
    struct State;
    explicit ProducerLogReader(std::unique_ptr<State> state) noexcept;
    std::unique_ptr<State> _state;
  };
} // namespace angaria::detail::recorded
