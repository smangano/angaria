#pragma once

// Review-only declarations. No log/archive/replay implementation is supplied.
// See docs/public-interface.md for the proposed workflows and open decisions.

#include "angaria/proposed/core.h"
#include "angaria/proposed/schema.h"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace angaria::proposed
{
  namespace detail
  {
    class CatalogState;
    class ArchiveState;
    class ArchiveContext;
    class LogCursorState;
    class ReplayState;
    class InputTraceState;
  }

  struct ArchiveLimits
  {
    // Limits are checked before copies/materialization. Pool text and pool
    // metadata have separate budgets; entry/count limits also bound vectors
    // whose process representation is wider than the stored representation.
    std::uint64_t _maxRecordBytes{1048576};
    std::uint64_t _maxCatalogBytes{16777216};
    std::uint64_t _maxPoolBytes{16777216};
    std::uint64_t _maxPoolMetadataBytes{4194304};
    std::uint32_t _maxPoolEntries{65536};
    std::uint32_t _maxDestinationsPerRecord{65535};
    // One returned record's payload plus expanded destination/progress arrays;
    // decode() applies this and the codec's own materialization/nesting limits.
    // Budgets are representation estimates, not a promise about exact RSS.
    std::uint64_t _maxMaterializedBytes{16777216};
    std::uint64_t _maxArchiveBytes{1073741824};
    std::uint64_t _maxRenderedBytes{4194304};
  };

  // Opaque bookmarks bind an immutable archive cut and cursor projection.
  // They are not native offsets and cannot be moved to another filtered cursor.
  class LogPosition
  {
  public:
    static Result<LogPosition> parse(std::string_view token, std::uint64_t maxBytes = 4096);
    std::string_view token() const & noexcept;
    std::string_view token() const && = delete;
    bool operator==(const LogPosition&) const = default;

  private:
    std::string _token;
  };

  struct ReplayOrigin
  {
    PublicationId _publication{};
    LogPosition _position;
    std::optional<QueueId> _queue;
    std::optional<std::uint64_t> _queueSequence;
  };

  enum class RecordedReceiptState
  {
    Unaccepted,
    Pending,
    Issued,
    Acknowledged,
    Lost
  };

  // This is progress at the archive cut, not an event history of every borrow.
  // A recorded token is inspection data and grants no live acknowledgment right.
  struct ReceiptSnapshot
  {
    RecordedReceiptState _state{RecordedReceiptState::Unaccepted};
    std::optional<DeliveryToken> _lastIssuance;
    std::optional<UtcTimestamp> _dequeuedAt;
    std::optional<UtcTimestamp> _acknowledgedAt;
    std::optional<UtcTimestamp> _lostAt;
  };

  struct LoggedDestination
  {
    QueueId _queue{};
    DestinationStatus _acceptance{DestinationStatus::Rejected};
    ErrorCode _reason{ErrorCode::None};
    std::optional<std::uint64_t> _queueSequence;
    std::optional<UtcTimestamp> _enqueuedAt;
    ReceiptSnapshot _receipt;
  };

  // Immutable acceptance evidence, without the receipt's later progress at the
  // archive cut. Original rows and later attempt rows must remain distinguishable.
  struct LoggedAcceptanceOutcome
  {
    QueueId _queue{};
    DestinationStatus _acceptance{DestinationStatus::Rejected};
    ErrorCode _reason{ErrorCode::None};
    std::optional<std::uint64_t> _queueSequence;
    std::optional<UtcTimestamp> _enqueuedAt;
  };

  enum class AcceptanceAttemptKind
  {
    Initial,
    Retry
  };

  struct AcceptanceAttempt
  {
    AcceptanceAttemptKind _kind{AcceptanceAttemptKind::Initial};
    TaskClaim _actor{};
    // Present only when a common attempt timestamp was recorded. The initial
    // Publication has its own _publishedAt; individual accepted destination
    // timestamps remain in outcomes and are never synthesized from it.
    std::optional<UtcTimestamp> _acceptedAt;
    // Exactly the outcomes retained by this attempt, not cumulative receipt
    // progress or fabricated outcomes for destinations it did not attempt.
    std::vector<LoggedAcceptanceOutcome> _outcomes;
  };

  struct RecordedInput
  {
    TaskClaim _consumer{};
    std::uint64_t _ordinal{};
    std::chrono::nanoseconds _elapsed{}; // Monotonic elapsed time in this trace.
    MessageContext _context; // Original handler context, before replay marking.
    DeliveryToken _recordedToken{}; // Historical issuance data, not ownership.
  };

  class LogRecord
  {
  public:
    ReplayOrigin _origin;
    Message _message;
    UtcTimestamp _publishedAt{};
    std::uint64_t _topologyRevision{};
    std::optional<PublishingQueueSetId> _publishingSet;
    std::vector<LoggedAcceptanceOutcome> _originalDestinations;
    // Cumulative acceptance plus latest receipt progress at this archive cut.
    std::vector<LoggedDestination> _destinations;
    // Present for deliveries() or recorded inputs. A fan-out publication may then
    // appear once per accepted destination; publications() yields it once.
    std::optional<LoggedDestination> _selectedDelivery;
    // Present only for acceptance_attempts(). Attempts are streamed rather
    // than building an unbounded per-Publication history vector. Original,
    // cumulative, and selected-attempt arrays all charge materialization caps.
    std::optional<AcceptanceAttempt> _selectedAttempt;
    std::optional<RecordedInput> _recordedInput; // ExecutionTrace projection only.

    // Owning values resolve recorded Symbol/String references through this
    // record's retained archive context, never a pool in a live target Session.
    Result<std::vector<FieldValue>> decode(const SchemaCodec& codec) const;

  private:
    friend class LogCursor;
    std::shared_ptr<const detail::ArchiveContext> _context;
  };

  struct LogFilter
  {
    std::optional<TaskId> _producer;
    std::optional<std::uint64_t> _producerIncarnation;
    std::optional<SchemaId> _schema;
    std::optional<PublishingQueueSetId> _publishingSet;
    std::optional<UtcTimestamp> _publishedFrom;
    std::optional<UtcTimestamp> _publishedBefore;
    // Inclusive lower and exclusive upper bounds. Publication ranges require
    // a producer; without an incarnation they apply to each selected claim.
    std::optional<std::uint64_t> _publicationSequenceFrom;
    std::optional<std::uint64_t> _publicationSequenceBefore;
    // Delivery projections only; a Source projection applies them separately
    // to each selected Queue. Queue sequences are never global positions.
    std::optional<std::uint64_t> _queueSequenceFrom;
    std::optional<std::uint64_t> _queueSequenceBefore;
  };

  struct RecordedTaskState
  {
    TaskId _task{};
    std::uint64_t _incarnation{};
    std::uint64_t _restartCount{};
    std::uint64_t _processId{};
    std::uint64_t _threadId{};
    bool _recordedAttached{}; // Stored diagnostics, never proof of live ownership.
    std::optional<UtcTimestamp> _lastAttachedAt;
    std::optional<UtcTimestamp> _lastDetachedAt;
  };
  struct ArchiveObservation
  {
    std::uint64_t _registryRevision{};
    std::uint64_t _operationGeneration{};
    StorageStatus _storage;
    std::vector<RecordedTaskState> _tasks;
    std::vector<QueueStatus> _queues;
    std::vector<SourceStatus> _sources;
    std::vector<PublishingStatus> _publishing;
  };

  class Catalog
  {
  public:
    static Result<Catalog> load(const std::filesystem::path& path, ArchiveLimits limits = {});
    OperationResult save(const std::filesystem::path& newPath) const;
    SessionId session() const;
    Result<SessionMetadata> metadata() const;
    Result<SessionOptions> options() const;
    Result<RegistrySnapshot> snapshot(std::optional<std::uint64_t> revision = std::nullopt) const;
    Result<SchemaCodec> compile(SchemaId schema) const;
    Result<std::string> inspect(const LogRecord& record) const;

  private:
    std::shared_ptr<const detail::CatalogState> _state;
  };

  class LogCursor
  {
  public:
    LogCursor(LogCursor&&) noexcept;
    LogCursor& operator=(LogCursor&&) noexcept;
    LogCursor(const LogCursor&) = delete;
    LogCursor& operator=(const LogCursor&) = delete;
    ~LogCursor();

    // Each returned record owns its payload and bounded destination metadata,
    // and retains decoding context. It can outlive the cursor/archive; reading
    // another record invalidates none. Counts and expanded representation bytes
    // are charged to the originating ArchiveLimits before allocation/copying.
    // Empty means clean exhaustion of the selected retained range. Requested
    // missing/retired history is HistoryUnavailable, never fabricated EOF.
    Result<std::optional<LogRecord>> read_next();
    Result<LogPosition> position() const;
    // A bookmark from another archive cut/projection is InvalidArgument.
    // A valid position whose required retained history is gone reports
    // HistoryUnavailable; neither failure silently jumps to a later message.
    OperationResult seek(const LogPosition& nextRecord);
    OperationResult rewind();

  private:
    std::shared_ptr<detail::LogCursorState> _state;
  };

  class LogArchive
  {
  public:
    // Nonblocking open must acquire/retain a read freeze against cooperating
    // mutation and reclamation for every archive/cursor/context still reading
    // backing storage. If stable input cannot be guaranteed, return OwnershipBusy
    // without an archive; a size check or caller assurance is not sufficient.
    // The freeze can block cooperating writers; use capture_live for a running
    // Session. External code bypassing the coordination protocol is outside
    // this guarantee. Reading is physically read-only; supported pending redo
    // is overlaid in private memory. It does not acquire a Task, acknowledge a
    // message, or repair the original file.
    // An unsupported encoding/capability fails explicitly with UnsupportedPolicy
    // rather than skipping records; malformed retained bytes are IoError.
    // A Session with live Task claims returns OwnershipBusy; inspecting a
    // running application uses capture_live(), rather than freezing its loop.
    static Result<LogArchive> open_session(const std::filesystem::path& directory,
                                            ArchiveLimits limits = {});
    // Completed immutable portable bundles retain exact definitions, source pool
    // namespaces/values, and progress at the cut. They are a new artifact, not
    // the existing flat MessageCaptureWriter record stream without a catalog.
    static Result<LogArchive> open_capture(const std::filesystem::path& path,
                                            ArchiveLimits limits = {});
    // A cooperating live capture takes a consistent committed cut. Ordinary
    // Session recovery may repair its predecessor first; delivery progress is
    // not advanced. Destination paths must not already exist.
    static Result<LogArchive> capture_live(Session& session, const std::filesystem::path& newPath,
                                            ArchiveLimits limits = {});

    SessionId session() const;
    Result<Catalog> catalog() const;
    // Captured progress/last Task diagnostics, not an unlimited lifecycle trace.
    Result<ArchiveObservation> observe() const;
    OperationResult export_capture(const std::filesystem::path& newPath) const;
    // Proven absent identities return NotFound; retired/unavailable required
    // history returns HistoryUnavailable, rather than proof of nonacceptance.
    Result<LogRecord> publication(PublicationId publication) const;
    Result<LogRecord> delivery(QueueId queue, std::uint64_t sequence) const;
    // Publications use their initial commit order. Delivery projections use
    // acceptance commit order (including retry acceptance), then destination
    // order within a commit. Queue order is preserved; a Source projection is
    // a reproducible input schedule, not the original consumer's receive order.
    Result<LogCursor> publications(LogFilter filter = {}) const;
    Result<LogCursor> deliveries(QueueId queue, LogFilter filter = {}) const;
    Result<LogCursor> deliveries(SourceQueueSetId source, LogFilter filter = {}) const;
    // One entry per committed initial/retry attempt in commit order. Rejected
    // zero-acceptance attempts have no stored record and are not invented.
    Result<LogCursor> acceptance_attempts(LogFilter filter = {}) const;

  private:
    std::shared_ptr<const detail::ArchiveState> _state;
  };

  // OPTIONAL application instrumentation, separate from the ordinary message
  // log. One recorder serializes one consumer processing stream. Call just
  // before invoking its handler, including repeated handling of the same pin.
  // It copies the selected message and its exact schema/pool context so later
  // Session reclamation cannot invalidate the completed self-contained trace.
  class InputTraceRecorder
  {
  public:
    static Result<InputTraceRecorder> create(const std::filesystem::path& newPath,
                                               Task& consumer, ArchiveLimits limits = {});
    InputTraceRecorder(InputTraceRecorder&&) noexcept;
    InputTraceRecorder& operator=(InputTraceRecorder&&) noexcept;
    InputTraceRecorder(const InputTraceRecorder&) = delete;
    InputTraceRecorder& operator=(const InputTraceRecorder&) = delete;
    ~InputTraceRecorder();
    // Retains this exact native claim and affinity. Validate current ownership
    // and matching pin before resolving canonical stored bytes; never dereference
    // a caller's stale borrowed span. Copy all schema/pool/context dependencies.
    // Success publishes the complete framed input/dependencies beyond userspace
    // buffering before processing begins: process-termination scope, not fsync or
    // power-loss durability. An I/O failure is terminal; only close is usable,
    // and readers can inspect the validated complete prefix, not skip a bad tail.
    // Return this recorder's input ordinal, not an immutable-cut seek bookmark.
    Result<std::uint64_t> record_input(const DeliveryView& delivery);
    OperationResult close();
  private:
    std::shared_ptr<detail::InputTraceState> _state;
  };

  class ExecutionTrace
  {
  public:
    struct OpenOptions
    {
      ArchiveLimits _limits;
      bool _allowIncompleteTail{true};
    };
    struct Status
    {
      TaskClaim _consumer{};
      UtcTimestamp _startedAt{}; // UTC anchor sampled when the recorder starts.
      std::uint64_t _completeInputs{};
      bool _finalized{};
      bool _incompleteTail{};
    };
    // Snapshot a validated complete prefix, including after process termination.
    // Incomplete-tail use is explicit in status(); malformed completed entries
    // reject, rather than silently skipping them. No source file is repaired.
    static Result<ExecutionTrace> open(const std::filesystem::path& path,
                                        OpenOptions options);
    static Result<ExecutionTrace> open(const std::filesystem::path& path);
    Result<Status> status() const;
    Result<Catalog> catalog() const;
    // Original per-recorder input ordinal, not publication/Queue/UTC order.
    // Filtering preserves relative order and exposes original ordinal gaps.
    Result<LogCursor> inputs(LogFilter filter = {}) const;
  private:
    std::shared_ptr<const detail::InputTraceState> _state;
  };

  enum class ReplayPacing
  {
    Immediate,
    RecordedIntervals
  };

  enum class ReplayTimeBasis
  {
    Publication,
    Enqueue,
    InputTraceElapsed
  };

  struct ReplayOptions
  {
    // Speed affects wall-clock pacing, not original record order. Recorded
    // negative clock deltas contribute zero delay; virtual time is monotonic.
    // Enqueue time requires a selected delivery. InputTraceElapsed requires a
    // recorded-input projection and anchors virtual UTC at trace startedAt plus
    // monotonic recorded elapsed time, not the message's original wall clock.
    // Default replay sleeps
    // nowhere; pacing is not a real-time or scheduling guarantee.
    ReplayPacing _pacing{ReplayPacing::Immediate};
    ReplayTimeBasis _timeBasis{ReplayTimeBasis::Publication};
    double _speed{1.0};
  };

  struct ReplayFrame
  {
    LogRecord _record;
    // The same application-handler context used by live delivery. Original
    // identities/timestamps remain provenance and _replayed is true; no live
    // delivery token or acknowledgment operation is fabricated for replay.
    MessageContext _context;
    UtcTimestamp _virtualTime{};
  };

  class ReplayCheckpoint
  {
  public:
    static Result<ReplayCheckpoint> parse(std::string_view token, std::uint64_t maxBytes = 4096);
    std::string_view token() const & noexcept;
    std::string_view token() const && = delete;

  private:
    std::string _token;
  };

  enum class ReplayDecision
  {
    Continue,
    Pause,
    Stop
  };

  enum class ReplayStopReason
  {
    End,
    Paused,
    Stopped,
    MessageLimit,
    TimeLimit,
    Cancelled
  };

  struct ReplayRunLimits
  {
    // Message count must be positive. Zero elapsed time disables that limit;
    // negative elapsed time is invalid. Time/cancellation checks occur between
    // frames and do not preempt user handlers or imply hard-real-time bounds.
    std::uint64_t _maxMessages{1024};
    std::chrono::nanoseconds _maxElapsed{};
  };

  struct ReplayRunReport
  {
    std::uint64_t _messages{};
    ReplayStopReason _reason{ReplayStopReason::End};
    ReplayCheckpoint _next;
  };

  using ReplayHandler = std::function<Result<ReplayDecision>(const ReplayFrame&)>;
  // Portable C++20 cancellation, including standard libraries without
  // std::stop_token. Empty means no cancellation; a callable can read an
  // application-owned atomic stop flag. It is polled between bounded waits
  // and frames and cannot preempt a running application handler.
  using CancellationCheck = std::function<bool()>;

  class ReplaySession
  {
  public:
    // Publications, deliveries, or recorded-input projections. The acceptance-attempt
    // inspection cursor is rejected rather than replaying retries as messages.
    static Result<ReplaySession> create(LogCursor input, ReplayOptions options = {});
    ReplaySession(ReplaySession&&) noexcept;
    ReplaySession& operator=(ReplaySession&&) noexcept;
    ReplaySession(const ReplaySession&) = delete;
    ReplaySession& operator=(const ReplaySession&) = delete;
    ~ReplaySession();

    // step() returns and advances one input record; save a checkpoint after
    // application processing. run() is bounded and synchronous: successful
    // Continue/Pause/Stop consumes its frame, while handler error leaves that
    // frame next. Original commit/acceptance order is never a timestamp sort.
    Result<std::optional<ReplayFrame>> step();
    Result<ReplayRunReport> run(const ReplayHandler& handler, ReplayRunLimits limits = {},
                                  const CancellationCheck& cancelled = {});
    Result<ReplayCheckpoint> checkpoint() const;
    // Restores this input/timing cursor only, never application state or side
    // effects. A handler failure may already have performed external work.
    OperationResult restore(const ReplayCheckpoint& checkpoint);

  private:
    std::shared_ptr<detail::ReplayState> _state;
  };

  // Reinjection into a separately created test Session uses ordinary transport
  // APIs. Resolve source pools with LogRecord::decode, encode with the target
  // SchemaCodec/pools, issue a NEW target PublicationId, then send/publish.
  // Preserve this provenance alongside its outcome/checkpoint; original IDs
  // and archived delivery tokens are never installed as target live identities.
  struct ReplayQueueMapping
  {
    QueueId _recordedQueue{};
    QueueId _targetQueue{};
  };

  struct ReplaySchemaMapping
  {
    // Mapping IDs does not convert units or application meaning. Ordered values
    // must fit the target schema, or an explicit application transform is needed.
    SchemaId _recordedSchema{};
    SchemaId _targetSchema{};
  };

  struct ReplayPublishingMapping
  {
    PublishingQueueSetId _recordedPublishingSet{};
    PublishingQueueSetId _targetPublishingSet{};
  };

  struct ReplayInjection
  {
    ReplayOrigin _origin;
    // Persist/retain this issued target identity before the send attempt. An
    // uncertain transport error is resolved with ordinary publication_status;
    // it is not retried by silently issuing another target identity.
    PublicationId _targetPublication{};
    PublicationOutcome _outcome;
  };
} // namespace angaria::proposed
