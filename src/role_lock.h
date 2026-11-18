#pragma once

#include "angaria/session.h"

#include <filesystem>
#include <mutex>

#include <sys/types.h>

namespace angaria::detail
{
  enum class Role
  {
    Producer,
    Consumer
  };

  class RoleLock
  {
  public:
    RoleLock(const std::filesystem::path& session, Role role);
    ~RoleLock();
    RoleLock(const RoleLock&) = delete;
    RoleLock& operator=(const RoleLock&) = delete;

    void activate();
    RoleIdentity identity() const;
    void check_owner() const;

  private:
    static void before_fork();
    static void after_fork_parent();
    static void after_fork_child();

    static std::mutex _registryMutex;
    static RoleLock* _registryHead;

    std::filesystem::path _statePath;
    RoleIdentity _identity;
    pid_t _ownerPid;
    int _fd{-1};
    RoleLock* _next{};
  };
} // namespace angaria::detail
