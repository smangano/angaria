#pragma once

// Private, fixed-topology recorded/unacknowledged point-to-point endpoints.
// Public Session admission and integrated Task lifecycle follow separately.
#include "producer_log.h"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <variant>

namespace angaria::detail::recorded
{
  struct RecordedQueueOptions
  {
    ProducerLogPaths _producerPaths;
    std::filesystem::path _connectionPath;
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
    DeliveryAfterCredit
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
  // The initial profile has exactly one outgoing Queue per producer stream.
  OperationResult create_recorded_queue(const RecordedQueueOptions& options);

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
    Result<TaskClaim> claim() const;
    Result<PublicationId> next_publication_id();
    RecordedSendResult try_send(PublicationId publication, const MessageView& message);
    Result<std::optional<ProducerAppendResult>> lookup(PublicationId publication) const;

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
                                          RecordedQueueObserver* observer = nullptr);
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
} // namespace angaria::detail::recorded
