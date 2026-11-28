#pragma once

#include "angaria/runtime.h"

#include <cstdint>
#include <iosfwd>
#include <memory>
#include <optional>
#include <vector>

namespace angaria
{
  namespace detail
  {
    struct LogArchiveState;
    struct LogCursorState;
  }

  struct ArchiveLimits
  {
    // Native encoded bytes, including the Publication header/destinations.
    std::uint64_t _maxRecordBytes{1048576};
    // Bounds the sum of configured producer Publication capacities before
    // allocating their read indexes, not only the currently accepted count.
    std::uint64_t _maxRecords{1048576};
    // Bounds the sum of fixed producer-file reservations before mapping them.
    // These are conservative representation limits, not exact resident RSS.
    std::uint64_t _maxArchiveBytes{1073741824};
    // Aggregate rendered inspection bytes, including the archive summary and
    // each complete native-destination/decoded-message unit. Export is separate.
    std::uint64_t _maxTextBytes{16777216};
  };

  struct LoggedAcceptanceOutcome
  {
    QueueId _queue{};
    DestinationStatus _acceptance{DestinationStatus::Accepted};
    ErrorCode _reason{ErrorCode::None};
    std::uint64_t _queueSequence{};
    UtcTimestamp _enqueuedAt{};
  };

  struct LogRecord
  {
    // Owns bytes and exact schema decoding context. This is a publication
    // header, without invented consumer observations or acknowledgment rights.
    Message _message;
    std::uint64_t _topologyRevision{};
    std::vector<LoggedAcceptanceOutcome> _destinations;
  };

  struct LogFilter
  {
    std::optional<TaskId> _producer;
    std::optional<SchemaId> _schema;
    std::optional<QueueId> _queue;
  };

  class LogCursor
  {
  public:
    ~LogCursor();
    LogCursor(LogCursor&&) noexcept;
    LogCursor& operator=(LogCursor&&) noexcept;
    LogCursor(const LogCursor&) = delete;
    LogCursor& operator=(const LogCursor&) = delete;

    // Empty means exhaustion of this frozen cut, not a live receive timeout.
    // Owning records remain valid after the cursor, archive or Session closes.
    Result<std::optional<LogRecord>> read_next();
    OperationResult rewind();

  private:
    friend class LogArchive;
    explicit LogCursor(std::shared_ptr<detail::LogCursorState> state);
    std::shared_ptr<detail::LogCursorState> _state;
  };

  class LogArchive
  {
  public:
    LogArchive(LogArchive&&) noexcept = default;
    LogArchive& operator=(LogArchive&&) noexcept = default;
    LogArchive(const LogArchive&) = default;
    LogArchive& operator=(const LogArchive&) = default;

    // Acquires immutable accepted prefixes while producers keep working. The
    // Session must already have admitted messaging. Capturing does not attach a
    // Task, seal setup, acknowledge processing or recover a producer's log.
    // Each producer has its own committed-tail cut; there is no global instant
    // or original consumer receive order. Between streams, iteration uses Task
    // ID order. Within each producer/Queue it preserves accepted record order.
    // Receipt snapshots, standalone catalog bundles and execution traces remain
    // later APIs. Backing files must remain in place: live Session deletion and
    // log rotation/reclamation are not supported by the current runtime.
    static Result<LogArchive> capture_live(Session& session, ArchiveLimits limits = {});

    SessionId session() const noexcept;
    std::uint64_t publication_count() const noexcept;
    Result<RegistrySnapshot> catalog() const;
    Result<LogCursor> publications(LogFilter filter = {}) const;
    // Uses MessageInspector's exact schema-aware rendering and its default
    // 16 MiB catalog budget. Capacity errors leave only complete output units;
    // an I/O failure can leave a partial final unit in the caller-owned stream.
    OperationResult inspect(std::ostream& output) const;
    // The existing flat MessageCapture encoding exports publication headers
    // and payloads only. Keep the matching catalog() for schema decoding. This
    // is not a self-contained catalog/receipt bundle. I/O failure may leave a
    // complete prefix and a partial last record in the borrowed output stream.
    OperationResult export_capture(std::ostream& output) const;

  private:
    explicit LogArchive(std::shared_ptr<detail::LogArchiveState> state);
    std::shared_ptr<detail::LogArchiveState> _state;
  };
} // namespace angaria
