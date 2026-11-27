#pragma once

#include "mapped_superblock.h"
#include "native_file.h"

#include <filesystem>
#include <memory>

namespace angaria::detail::mapped
{
  struct SessionControllerState;
  struct MutationGuardState;
  struct TaskLeaseState;
  class SessionController;

  // Private qualification seam. Blocking callers have registered their request;
  // try callers have passed the first queued-request check. Observers can
  // coordinate other threads, but must not reenter or mutate the Session.
  class GateAcquireObserver
  {
  public:
    virtual ~GateAcquireObserver() = default;
    virtual OperationResult before_local_lock(bool nonblocking) = 0;
  };

  // A thread-affine gate owner. Keep acquisition, use, moves, and destruction
  // on the acquiring thread. It retains its controller until the gate closes.
  class MutationGuard
  {
  public:
    MutationGuard() noexcept;
    ~MutationGuard();
    MutationGuard(MutationGuard&&) noexcept;
    MutationGuard& operator=(MutationGuard&&) noexcept;
    MutationGuard(const MutationGuard&) = delete;
    MutationGuard& operator=(const MutationGuard&) = delete;

    OperationResult check_owner() const;

  private:
    friend class SessionController;
    explicit MutationGuard(std::unique_ptr<MutationGuardState> state) noexcept;
    std::unique_ptr<MutationGuardState> _state;
  };

  // Native lifetime ownership only; persisted incarnations and Task-kind/parent
  // validation are installed by the later Session/Registry implementation.
  class TaskLease
  {
  public:
    ~TaskLease();
    TaskLease(const TaskLease&) = delete;
    TaskLease& operator=(const TaskLease&) = delete;

    OperationResult check_owner() const;
    TaskId task_id() const noexcept;

  private:
    friend class SessionController;
    explicit TaskLease(std::unique_ptr<TaskLeaseState> state) noexcept;
    std::unique_ptr<TaskLeaseState> _state;
  };

  // Private coordination primitives, not a recovered or usable public Session.
  class SessionController : public std::enable_shared_from_this<SessionController>
  {
  public:
    ~SessionController();
    SessionController(const SessionController&) = delete;
    SessionController& operator=(const SessionController&) = delete;

    static Result<std::shared_ptr<SessionController>> open_existing(const std::filesystem::path& directory);
    OperationResult check_owner() const;
    SessionId session_id() const noexcept;
    FileIdentity file_identity() const noexcept;
    const SuperblockConfiguration& configuration() const noexcept;

    Result<MutationGuard> acquire_mutation(GateAcquireObserver* observer = nullptr);
    Result<MutationGuard> try_acquire_mutation(GateAcquireObserver* observer = nullptr);
    OperationResult create_task_claim_file(const MutationGuard& guard, TaskId task);
    Result<std::shared_ptr<TaskLease>> try_claim_task(TaskId task, std::shared_ptr<TaskLease> parent = {});
    // True means a live owner currently holds the claim; probing never waits
    // for the native claim lock, including when called under the mutation gate.
    Result<bool> probe_task_claim(TaskId task);
    // Borrowed owner, valid only while this controller and guard remain alive.
    Result<NativeFile*> file(const MutationGuard& guard);

  private:
    friend struct MutationGuardState;
    friend class TaskLease;
    explicit SessionController(std::unique_ptr<SessionControllerState> state) noexcept;
    Result<MutationGuard> acquire_mutation(bool nonblocking, GateAcquireObserver* observer);
    std::unique_ptr<SessionControllerState> _state;
  };
} // namespace angaria::detail::mapped
