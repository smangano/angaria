#include "public_runtime.h"
#include "registry_access.h"
#include "source_runtime_state.h"

#include <memory>
#include <mutex>
#include <stdexcept>
#include <utility>
#include <variant>

namespace angaria::detail
{
  class QueueWriterState
  {
  public:
    std::shared_ptr<TaskState> _task;
    QueueId _id{};
    recorded::RecordedProducer _producer;
    std::uint64_t _revision{};
  };

  class PublishingQueueSetState
  {
  public:
    std::shared_ptr<TaskState> _task;
    PublishingQueueSetId _id{};
  };

  namespace
  {
    template <typename State>
    auto endpoint_id(const std::shared_ptr<State>& state)
    {
      try
      {
        runtime_check_messaging_owner(state ? state->_task : nullptr);
        return state->_id;
      }
      catch (...)
      {
        throw std::logic_error(runtime_caught_error()._message);
      }
    }

    PublicationReport publication_error(PublicationId publication, ErrorCode code)
    {
      const auto status = code == ErrorCode::OutcomeUnknown || code == ErrorCode::IoError ?
          PublicationStatus::OutcomeUnknown : PublicationStatus::Rejected;
      return {publication, status, 0, {}, 0, code};
    }

    Result<PublicationOutcome> own_report(const PublicationReport& report, std::vector<DestinationOutcome> outcomes)
    {
      if (report._error != ErrorCode::None)
      {
        return Error{report._error, "Publication operation did not establish an outcome", "publication"};
      }
      outcomes.resize(report._destinations.size());
      const auto status = report._status == PublicationStatus::Accepted ? AcceptanceStatus::Accepted :
          report._status == PublicationStatus::Partial ? AcceptanceStatus::Partial : AcceptanceStatus::Rejected;
      return PublicationOutcome{report._publication, status, report._topologyRevision, std::move(outcomes)};
    }

    PublicationReport send(const std::shared_ptr<QueueWriterState>& state, PublicationId publication,
                            const MessageView& message, std::span<DestinationOutcome> outcomes)
    {
      // Both public overloads validate ownership before allocation/local locks.
      if (outcomes.empty())
      {
        return {publication, PublicationStatus::OutcomeUnknown, state->_revision, {}, 1, ErrorCode::OutputBufferTooSmall};
      }
      std::lock_guard lock(state->_task->_producerMutex);
      auto result = state->_producer.try_send(publication, message);
      if (const auto* error = std::get_if<Error>(&result))
      {
        return publication_error(publication, error->_code);
      }
      const auto full = std::holds_alternative<recorded::QueueCreditFull>(result);
      outcomes[0] = {state->_id, full ? DestinationStatus::Rejected : DestinationStatus::Accepted,
                    full ? ErrorCode::QueueFull : ErrorCode::None,
                    full ? std::optional<std::uint64_t>{} : std::get<recorded::ProducerAppendResult>(result)._queueSequence};
      return {publication, full ? PublicationStatus::Rejected : PublicationStatus::Accepted,
              state->_revision, outcomes.first(1), 1, ErrorCode::None};
    }
  }
}

namespace angaria
{
  QueueWriter::QueueWriter(std::shared_ptr<detail::QueueWriterState> state) : _state(std::move(state))
  {
  }

  SourceQueueSet::SourceQueueSet(std::shared_ptr<detail::SourceQueueSetState> state) : _state(std::move(state))
  {
  }

  PublishingQueueSet::PublishingQueueSet(std::shared_ptr<detail::PublishingQueueSetState> state) : _state(std::move(state))
  {
  }

  QueueId QueueWriter::id() const
  {
    return detail::endpoint_id(_state);
  }

  Result<std::uint64_t> QueueWriter::acknowledged_sequence() const
  {
    try
    {
      detail::runtime_check_messaging_owner(_state ? _state->_task : nullptr);
      std::lock_guard lock(_state->_task->_producerMutex);
      return _state->_producer.acknowledged_sequence();
    }
    catch (...)
    {
      return detail::runtime_caught_error();
    }
  }

  SourceQueueSetId SourceQueueSet::id() const
  {
    try
    {
      const auto task = detail::runtime_source_task(_state);
      return _state->_id;
    }
    catch (...)
    {
      throw std::logic_error(detail::runtime_caught_error()._message);
    }
  }

  PublishingQueueSetId PublishingQueueSet::id() const
  {
    return detail::endpoint_id(_state);
  }

  Result<PublicationOutcome> QueueWriter::send(PublicationId publication, MessageView message)
  {
    try
    {
      detail::runtime_check_messaging_owner(_state ? _state->_task : nullptr);
      std::vector<DestinationOutcome> outcomes(1);
      const auto report = detail::send(_state, publication, message, outcomes);
      return detail::own_report(report, std::move(outcomes));
    }
    catch (...)
    {
      return detail::runtime_caught_error();
    }
  }

  PublicationReport QueueWriter::send(PublicationId publication, MessageView message, std::span<DestinationOutcome> outcomes)
  {
    try
    {
      detail::runtime_check_messaging_owner(_state ? _state->_task : nullptr);
      return detail::send(_state, publication, message, outcomes);
    }
    catch (...)
    {
      return detail::publication_error(publication, detail::runtime_caught_error()._code);
    }
  }

  Result<PublicationOutcome> Task::publication_status(PublicationId publication)
  {
    try
    {
      detail::runtime_check_messaging_owner(_state);
      std::vector<DestinationOutcome> outcomes(1);
      std::lock_guard lock(_state->_producerMutex);
      const auto report = detail::runtime_recorded_publication_status(_state, publication, outcomes);
      return detail::own_report(report, std::move(outcomes));
    }
    catch (...)
    {
      return detail::runtime_caught_error();
    }
  }

  PublicationReport Task::publication_status(PublicationId publication, std::span<DestinationOutcome> outcomes)
  {
    try
    {
      detail::runtime_check_messaging_owner(_state);
      std::lock_guard lock(_state->_producerMutex);
      return detail::runtime_recorded_publication_status(_state, publication, outcomes);
    }
    catch (...)
    {
      return detail::publication_error(publication, detail::runtime_caught_error()._code);
    }
  }

  Result<QueueWriter> Task::open_writer(std::string_view name)
  {
    try
    {
      detail::runtime_check_task_owner(_state);
      return open_writer(detail::runtime_value(Registry{_state->_session, _state}.find_queue(name)));
    }
    catch (...)
    {
      return detail::runtime_caught_error();
    }
  }

  Result<SourceQueueSet> Task::open_source_set(std::string_view name)
  {
    try
    {
      detail::runtime_check_task_owner(_state);
      return open_source_set(detail::runtime_value(Registry{_state->_session, _state}.find_source_set(name)));
    }
    catch (...)
    {
      return detail::runtime_caught_error();
    }
  }

  Result<PublishingQueueSet> Task::open_publishing_set(std::string_view name)
  {
    try
    {
      detail::runtime_check_task_owner(_state);
      return open_publishing_set(detail::runtime_value(Registry{_state->_session, _state}.find_publishing_set(name)));
    }
    catch (...)
    {
      return detail::runtime_caught_error();
    }
  }

  Result<QueueWriter> Task::open_writer(QueueId queue)
  {
    try
    {
      detail::runtime_check_messaging_owner(_state);
      std::lock_guard lock(_state->_producerMutex);
      const auto resources = detail::runtime_task_resources(_state, queue);
      const auto found = resources->_queues.find(queue);
      if (found == resources->_queues.end())
      {
        detail::runtime_fail(ErrorCode::NotActive, "writer.queue", "Queue is not an admitted Active connection");
      }
      if (found->second._log._header._producer != _state->_claim._task)
      {
        detail::runtime_fail(ErrorCode::WrongOwner, "writer.queue", "Task does not own this outgoing Queue");
      }
      auto state = std::make_shared<detail::QueueWriterState>();
      state->_task = _state;
      state->_id = queue;
      state->_revision = found->second._topologyRevision;
      state->_producer = detail::runtime_value(detail::recorded::RecordedProducer::open_shared(
          found->second, detail::runtime_producer_log(_state)));
      return QueueWriter{std::move(state)};
    }
    catch (...)
    {
      return detail::runtime_caught_error();
    }
  }

  Result<SourceQueueSet> Task::open_source_set(SourceQueueSetId source)
  {
    try
    {
      detail::runtime_check_messaging_owner(_state);
      const auto resources = detail::runtime_task_resources(_state, {}, source);
      auto found = resources->_queues.end();
      for (auto it = resources->_queues.begin(); it != resources->_queues.end(); ++it)
      {
        if (it->second._source == source)
        {
          if (found != resources->_queues.end())
          {
            detail::runtime_fail(ErrorCode::UnsupportedPolicy, "source.members", "Multiple-input selection is a later milestone");
          }
          found = it;
        }
      }
      if (found == resources->_queues.end())
      {
        detail::runtime_fail(ErrorCode::NotActive, "source.members", "Source requires one admitted Active input Queue");
      }
      if (found->second._consumer != _state->_claim._task)
      {
        detail::runtime_fail(ErrorCode::WrongOwner, "source.owner", "Task does not own this Source");
      }
      auto state = std::make_shared<detail::SourceQueueSetState>();
      state->_task = _state;
      state->_process = _state->_process;
      state->_ownerThread = _state->_thread;
      state->_threadAffine = _state->_kind == TaskKind::Thread;
      state->_id = source;
      state->_reservation = std::make_shared<detail::SourceReservation>();
      state->_reservation->_id = source;
      state->_reservation->_claim = _state->_claim;
      std::lock_guard lock(_state->_endpointMutex);
      if (!_state->_openSources.emplace(source, state->_reservation).second)
      {
        detail::runtime_fail(ErrorCode::OwnershipBusy, "source", "Source already has an open handle in this Task claim");
      }
      state->_registered = true;
      if (found->second._acknowledged)
      {
        state->_consumer = detail::runtime_value(detail::recorded::RecordedAcknowledgedConsumer::open(
            found->second, nullptr, nullptr, _state->_claim));
      }
      else
      {
        state->_consumer = detail::runtime_value(detail::recorded::RecordedConsumer::open(found->second, nullptr, _state->_claim));
      }
      return SourceQueueSet{std::move(state)};
    }
    catch (...)
    {
      return detail::runtime_caught_error();
    }
  }

  Result<PublishingQueueSet> Task::open_publishing_set(PublishingQueueSetId)
  {
    return Error{ErrorCode::UnsupportedPolicy, "Publishing is a later milestone", "publishing"};
  }

  PublicationReport PublishingQueueSet::publish(PublicationId publication, MessageView, std::span<DestinationOutcome>)
  {
    return detail::publication_error(publication, ErrorCode::UnsupportedPolicy);
  }

  PublicationReport PublishingQueueSet::retry(PublicationId publication, std::span<const QueueId>, std::span<DestinationOutcome>)
  {
    return detail::publication_error(publication, ErrorCode::UnsupportedPolicy);
  }
}
