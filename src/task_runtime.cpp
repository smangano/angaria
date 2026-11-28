#include "runtime_state.h"

#include "performance_diagnostics.h"

#include "topology_state.h"
#include "public_runtime.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <span>
#include <thread>
#include <utility>
#include <variant>
#include <vector>
#include <unistd.h>

namespace angaria::detail
{
  namespace
  {
    using TaskWords = std::array<std::uint64_t, 10>;

    struct TaskKey
    {
      const mapped::SessionController* _controller{};
      TaskId _task{};
    };

    struct TaskKeyLess
    {
      bool operator()(const TaskKey& left, const TaskKey& right) const noexcept
      {
        if (left._controller != right._controller)
        {
          return std::less<const mapped::SessionController*>{}(left._controller, right._controller);
        }
        return left._task < right._task;
      }
    };

    struct TaskRegistry
    {
      std::mutex _mutex;
      std::map<TaskKey, std::weak_ptr<TaskState>, TaskKeyLess> _tasks;
    };

    TaskRegistry& task_registry()
    {
      // Process lifetime avoids static destruction ordering and never owns a
      // Task. Attachment prunes expired slots, including old controllers.
      static auto* const registry = new TaskRegistry;
      return *registry;
    }

    std::shared_ptr<TaskState> find_parent(const std::shared_ptr<SessionState>& session, TaskId task)
    {
      auto& registry = task_registry();
      std::lock_guard lock(registry._mutex);
      const auto found = registry._tasks.find(TaskKey{session->_controller.get(), task});
      if (found == registry._tasks.end())
      {
        runtime_fail(ErrorCode::WrongOwner, "task.process", "Thread Task requires its live local Process Task");
      }
      auto parent = found->second.lock();
      if (!parent || !parent->_attached.load(std::memory_order_acquire) || parent->_kind != TaskKind::Process)
      {
        runtime_fail(ErrorCode::WrongOwner, "task.process", "Thread Task requires its attached Process Task");
      }
      return parent;
    }

    void reserve_task(const std::shared_ptr<TaskState>& task)
    {
      auto& registry = task_registry();
      std::lock_guard lock(registry._mutex);
      std::erase_if(registry._tasks, [](const auto& entry)
      {
        return entry.second.expired();
      });
      const TaskKey key{task->_session->_controller.get(), task->_claim._task};
      auto [found, inserted] = registry._tasks.try_emplace(key);
      if (!inserted && !found->second.expired())
      {
        runtime_fail(ErrorCode::OwnershipBusy, "task", "Task already has a local owner or pending attachment");
      }
      // The pending state is fully initialized but is not a usable parent until
      // the journal succeeds. Publication after acceptance is one atomic store.
      found->second = task;
    }

    TaskWords task_words(RuntimeAccess& access, const LoadedTask& task)
    {
      const auto words = access.read_words(task._stateOffset, 32);
      runtime_validate_task_state(words, task._definition._descriptor._kind);
      TaskWords result{};
      std::copy_n(words.begin(), result.size(), result.begin());
      return result;
    }

    void check_native(const std::shared_ptr<TaskState>& task, bool requireThread = true)
    {
      ANGARIA_DIAGNOSTIC_SCOPE(TaskCheck);
      if (!task || !task->_session || !task->_session->_controller || !task->_lease)
      {
        runtime_fail(ErrorCode::InvalidState, "task", "Task has no live ownership state");
      }
      // Inherited handles must fail before touching any process-local mutex.
      runtime_value(task->_session->_controller->check_owner());
      runtime_value(task->_lease->check_owner());
      if (!task->_attached.load(std::memory_order_acquire))
      {
        runtime_fail(ErrorCode::InvalidState, "task", "Task attachment has not completed");
      }
      if (task->_lease->task_id() != task->_claim._task ||
          task->_claim._session != task->_session->_controller->session_id())
      {
        runtime_fail(ErrorCode::WrongOwner, "task", "Task claim does not match its native owner");
      }
      if (requireThread && task->_kind == TaskKind::Thread && task->_thread != std::this_thread::get_id())
      {
        runtime_fail(ErrorCode::WrongOwner, "task", "Thread Task can be used only by its attaching thread");
      }
    }

    TaskWords check_with_access(RuntimeAccess& access, const std::shared_ptr<TaskState>& task,
                                const LoadedTask& loaded)
    {
      ANGARIA_DIAGNOSTIC_SCOPE(TaskCheck);
      const auto words = task_words(access, loaded);
      if (task->_session->_controller.get() != access._session->_controller.get() ||
          loaded._definition._id != task->_claim._task || loaded._definition._descriptor._kind != task->_kind ||
          words[0] != task->_claim._incarnation || (words[6] & task_attached_flag) == 0 ||
          words[3] != static_cast<std::uint64_t>(::getpid()) ||
          (task->_kind == TaskKind::Thread && words[4] != runtime_thread_id()))
      {
        runtime_fail(ErrorCode::WrongOwner, "task.claim", "Task no longer owns its recorded incarnation");
      }
      if (task->_kind == TaskKind::Thread)
      {
        if (!task->_parent || task->_parent->_kind != TaskKind::Process ||
            loaded._definition._descriptor._process != task->_parent->_claim._task)
        {
          runtime_fail(ErrorCode::WrongOwner, "task.process", "Thread Task has no matching retained Process Task");
        }
        check_native(task->_parent, false);
        const auto parent = runtime_task(access, task->_parent->_claim._task);
        const auto parentWords = check_with_access(access, task->_parent, parent);
        if (words[7] != parentWords[0])
        {
          runtime_fail(ErrorCode::WrongOwner, "task.process", "Thread Task belongs to an earlier Process incarnation");
        }
      }
      return words;
    }

    void require_cleanup_state(RuntimeAccess& access)
    {
      // Claims only change administrative diagnostics. Message pins/receipts
      // recover through their own consumer-owned progress files.
      (void)runtime_topology(access);
    }

    mapped::TransactionPlan fixed_plan(RuntimeAccess& access, TaskId actor, std::uint64_t incarnation)
    {
      mapped::TransactionPlan plan;
      plan._base = access._snapshot;
      plan._kind = mapped::JournalKind::FixedState;
      plan._subtype = mapped::TransactionSubtype::Registry;
      plan._actor = actor;
      plan._actorIncarnation = incarnation;
      plan._newRevision = access._snapshot._revision;
      return plan;
    }
  } // namespace

  TaskState::~TaskState()
  {
    if (!_attached.load(std::memory_order_acquire) || !_session || !_session->_controller || !_lease)
    {
      return;
    }
    try
    {
      // Fork children must not acquire inherited mutexes or change parent state.
      if (std::holds_alternative<Error>(_session->_controller->check_owner()) ||
          std::holds_alternative<Error>(_lease->check_owner()))
      {
        return;
      }
      RuntimeAccess access(_session);
      const auto loaded = runtime_task(access, _claim._task);
      const auto words = task_words(access, loaded);
      if (words[0] != _claim._incarnation || (words[6] & task_attached_flag) == 0)
      {
        return;
      }
      require_cleanup_state(access);
      auto plan = fixed_plan(access, _claim._task, _claim._incarnation);
      auto detached = words;
      detached[6] = (detached[6] & ~task_attached_flag) | task_detachment_time_flag;
      detached[9] = std::bit_cast<std::uint64_t>(runtime_now_ns());
      plan._commands.push_back(runtime_write_words(loaded._stateOffset, detached));
      runtime_value(access.execute(plan));
      _attached.store(false, std::memory_order_release);
    }
    catch (...)
    {
      // Native ownership remains authoritative. Closing the lease after this
      // destructor allows the next attacher to repair persisted diagnostics.
    }
  }

  void runtime_check_task_owner(const std::shared_ptr<TaskState>& task)
  {
    check_native(task);
  }

  void runtime_check_messaging_owner(const std::shared_ptr<TaskState>& task)
  {
    // The native lease and immutable resources were authenticated at binding.
    // Their owning handles keep them alive; online replacement is unsupported.
    // Check fork/Thread affinity before any inherited local mutex is touched.
    const auto pid = static_cast<std::uint64_t>(::getpid());
    if (!task || !task->_lease || !task->_attached.load(std::memory_order_acquire))
    {
      runtime_fail(ErrorCode::InvalidState, "task", "Task has no live attachment");
    }
    if (task->_ownerPid != pid ||
        (task->_kind == TaskKind::Thread && task->_thread != std::this_thread::get_id()))
    {
      runtime_fail(ErrorCode::WrongOwner, "task", "Task belongs to another process or thread");
    }
    if (task->_kind == TaskKind::Thread && (!task->_parent || task->_parent->_ownerPid != pid ||
        !task->_parent->_attached.load(std::memory_order_acquire)))
    {
      runtime_fail(ErrorCode::WrongOwner, "task.process", "Thread Task requires its retained Process claim");
    }
  }

  void runtime_check_task(RuntimeAccess& access, const std::shared_ptr<TaskState>& task)
  {
    check_native(task);
    if (task->_session->_controller.get() != access._session->_controller.get())
    {
      runtime_fail(ErrorCode::WrongOwner, "task.claim", "Acting Task belongs to another Session controller");
    }
    const auto loaded = runtime_task(access, task->_claim._task);
    check_with_access(access, task, loaded);
  }

  void runtime_check_task(const std::shared_ptr<TaskState>& task)
  {
    runtime_check_task_owner(task);
    RuntimeAccess access(task->_session);
    runtime_check_task(access, task);
  }

  Result<std::shared_ptr<TaskState>> attach_runtime_task(std::shared_ptr<SessionState> session,
                                                        TaskId task, bool bootstrap,
                                                        std::shared_ptr<mapped::TaskLease> bootstrapLease,
                                                        TaskAttachmentObserver* observer)
  {
    try
    {
      if (!session || !session->_controller)
      {
        runtime_fail(ErrorCode::InvalidState, "session", "Session has no live state");
      }
      if (task == TaskId::Invalid)
      {
        runtime_fail(ErrorCode::InvalidArgument, "task", "Task attachment requires a Session and nonzero identity");
      }
      runtime_value(session->_controller->check_owner());
      if (bootstrapLease && !bootstrap)
      {
        runtime_fail(ErrorCode::InvalidArgument, "task", "A preheld claim is valid only for creator bootstrap");
      }
      // Status readers use this same gate. Keep native lease acquisition and
      // incarnation installation inside one observation boundary so a busy new
      // lease can never make the predecessor's stale diagnostics appear live.
      RuntimeAccess access(session, !bootstrap);
      const auto loaded = runtime_task(access, task);
      if (bootstrap && (task != static_cast<TaskId>(1) ||
          loaded._definition._descriptor._kind != TaskKind::Process || access.read_words(112, 1)[0] != 0))
      {
        runtime_fail(ErrorCode::InvalidState, "task", "Only the creator Process Task can publish Session readiness");
      }
      std::shared_ptr<TaskState> parent;
      if (loaded._definition._descriptor._kind == TaskKind::Thread)
      {
        parent = find_parent(session, *loaded._definition._descriptor._process);
        check_native(parent, false);
      }
      // Declare temporary ownership after access: every precommit failure drops
      // its lease before the guard unlocks. The controller's local bookkeeping
      // and native claim attempts are both nonblocking while this gate is held.
      auto lease = std::move(bootstrapLease);
      if (lease)
      {
        runtime_value(lease->check_owner());
        if (lease->task_id() != task)
        {
          runtime_fail(ErrorCode::WrongOwner, "task", "Bootstrap lease does not own the creator Task");
        }
      }
      else
      {
        lease = runtime_value(session->_controller->try_claim_task(task, parent ? parent->_lease : nullptr));
      }
      if (observer)
      {
        runtime_value(observer->after_native_claim());
      }
      require_cleanup_state(access);
      auto words = task_words(access, loaded);
      if (bootstrap && (words[0] != 0 || access.read_words(112, 1)[0] != 0))
      {
        runtime_fail(ErrorCode::InvalidState, "task", "Creator bootstrap has already completed");
      }
      std::uint64_t parentIncarnation{};
      if (parent)
      {
        check_native(parent, false);
        const auto parentTask = runtime_task(access, parent->_claim._task);
        parentIncarnation = check_with_access(access, parent, parentTask)[0];
      }
      const auto previous = words[0];
      if (previous == std::numeric_limits<std::uint64_t>::max() ||
          (previous != 0 && words[2] == std::numeric_limits<std::uint64_t>::max()))
      {
        runtime_fail(ErrorCode::CapacityExceeded, "task.incarnation", "Task incarnation or restart counter is exhausted");
      }
      const auto timestamp = runtime_now_ns();
      const auto previousFlags = words[6];
      words[0] = previous + 1;
      words[1] = 0;
      words[2] = previous == 0 ? 0 : words[2] + 1;
      words[3] = static_cast<std::uint64_t>(::getpid());
      words[4] = runtime_thread_id();
      words[5] = std::bit_cast<std::uint64_t>(timestamp);
      words[6] = task_attached_flag | task_attachment_time_flag | (words[6] & task_detachment_time_flag);
      words[7] = parentIncarnation;
      words[8] = 0;
      // Reattachment ends its predecessor even if that process did not detach.
      if (previous != 0 && ((previousFlags & task_attached_flag) != 0 ||
          (previousFlags & task_detachment_time_flag) == 0))
      {
        words[6] |= task_detachment_time_flag;
        words[9] = std::bit_cast<std::uint64_t>(timestamp);
      }
      auto state = std::make_shared<TaskState>();
      state->_session = std::move(session);
      state->_lease = std::move(lease);
      state->_parent = std::move(parent);
      state->_claim = TaskClaim{state->_session->_controller->session_id(), task, words[0]};
      state->_kind = loaded._definition._descriptor._kind;
      state->_thread = std::this_thread::get_id();
      state->_ownerPid = static_cast<std::uint64_t>(::getpid());
      auto plan = fixed_plan(access, task, words[0]);
      plan._commands.push_back(runtime_write_words(loaded._stateOffset, words));
      if (bootstrap)
      {
        const std::array ready{session_ready_flag};
        plan._commands.push_back(runtime_write_words(112, ready));
      }
      reserve_task(state);
      runtime_value(access.execute(plan));
      state->_attached.store(true, std::memory_order_release);
      return state;
    }
    catch (...)
    {
      return runtime_caught_error();
    }
  }

  Result<PublicationId> runtime_next_publication_id(const std::shared_ptr<TaskState>& task)
  {
    try
    {
      runtime_check_messaging_owner(task);
      std::lock_guard lock(task->_producerMutex);
      return runtime_producer_log(task)->next_publication_id();
    }
    catch (...)
    {
      return runtime_caught_error();
    }
  }

  Result<TaskRuntimeInfo> runtime_task_status(RuntimeAccess& access, TaskId task)
  {
    try
    {
      const auto loaded = runtime_task(access, task);
      const auto words = task_words(access, loaded);
      TaskRuntimeInfo info;
      info._task = task;
      info._processId = words[3];
      info._threadId = words[4];
      info._incarnation = words[0];
      info._restartCount = words[2];
      if ((words[6] & task_attachment_time_flag) != 0)
      {
        info._lastAttachedAt = UtcTimestamp{std::chrono::nanoseconds{std::bit_cast<std::int64_t>(words[5])}};
      }
      info._attached = (words[6] & task_attached_flag) != 0 &&
                       runtime_value(access._session->_controller->probe_task_claim(task));
      if (info._attached && loaded._definition._descriptor._kind == TaskKind::Thread)
      {
        const auto parent = runtime_task(access, *loaded._definition._descriptor._process);
        const auto parentWords = task_words(access, parent);
        if (parent._definition._descriptor._kind != TaskKind::Process)
        {
          runtime_fail(ErrorCode::IoError, "task.process", "Thread Task parent is not a Process Task");
        }
        info._attached = parentWords[0] == words[7] && parentWords[3] == words[3] &&
                         (parentWords[6] & task_attached_flag) != 0 &&
                         runtime_value(access._session->_controller->probe_task_claim(parent._definition._id));
      }
      return info;
    }
    catch (...)
    {
      return runtime_caught_error();
    }
  }
} // namespace angaria::detail
