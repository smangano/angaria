#include "topology_state.h"
#include "registry_access.h"

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
