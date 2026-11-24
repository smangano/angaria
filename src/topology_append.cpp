#include "topology_append.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <utility>

namespace angaria::detail
{
  void check_actor(const std::shared_ptr<SessionState>& session,
                     const std::shared_ptr<TaskState>& actor)
  {
    if (!session || !session->_controller)
    {
      runtime_fail(ErrorCode::InvalidState, "session", "Registry has no live Session state");
    }
    runtime_value(session->_controller->check_owner());
    if (!actor)
    {
      runtime_fail(ErrorCode::ReadOnlyRegistry, "registry", "Registration requires an actor-bound Registry");
    }
    runtime_check_task_owner(actor);
  }

  void check_task(TaskId id, const TopologySnapshot& topology, const char* path)
  {
    if (id != TaskId::Invalid &&
        std::none_of(topology._tasks.begin(), topology._tasks.end(), [id](const auto& task)
        {
          return task._definition._id == id;
        }))
    {
      runtime_fail(ErrorCode::NotFound, path, "Task is not registered");
    }
  }

  std::uint64_t add_bytes(std::uint64_t left, std::uint64_t right)
  {
    const auto result = mapped::checked_add(left, right);
    if (!result || *result > mapped::max_log_bytes)
    {
      runtime_fail(ErrorCode::LogFull, "topology.definition", "Topology allocation exceeds format byte bounds");
    }
    return *result;
  }

  std::uint64_t multiply_bytes(std::uint64_t count, std::uint64_t width)
  {
    const auto result = mapped::checked_multiply(count, width);
    if (!result || *result > mapped::max_log_bytes)
    {
      runtime_fail(ErrorCode::LogFull, "topology.definition", "Topology array exceeds format byte bounds");
    }
    return *result;
  }

  std::uint64_t record_bytes(std::uint64_t bodyBytes)
  {
    const auto body = mapped::checked_align(bodyBytes, mapped::record_alignment);
    const auto required = body ? mapped::checked_add(mapped::record_envelope_bytes, *body) : std::nullopt;
    const auto total = required ? mapped::checked_align(*required, mapped::section_alignment) : std::nullopt;
    if (!total || *total > mapped::max_log_bytes)
    {
      runtime_fail(ErrorCode::LogFull, "topology.definition", "Topology record allocation exceeds format byte bounds");
    }
    return *total;
  }

  std::uint64_t append_tail(std::uint64_t tail, std::span<const std::uint64_t> bodies,
                             std::uint64_t commandBytes, std::uint64_t maxLogBytes)
  {
    for (const auto body : bodies)
    {
      tail = add_bytes(tail, record_bytes(body));
    }
    const auto manifestBody = add_bytes(add_bytes(64, multiply_bytes(bodies.size(), 8)), commandBytes);
    tail = add_bytes(tail, record_bytes(manifestBody));
    if (tail > maxLogBytes)
    {
      runtime_fail(ErrorCode::LogFull, "session.max_log_bytes", "Complete definitions, manifest, and redo exceed the log byte ceiling");
    }
    return tail;
  }

  PreparedAppend prepare_append(RuntimeAccess& access, const std::shared_ptr<TaskState>& actor,
                                   std::span<const std::uint64_t> bodies,
                                   std::uint64_t commandBytes)
  {
    if (access._snapshot._revision == std::numeric_limits<std::uint64_t>::max() ||
        access._snapshot._generation == std::numeric_limits<std::uint64_t>::max())
    {
      runtime_fail(ErrorCode::CapacityExceeded, "topology.counter", "Registry revision or operation generation is exhausted");
    }
    (void)append_tail(access._snapshot._tail, bodies, commandBytes,
                      access._session->_controller->configuration()._maxLogBytes);
    PreparedAppend prepared;
    auto offset = access._snapshot._tail;
    for (const auto body : bodies)
    {
      prepared._references.push_back(offset);
      offset = add_bytes(offset, record_bytes(body));
    }
    prepared._plan._base = access._snapshot;
    prepared._plan._kind = mapped::JournalKind::Append;
    prepared._plan._actor = actor->_claim._task;
    prepared._plan._actorIncarnation = actor->_claim._incarnation;
    prepared._plan._newRevision = access._snapshot._revision + 1;
    return prepared;
  }

  void add_index(PreparedAppend& prepared, std::uint64_t offset, std::uint16_t id, std::uint64_t reference, EntityState state)
  {
    const std::array<std::uint64_t, 4> row{id, reference, prepared._plan._newRevision, state == EntityState::Active ? 2ull : 1ull};
    prepared._plan._commands.push_back(runtime_write_words(offset, row));
  }

  void add_membership(PreparedAppend& prepared, RuntimeAccess& access,
                         std::uint64_t group, std::uint64_t slot,
                         QueueId queue, std::uint8_t priority, std::uint64_t revision)
  {
    const auto capacity = access._session->_controller->configuration()._limits._membershipsPerSet;
    const auto offset = access.section_offset(mapped::SectionKind::MembershipState) +
                        (group * capacity + slot) * mapped::membership_bytes;
    const std::array<std::uint64_t, 2> row{
        static_cast<std::uint16_t>(queue) | (std::uint64_t{priority} << 16), revision};
    prepared._plan._commands.push_back(runtime_write_words(offset, row));
  }

} // namespace angaria::detail
