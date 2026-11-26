#include "topology_state.h"
#include "registry_access.h"
#include "publication_runtime.h"

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
  };

  class PublishingQueueSetState
  {
  public:
    std::shared_ptr<TaskState> _task;
    PublishingQueueSetId _id{};
  };

  class SourceQueueSetState
  {
  public:
    ~SourceQueueSetState()
    {
      // No delivery has been issued in this milestone. Closing releases only
      // the claim-local exclusive opening; future receive adds pin cleanup.
      // Inherited children must not acquire a process-local mutex after fork.
      if (_registered && _task && _task->_session && _task->_session->_controller &&
          !std::holds_alternative<Error>(_task->_session->_controller->check_owner()))
      {
        std::lock_guard lock(_task->_endpointMutex);
        _task->_openSources.erase(_id);
      }
    }

    std::shared_ptr<TaskState> _task;
    SourceQueueSetId _id{};
    bool _registered{};
  };

  namespace
  {
    template <typename Loaded, typename Id>
    const Loaded& find_endpoint(const std::vector<Loaded>& entries, Id id)
    {
      return require_registered_entity(entries, id, "endpoint.id", "Endpoint identity must be nonzero",
                                         "Endpoint identity is not registered with this kind");
    }

    template <typename Loaded>
    void check_endpoint(const Loaded& loaded, TaskId owner, const std::shared_ptr<TaskState>& task)
    {
      if (loaded._definition._state != EntityState::Active)
      {
        runtime_fail(ErrorCode::NotActive, "endpoint.state", "Endpoint requires an Active entity");
      }
      if (owner != task->_claim._task)
      {
        runtime_fail(ErrorCode::WrongOwner, "endpoint.owner", "Acting Task does not own this endpoint");
      }
    }

    template <typename State>
    auto endpoint_id(const std::shared_ptr<State>& state)
    {
      try
      {
        runtime_check_task(state ? state->_task : nullptr);
        return state->_id;
      }
      catch (...)
      {
        throw std::logic_error(runtime_caught_error()._message);
      }
    }

    PublicationReport publication_error(PublicationId publication, ErrorCode code)
    {
      PublicationReport report;
      report._publication = publication;
      report._status = code == ErrorCode::OutcomeUnknown ? PublicationStatus::OutcomeUnknown : PublicationStatus::Rejected;
      report._error = code;
      return report;
    }

    Result<PublicationOutcome> own_publication_report(const PublicationReport& report,
                                                       std::vector<DestinationOutcome> destinations)
    {
      if (report._error != ErrorCode::None)
      {
        return Error{report._error, "Publication operation could not establish a usable outcome", "publication"};
      }
      AcceptanceStatus status;
      switch (report._status)
      {
      case PublicationStatus::Accepted:
        status = AcceptanceStatus::Accepted;
        break;
      case PublicationStatus::Partial:
        status = AcceptanceStatus::Partial;
        break;
      case PublicationStatus::Rejected:
        status = AcceptanceStatus::Rejected;
        break;
      case PublicationStatus::OutcomeUnknown:
      default:
        return Error{ErrorCode::OutcomeUnknown, "Publication outcome requires recovery and lookup", "publication"};
      }
      destinations.resize(report._destinations.size());
      return PublicationOutcome{report._publication, status, report._topologyRevision, std::move(destinations)};
    }
  } // namespace
} // namespace angaria::detail

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

  SourceQueueSetId SourceQueueSet::id() const
  {
    return detail::endpoint_id(_state);
  }

  PublishingQueueSetId PublishingQueueSet::id() const
  {
    return detail::endpoint_id(_state);
  }

  Result<PublicationOutcome> QueueWriter::send(PublicationId publication, MessageView message)
  {
    try
    {
      auto access = detail::task_access(_state ? _state->_task : nullptr);
      // Reserve the complete owning result before acceptance can commit. Moving
      // it out after the journal succeeds requires no new destination storage.
      std::vector<DestinationOutcome> outcomes(1);
      const auto report = detail::runtime_send(access, _state->_task->_claim, _state->_id,
                                                publication, message, outcomes);
      return detail::own_publication_report(report, std::move(outcomes));
    }
    catch (...)
    {
      return detail::runtime_caught_error();
    }
  }

  PublicationReport QueueWriter::send(PublicationId publication, MessageView message,
                                        std::span<DestinationOutcome> outcomes)
  {
    try
    {
      auto access = detail::task_access(_state ? _state->_task : nullptr);
      return detail::runtime_send(access, _state->_task->_claim, _state->_id, publication, message, outcomes);
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
      auto access = detail::task_access(_state);
      auto report = detail::runtime_publication_status(access, _state->_claim, publication, {});
      std::vector<DestinationOutcome> outcomes;
      if (report._error == ErrorCode::OutputBufferTooSmall)
      {
        // Keep the same recovered snapshot and gate while sizing the owned
        // result. Status never creates an acceptance or changes the ID.
        outcomes.resize(report._requiredCapacity);
        report = detail::runtime_publication_status(access, _state->_claim, publication, outcomes);
      }
      return detail::own_publication_report(report, std::move(outcomes));
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
      auto access = detail::task_access(_state);
      return detail::runtime_publication_status(access, _state->_claim, publication, outcomes);
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
      auto access = detail::task_access(_state);
      const auto topology = detail::runtime_topology(access);
      const auto& loaded = detail::find_endpoint(topology._queues, queue);
      detail::check_endpoint(loaded, loaded._definition._descriptor._producer, _state);
      auto state = std::make_shared<detail::QueueWriterState>();
      state->_task = _state;
      state->_id = queue;
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
      auto access = detail::task_access(_state);
      const auto topology = detail::runtime_topology(access);
      const auto& loaded = detail::find_endpoint(topology._sources, source);
      detail::check_endpoint(loaded, loaded._definition._descriptor._owner, _state);
      auto state = std::make_shared<detail::SourceQueueSetState>();
      state->_task = _state;
      state->_id = source;
      {
        std::lock_guard lock(_state->_endpointMutex);
        if (!_state->_openSources.insert(source).second)
        {
          detail::runtime_fail(ErrorCode::OwnershipBusy, "endpoint.source", "Source already has an open handle for this Task claim");
        }
        state->_registered = true;
      }
      return SourceQueueSet{std::move(state)};
    }
    catch (...)
    {
      return detail::runtime_caught_error();
    }
  }

  Result<PublishingQueueSet> Task::open_publishing_set(PublishingQueueSetId publisher)
  {
    try
    {
      auto access = detail::task_access(_state);
      const auto topology = detail::runtime_topology(access);
      const auto& loaded = detail::find_endpoint(topology._publishing, publisher);
      detail::check_endpoint(loaded, loaded._definition._descriptor._owner, _state);
      auto state = std::make_shared<detail::PublishingQueueSetState>();
      state->_task = _state;
      state->_id = publisher;
      return PublishingQueueSet{std::move(state)};
    }
    catch (...)
    {
      return detail::runtime_caught_error();
    }
  }
} // namespace angaria
