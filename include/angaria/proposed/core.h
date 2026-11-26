#pragma once

// REVIEW ONLY: proposed complete application interface. Declarations have no
// runtime definitions and intentionally live outside the supported namespace.
#include "angaria/proposed/schema.h"

#include <chrono>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace angaria::proposed
{
  class Session;
  class Task;
  class Registry;
  struct CreatedSession;
  namespace detail
  {
    class MessageRetention;
  }

  struct MessageView
  {
    SchemaId _schema{};
    PayloadFormat _format{PayloadFormat::Binary};
    std::span<const std::byte> _payload;
    std::uint32_t _flags{};
  };
  struct Message
  {
    SchemaId _schema{};
    PayloadFormat _format{PayloadFormat::Binary};
    std::vector<std::byte> _payload;
    std::uint32_t _flags{};
    SessionId schema_session() const;
    Result<std::vector<FieldValue>> decode(const SchemaCodec& codec) const;
    MessageView view() const &;
    MessageView view() const && = delete;
  private:
    friend struct DeliveryView;
    friend class LogCursor;
    // Owning copies retain source schema/pool context, not just native bytes.
    // They remain decodable after acknowledgment and later log reclamation.
    std::shared_ptr<const detail::MessageRetention> _retention;
  };

  // Processing handlers use this for both live delivery and offline replay.
  // A replay context is never a live acknowledgment token.
  struct MessageContext
  {
    PublicationId _publication{};
    UtcTimestamp _publishedAt{};
    std::optional<QueueId> _queue;
    std::optional<std::uint64_t> _queueSequence;
    std::optional<UtcTimestamp> _enqueuedAt;
    std::optional<UtcTimestamp> _dequeuedAt;
    bool _replayed{};
  };

  struct DeliveryView
  {
    DeliveryToken _token{};
    PublicationId _publication{};
    MessageView _message;
    UtcTimestamp _publishedAt{};
    UtcTimestamp _enqueuedAt{};
    UtcTimestamp _dequeuedAt{};
    MessageContext context() const;
    Result<Message> copy_message(std::uint64_t maxPayloadBytes) const;
  };

  struct DeliveryGap
  {
    QueueId _queue{};
    std::uint64_t _firstSequence{};
    std::uint64_t _lastSequence{};
  };
  struct ReceiveTimeout
  {
  };
  using ReceiveResult = std::variant<DeliveryView, DeliveryGap, ReceiveTimeout, Error>;

  enum class AcceptanceStatus
  {
    Accepted,
    Partial,
    Rejected
  };
  // Owned values contain known acceptance only. Uncertainty is reported through
  // ErrorCode::OutcomeUnknown, never a second status/error channel in this value.
  // The explicit request ID remains available even
  // if the operation returns ErrorCode::OutcomeUnknown instead of this value.
  struct PublicationOutcome
  {
    PublicationId _publication{};
    AcceptanceStatus _status{AcceptanceStatus::Rejected};
    std::uint64_t _topologyRevision{};
    std::vector<DestinationOutcome> _destinations;
  };

  struct FieldSnapshot
  {
    FieldDescriptor _descriptor;
    RegistrationMetadata _registration;
  };
  struct MessageSnapshot
  {
    MessageDescriptor _descriptor;
    RegistrationMetadata _registration;
  };
  struct RegistrySnapshot
  {
    std::uint64_t _revision{};
    std::uint64_t _currentRevision{};
    std::vector<TaskSnapshot> _tasks;
    std::vector<QueueSnapshot> _queues;
    std::vector<SourceSnapshot> _sources;
    std::vector<PublishingSnapshot> _publishing;
    std::vector<FieldSnapshot> _fields;
    std::vector<MessageSnapshot> _messages;
  };

  enum class DiagnosticSeverity
  {
    Warning,
    Error
  };
  struct Diagnostic
  {
    DiagnosticSeverity _severity{DiagnosticSeverity::Error};
    std::string _path;
    std::string _message;
  };
  struct ValidationReport
  {
    bool _valid{};
    bool _diagnosticsTruncated{};
    std::vector<Diagnostic> _diagnostics;
  };
  struct ConfigurationLimits
  {
    std::uint64_t _maxInputBytes{1048576};
    std::uint64_t _maxOutputBytes{1048576};
    std::uint32_t _maxNesting{32};
    std::uint32_t _maxDiagnostics{100};
  };
  enum class ConfigurationFormat
  {
    Toml,
    Yaml,
    Json,
    Xml
  };
  class Configuration
  {
  public:
    static Result<Configuration> load(const std::filesystem::path& tomlFile,
                                       ConfigurationLimits limits = {});
    static Result<Configuration> parse_toml(std::string_view toml,
                                            const std::filesystem::path& baseDirectory,
                                            ConfigurationLimits limits = {});
    ValidationReport validate() const;
    Result<SessionOptions> session_options() const;
    Result<std::string> to_toml() const;
  private:
    struct State;
    std::shared_ptr<State> _state;
  };
  // Converters emit TOML; every factory still uses the common TOML path.
  Result<std::string> convert_configuration(std::string_view input, ConfigurationFormat format,
                                             ConfigurationLimits limits = {});

  struct StorageStatus
  {
    std::uint64_t _allocatedBytes{};
    std::uint64_t _maxLogBytes{};
    std::uint64_t _reclaimableBytes{};
    std::uint64_t _segments{};
  };
  struct QueueStatus
  {
    QueueId _queue{};
    std::uint64_t _lastAcceptedSequence{};
    std::uint64_t _terminalSequence{};
    std::uint64_t _outstandingDeliveries{};
    std::uint64_t _inFlightDeliveries{};
    std::uint64_t _retainedFromSequence{};
  };
  struct SourceStatus
  {
    SourceQueueSetId _source{};
    std::uint64_t _consumerIncarnation{};
    std::uint64_t _issuance{};
    std::optional<DeliveryToken> _inFlight;
    std::uint64_t _selectionRound{};
    struct MemberBudget
    {
      QueueId _queue{};
      std::uint64_t _remainingCredits{};
    };
    std::vector<MemberBudget> _budgets; // Present only for Fair selection.
  };
  struct PublishingStatus
  {
    PublishingQueueSetId _publishing{};
    std::uint64_t _topologyRevision{};
  };
  struct ObservationSnapshot
  {
    UtcTimestamp _observedAt{};
    std::uint64_t _registryRevision{};
    std::uint64_t _operationGeneration{};
    StorageStatus _storage;
    std::vector<TaskRuntimeInfo> _tasks;
    std::vector<QueueStatus> _queues;
    std::vector<SourceStatus> _sources;
    std::vector<PublishingStatus> _publishing;
  };
  struct Capabilities
  {
    std::vector<StorageMapping> _storage;
    std::vector<SelectionPolicy> _selection;
    std::vector<WaitPolicy> _wait;
    std::vector<DeliveryPolicy> _delivery;
    std::vector<AcceptancePolicy> _acceptance;
    std::vector<PayloadFormat> _serializers;
    std::uint16_t _maxBitFieldBits{};
    bool _temporalFields{};
    bool _compoundFields{};
    bool _pooledFields{};
    bool _portableDebugArchives{};
    bool _executionTrace{};
  };

  struct RetentionOptions
  {
    UtcTimestamp _publishedBefore{};
    std::uint64_t _targetBytes{};
  };
  struct ReclamationSummary
  {
    std::uint64_t _eligibleBytes{};
    std::uint64_t _protectedBytes{};
  };
  class ReclamationPlan
  {
  public:
    ReclamationSummary summary() const;
  private:
    friend class Session;
    struct State;
    std::shared_ptr<State> _state;
  };
  struct ReclamationReport
  {
    std::uint64_t _reclaimedBytes{};
    std::uint64_t _remainingBytes{};
  };

  class Registry
  {
  public:
    Result<TaskId> register_task(const TaskDescriptor& descriptor);
    Result<QueueId> register_queue(const QueueDescriptor& descriptor);
    Result<SourceQueueSetId> register_source_set(const SourceQueueSetDescriptor& descriptor);
    Result<PublishingQueueSetId> register_publishing_set(const PublishingQueueSetDescriptor& descriptor);
    Result<FieldId> register_field(const FieldDescriptor& descriptor);
    Result<SchemaId> register_message(const MessageDescriptor& descriptor);
    Result<TaskId> find_task(std::string_view name) const;
    Result<QueueId> find_queue(std::string_view name) const;
    Result<SourceQueueSetId> find_source_set(std::string_view name) const;
    Result<PublishingQueueSetId> find_publishing_set(std::string_view name) const;
    Result<FieldId> find_field(std::string_view name) const;
    Result<SchemaId> find_message(std::string_view name, std::uint32_t version) const;
    Result<TaskDescriptor> describe_task(TaskId id) const;
    Result<QueueDescriptor> describe_queue(QueueId id) const;
    Result<SourceQueueSetDescriptor> describe_source_set(SourceQueueSetId id) const;
    Result<PublishingQueueSetDescriptor> describe_publishing_set(PublishingQueueSetId id) const;
    Result<FieldDescriptor> describe_field(FieldId id) const;
    Result<MessageDescriptor> describe_message(SchemaId id) const;
    Result<TaskRuntimeInfo> task_status(TaskId id) const;
    Result<std::vector<QueueId>> source_members(SourceQueueSetId id) const;
    Result<RegistrySnapshot> snapshot(std::optional<std::uint64_t> revision = std::nullopt) const;
    Result<RegistrationMetadata> metadata(TaskId id) const;
    Result<RegistrationMetadata> metadata(QueueId id) const;
    Result<RegistrationMetadata> metadata(SourceQueueSetId id) const;
    Result<RegistrationMetadata> metadata(PublishingQueueSetId id) const;
    Result<RegistrationMetadata> metadata(FieldId id) const;
    Result<RegistrationMetadata> metadata(SchemaId id) const;
    Result<EntityState> state(QueueId id) const;
    Result<EntityState> state(SourceQueueSetId id) const;
    Result<EntityState> state(PublishingQueueSetId id) const;
    OperationResult activate_source_set(SourceQueueSetId id);
    OperationResult activate_queue(QueueId id);
    OperationResult activate_publishing_set(PublishingQueueSetId id);
    OperationResult adopt_queue(QueueId id, TaskId producer);
    OperationResult adopt_source_set(SourceQueueSetId id, TaskId owner);
    OperationResult adopt_publishing_set(PublishingQueueSetId id, TaskId owner);
    OperationResult assign_source(QueueId id, SourceQueueSetId source, std::uint8_t priority);
    OperationResult set_destinations(PublishingQueueSetId id, std::span<const QueueId> destinations);
  private:
    struct State;
    std::shared_ptr<State> _state;
  };

  // Endpoints and Task/Session remain move-only owners, as in today's API.
  class QueueWriter
  {
  public:
    QueueWriter(QueueWriter&&) noexcept;
    QueueWriter& operator=(QueueWriter&&) noexcept;
    QueueWriter(const QueueWriter&) = delete;
    QueueWriter& operator=(const QueueWriter&) = delete;
    QueueId id() const;
    Result<PublicationOutcome> send(PublicationId id, MessageView message);
    PublicationReport send(PublicationId id, MessageView message, std::span<DestinationOutcome> output);
  private:
    struct State;
    std::shared_ptr<State> _state;
  };
  class SourceQueueSet
  {
  public:
    SourceQueueSet(SourceQueueSet&&) noexcept;
    SourceQueueSet& operator=(SourceQueueSet&&) noexcept;
    SourceQueueSet(const SourceQueueSet&) = delete;
    SourceQueueSet& operator=(const SourceQueueSet&) = delete;
    SourceQueueSetId id() const;
    ReceiveResult receive(std::chrono::nanoseconds timeout = std::chrono::nanoseconds{0});
    OperationResult acknowledge(const DeliveryToken& token);
    OperationResult release(const DeliveryToken& token);
    OperationResult close();
  private:
    struct State;
    std::shared_ptr<State> _state;
  };
  class PublishingQueueSet
  {
  public:
    PublishingQueueSet(PublishingQueueSet&&) noexcept;
    PublishingQueueSet& operator=(PublishingQueueSet&&) noexcept;
    PublishingQueueSet(const PublishingQueueSet&) = delete;
    PublishingQueueSet& operator=(const PublishingQueueSet&) = delete;
    PublishingQueueSetId id() const;
    Result<PublicationOutcome> publish(PublicationId id, MessageView message);
    Result<PublicationOutcome> retry(PublicationId id, std::span<const QueueId> destinations);
    PublicationReport publish(PublicationId id, MessageView message, std::span<DestinationOutcome> output);
    PublicationReport retry(PublicationId id, std::span<const QueueId> destinations,
                             std::span<DestinationOutcome> output);
  private:
    struct State;
    std::shared_ptr<State> _state;
  };
  class Task
  {
  public:
    Task(Task&&) noexcept;
    Task& operator=(Task&&) noexcept;
    Task(const Task&) = delete;
    Task& operator=(const Task&) = delete;
    TaskClaim claim() const;
    Registry registry() const;
    // Writable pools retain and authenticate this Task claim for interning.
    Result<PoolContext> pools() const;
    Result<SymbolTable> symbols() const;
    Result<StringPool> strings() const;
    Result<QueueWriter> open_writer(QueueId queue);
    Result<QueueWriter> open_writer(std::string_view name);
    Result<SourceQueueSet> open_source_set(SourceQueueSetId source);
    Result<SourceQueueSet> open_source_set(std::string_view name);
    Result<PublishingQueueSet> open_publishing_set(PublishingQueueSetId publisher);
    Result<PublishingQueueSet> open_publishing_set(std::string_view name);
    Result<PublicationId> next_publication_id();
    Result<PublicationOutcome> publication_status(PublicationId id);
    PublicationReport publication_status(PublicationId id, std::span<DestinationOutcome> output);
  private:
    struct State;
    std::shared_ptr<State> _state;
  };
  class Session
  {
  public:
    Session(Session&&) noexcept;
    Session& operator=(Session&&) noexcept;
    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;
    static Result<CreatedSession> create(const SessionOptions& options, const TaskDescriptor& creator);
    static Result<CreatedSession> create(const Configuration& configuration, std::string_view creator);
    static Result<CreatedSession> create_from_file(const std::filesystem::path& toml, std::string_view creator);
    static Result<Session> attach(const std::filesystem::path& directory);
    static Result<Session> attach_from_file(const std::filesystem::path& toml);
    SessionId identity() const;
    Result<SessionMetadata> metadata() const;
    Result<SessionOptions> options() const;
    Result<Capabilities> capabilities() const;
    Registry registry() const;
    Result<Task> attach_task(TaskId task);
    Result<Task> attach_task(std::string_view name);
    Result<ObservationSnapshot> observe() const;
    // Session-only pool access resolves values; it does not grant mutation.
    Result<PoolContext> pools() const;
    Result<SymbolTable> symbols() const;
    Result<StringPool> strings() const;
    Result<ReclamationPlan> plan_reclamation(const RetentionOptions& options);
    Result<ReclamationReport> reclaim(const ReclamationPlan& plan);
  private:
    struct State;
    std::shared_ptr<State> _state;
  };
  struct CreatedSession
  {
    Session _session;
    Task _creator;
  };
} // namespace angaria::proposed
