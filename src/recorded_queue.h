#pragma once

// Private fixed-topology transport beneath public Task admission. Established
// endpoints use cached bindings and retain no Session administrative gate.
#include "producer_log.h"
#include "recorded_progress.h"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <variant>

namespace angaria::detail::recorded
{
  struct RecordedSchemaBinding
  {
    SchemaId _schema{};
    std::uint64_t _reference{};
  };

  struct RecordedQueueOptions
  {
    ProducerLogPaths _producerPaths;
    std::filesystem::path _connectionPath;
    // Acknowledged bindings reserve separate consumer-owned progress. Empty
    // selects <connectionPath>.progress; unacknowledged bindings have no file.
    std::filesystem::path _progressPath;
    bool _acknowledged{};
    ProducerLogOptions _log;
    // Supplied by the checked, sealed catalog owner. No endpoint consults
    // Session or reconstructs this snapshot during normal communication.
    RegistrySnapshot _catalog;
    QueueId _queue{};
    SourceQueueSetId _source{};
    TaskId _consumer{};
    SchemaId _schema{};
    std::uint64_t _schemaReference{};
    std::uint64_t _topologyRevision{};
    std::uint64_t _generation{1};
    std::uint64_t _capacity{32};
    // Public admission supplies every sealed exact schema and its authoritative
    // definition reference. An empty list retains the private single-schema
    // fixture shorthand above, not a second public delivery engine.
    std::vector<RecordedSchemaBinding> _schemaBindings;
  };

  // Known unaccepted backpressure, without allocating an Error or retiring the
  // caller's ID. The caller may retry that same ID when credit becomes free.
  struct QueueCreditFull
  {
  };
  using RecordedSendResult = std::variant<ProducerAppendResult, QueueCreditFull, Error>;

  enum class RecordedQueueCheckpoint
  {
    CommittedBeforeHint,
    DescriptorBeforeStamp,
    DescriptorPublished,
    DeliveryBeforeCredit,
    DeliveryAfterCredit,
    AcknowledgmentBeforeHint,
    AcknowledgmentAfterHint
  };

  // Qualification only; ordinary opens have no observer. Callbacks must not
  // reenter endpoints or mutate mappings. The inherited log seam lets process
  // tests stop actual appends without a second production send implementation.
  class RecordedQueueObserver : public ProducerLogObserver
  {
  public:
    OperationResult checkpoint(ProducerLogCheckpoint) override
    {
      return std::monostate{};
    }
    virtual OperationResult checkpoint(RecordedQueueCheckpoint point) = 0;
  };

  // Offline bootstrap, after catalog validation and before publishing paths.
  // Public admission creates a Task log once, then creates each connection.
  // create_recorded_queue is a convenience for private one-connection fixtures.
  OperationResult create_recorded_queue(const RecordedQueueOptions& options);
  OperationResult create_recorded_connection(const RecordedQueueOptions& options);

  class RecordedProducer
  {
  public:
    RecordedProducer() noexcept;
    ~RecordedProducer();
    RecordedProducer(RecordedProducer&&) noexcept;
    RecordedProducer& operator=(RecordedProducer&&) noexcept;
    RecordedProducer(const RecordedProducer&) = delete;
    RecordedProducer& operator=(const RecordedProducer&) = delete;

    static Result<RecordedProducer> open(const RecordedQueueOptions& options,
                                          RecordedQueueObserver* observer = nullptr);
    static Result<RecordedProducer> open_shared(const RecordedQueueOptions& options,
        std::shared_ptr<ProducerLog> log, RecordedQueueObserver* observer = nullptr);
    Result<TaskClaim> claim() const;
    Result<PublicationId> next_publication_id();
    RecordedSendResult try_send(PublicationId publication, const MessageView& message);
    Result<std::optional<ProducerAppendResult>> lookup(PublicationId publication) const;
    // A possibly lagging committed processing watermark; never inferred from
    // transport credit. Exact latest receipt inspection belongs to its owner.
    Result<std::uint64_t> acknowledged_sequence() const;

  private:
    struct State;
    explicit RecordedProducer(std::unique_ptr<State> state) noexcept;
    std::unique_ptr<State> _state;
  };

  class RecordedConsumer
  {
  public:
    RecordedConsumer() noexcept;
    ~RecordedConsumer();
    RecordedConsumer(RecordedConsumer&&) noexcept;
    RecordedConsumer& operator=(RecordedConsumer&&) noexcept;
    RecordedConsumer(const RecordedConsumer&) = delete;
    RecordedConsumer& operator=(const RecordedConsumer&) = delete;

    static Result<RecordedConsumer> open(const RecordedQueueOptions& options,
                                          RecordedQueueObserver* observer = nullptr,
                                          std::optional<TaskClaim> actor = {});
    Result<TaskClaim> claim() const;
    // Nonblocking. A call ends the preceding borrow, including timeout/error.
    // Received views retain exact schema decoding metadata; copy_message()
    // preserves their complete observed headers and bytes beyond this handle.
    Result<std::optional<MessageView>> try_receive();

  private:
    struct State;
    explicit RecordedConsumer(std::unique_ptr<State> state) noexcept;
    std::unique_ptr<State> _state;
  };

  class RecordedAcknowledgedConsumer
  {
  public:
    RecordedAcknowledgedConsumer() noexcept;
    ~RecordedAcknowledgedConsumer();
    RecordedAcknowledgedConsumer(RecordedAcknowledgedConsumer&&) noexcept;
    RecordedAcknowledgedConsumer& operator=(RecordedAcknowledgedConsumer&&) noexcept;
    RecordedAcknowledgedConsumer(const RecordedAcknowledgedConsumer&) = delete;
    RecordedAcknowledgedConsumer& operator=(const RecordedAcknowledgedConsumer&) = delete;

    static Result<RecordedAcknowledgedConsumer> open(const RecordedQueueOptions& options,
        RecordedQueueObserver* observer = nullptr, ProgressObserver* progressObserver = nullptr,
        std::optional<TaskClaim> actor = {});
    Result<TaskClaim> claim() const;
    // Timestamp overrides are private qualification inputs; an absent value
    // samples the runtime UTC clock. Existing pins retain their original time.
    Result<std::optional<Delivery>> try_receive(std::optional<UtcTimestamp> dequeuedAt = {});
    OperationResult acknowledge(DeliveryToken token, std::optional<UtcTimestamp> acknowledgedAt = {});
    OperationResult release(DeliveryToken token);
    OperationResult close();
    Result<ProgressSnapshot> progress() const;
    Result<ProgressReceipt> receipt(std::uint64_t sequence);

  private:
    struct State;
    explicit RecordedAcknowledgedConsumer(std::unique_ptr<State> state) noexcept;
    std::unique_ptr<State> _state;
  };
} // namespace angaria::detail::recorded
