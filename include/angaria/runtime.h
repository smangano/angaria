#pragma once

#include "angaria/message.h"
#include "angaria/registry.h"
#include "angaria/results.h"

#include <chrono>
#include <filesystem>
#include <memory>
#include <span>
#include <string_view>
#include <variant>

namespace angaria
{
  namespace detail
  {
    class TaskState;
    class QueueWriterState;
    class SourceQueueSetState;
    class PublishingQueueSetState;
  } // namespace detail

  struct CreatedSession;

  using ReceiveResult = std::variant<DeliveryView, DeliveryGap, ReceiveTimeout, Error>;

  class QueueWriter
  {
  public:
    QueueWriter(QueueWriter&&) noexcept = default;
    QueueWriter& operator=(QueueWriter&&) noexcept = default;
    QueueWriter(const QueueWriter&) = delete;
    QueueWriter& operator=(const QueueWriter&) = delete;

    QueueId id() const;
    PublicationReport send(PublicationId publication, MessageView message,
                           std::span<DestinationOutcome> outcomes);

  private:
    friend class Task;
    explicit QueueWriter(std::shared_ptr<detail::QueueWriterState> state);
    std::shared_ptr<detail::QueueWriterState> _state;
  };

  class SourceQueueSet
  {
  public:
    SourceQueueSet(SourceQueueSet&&) noexcept = default;
    SourceQueueSet& operator=(SourceQueueSet&&) noexcept = default;
    SourceQueueSet(const SourceQueueSet&) = delete;
    SourceQueueSet& operator=(const SourceQueueSet&) = delete;

    SourceQueueSetId id() const;
    ReceiveResult receive(std::chrono::nanoseconds timeout = std::chrono::nanoseconds{0});
    OperationResult acknowledge(const DeliveryToken& token);
    OperationResult release(const DeliveryToken& token);

  private:
    friend class Task;
    explicit SourceQueueSet(std::shared_ptr<detail::SourceQueueSetState> state);
    std::shared_ptr<detail::SourceQueueSetState> _state;
  };

  class PublishingQueueSet
  {
  public:
    PublishingQueueSet(PublishingQueueSet&&) noexcept = default;
    PublishingQueueSet& operator=(PublishingQueueSet&&) noexcept = default;
    PublishingQueueSet(const PublishingQueueSet&) = delete;
    PublishingQueueSet& operator=(const PublishingQueueSet&) = delete;

    PublishingQueueSetId id() const;
    PublicationReport publish(PublicationId publication, MessageView message,
                              std::span<DestinationOutcome> outcomes);
    PublicationReport retry(PublicationId publication, std::span<const QueueId> destinations,
                            std::span<DestinationOutcome> outcomes);

  private:
    friend class Task;
    explicit PublishingQueueSet(std::shared_ptr<detail::PublishingQueueSetState> state);
    std::shared_ptr<detail::PublishingQueueSetState> _state;
  };

  class Task
  {
  public:
    Task(Task&&) noexcept = default;
    Task& operator=(Task&&) noexcept = default;
    Task(const Task&) = delete;
    Task& operator=(const Task&) = delete;

    TaskClaim claim() const;
    Registry registry() const;
    Result<QueueWriter> open_writer(QueueId queue);
    Result<SourceQueueSet> open_source_set(SourceQueueSetId source);
    Result<PublishingQueueSet> open_publishing_set(PublishingQueueSetId publisher);
    Result<PublicationId> next_publication_id();
    PublicationReport publication_status(PublicationId publication, std::span<DestinationOutcome> outcomes);

  private:
    friend class Session;
    explicit Task(std::shared_ptr<detail::TaskState> state);
    std::shared_ptr<detail::TaskState> _state;
  };

  class Session
  {
  public:
    Session(Session&&) noexcept = default;
    Session& operator=(Session&&) noexcept = default;
    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;

    static Result<CreatedSession> create(const SessionOptions& options, const TaskDescriptor& creator);
    static Result<CreatedSession> create_from_file(const std::filesystem::path& configuration,
                                                   std::string_view creator);
    static Result<Session> attach(const std::filesystem::path& directory);
    static Result<Session> attach_from_file(const std::filesystem::path& configuration);

    SessionId identity() const;
    Result<SessionMetadata> metadata() const;
    Result<SessionOptions> options() const;
    Registry registry() const;
    Result<Task> attach_task(TaskId task);

  private:
    explicit Session(std::shared_ptr<detail::SessionState> state);
    std::shared_ptr<detail::SessionState> _state;
  };
  struct CreatedSession
  {
    Session _session;
    Task _creator;
  };
} // namespace angaria
