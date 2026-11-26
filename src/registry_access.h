#pragma once

#include "runtime_state.h"

#include <algorithm>
#include <memory>
#include <vector>

namespace angaria::detail
{
  // Required-actor operations must validate the Task before dereferencing its
  // Session, including moved-from handles and inherited-process use.
  inline RuntimeAccess task_access(const std::shared_ptr<TaskState>& actor)
  {
    runtime_check_task_owner(actor);
    RuntimeAccess access(actor->_session);
    runtime_check_task(access, actor);
    return access;
  }

  // Actor-bound reads must reject inherited/wrong-thread ownership before
  // entering the local gate, then check the current claim under that gate.
  inline RuntimeAccess registry_access(const std::shared_ptr<SessionState>& session,
                                         const std::shared_ptr<TaskState>& actor)
  {
    if (actor)
    {
      runtime_check_task_owner(actor);
    }
    RuntimeAccess access(session);
    if (actor)
    {
      runtime_check_task(access, actor);
    }
    return access;
  }

  // Registration uses the same native-first ordering, but requires an actor.
  // The caller supplies its operation-specific read-only diagnostic.
  inline void check_registry_actor(const std::shared_ptr<SessionState>& session,
                                    const std::shared_ptr<TaskState>& actor,
                                    const char* readOnlyMessage)
  {
    if (!session || !session->_controller)
    {
      runtime_fail(ErrorCode::InvalidState, "session", "Registry has no live Session state");
    }
    runtime_value(session->_controller->check_owner());
    if (!actor)
    {
      runtime_fail(ErrorCode::ReadOnlyRegistry, "registry", readOnlyMessage);
    }
    runtime_check_task_owner(actor);
  }

  // Common lookup for the typed topology definitions. Diagnostic wording stays
  // selectable by callers that distinguish reading, editing, and endpoints.
  template <typename Id, typename Loaded>
  const Loaded& require_registered_entity(const std::vector<Loaded>& entries, Id id, const char* path,
                                           const char* invalidMessage = "Entity identity is zero",
                                           const char* notFoundMessage = "Entity is not registered with this kind")
  {
    if (id == Id::Invalid)
    {
      runtime_fail(ErrorCode::InvalidArgument, path, invalidMessage);
    }
    const auto found = std::find_if(entries.begin(), entries.end(), [id](const auto& entry)
    {
      return entry._definition._id == id;
    });
    if (found == entries.end())
    {
      runtime_fail(ErrorCode::NotFound, path, notFoundMessage);
    }
    return *found;
  }
} // namespace angaria::detail
