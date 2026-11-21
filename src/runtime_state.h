#pragma once

#include "angaria/runtime.h"
#include "mutation_journal.h"
#include "session_metadata.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

namespace angaria::detail
{
  struct RuntimeFailure
  {
    Error _error;
  };

  template <typename T>
  T runtime_value(Result<T> result)
  {
    if (const auto* error = std::get_if<Error>(&result))
    {
      throw RuntimeFailure{*error};
    }
    return std::get<T>(std::move(result));
  }

  [[noreturn]] void runtime_fail(ErrorCode code, std::string path, std::string message);
  Error runtime_caught_error();
  std::int64_t runtime_now_ns();
  std::uint64_t runtime_thread_id();
  mapped::RedoCommand runtime_write_words(std::uint64_t offset, std::span<const std::uint64_t> values);

  class SessionState
  {
  public:
    std::shared_ptr<mapped::SessionController> _controller;
    std::filesystem::path _directory;
  };

  class TaskState
  {
  public:
    ~TaskState();
    std::shared_ptr<SessionState> _session;
    std::shared_ptr<mapped::TaskLease> _lease;
    std::shared_ptr<TaskState> _parent;
    TaskClaim _claim;
    TaskKind _kind{TaskKind::Process};
    std::thread::id _thread;
    std::atomic<bool> _attached{false};
  };

  inline constexpr std::uint64_t session_ready_flag = 1;
  inline constexpr std::uint64_t task_attached_flag = 1;
  inline constexpr std::uint64_t task_attachment_time_flag = 2;
  inline constexpr std::uint64_t task_detachment_time_flag = 4;

  // A recovered, gate-bound view for one public operation. Immutable record
  // reads exclude readiness words; mutable words are read atomically.
  class RuntimeAccess
  {
  public:
    explicit RuntimeAccess(std::shared_ptr<SessionState> session, bool requireReady = true);
    std::vector<std::uint64_t> read_words(std::uint64_t offset, std::size_t count) const;
    std::vector<std::byte> read_bytes(std::uint64_t offset, std::uint64_t length) const;
    std::vector<std::byte> immutable_record(std::uint64_t reference, std::uint16_t kind) const;
    std::uint64_t section_offset(mapped::SectionKind kind) const;

    std::shared_ptr<SessionState> _session;
    mapped::MutationGuard _guard;
    mapped::MutationJournal _journal;
    mapped::JournalSnapshot _snapshot;
    mapped::NativeFile* _file{};
  };

  struct LoadedTask
  {
    mapped::TaskDefinition _definition;
    std::uint64_t _reference{};
    std::uint64_t _stateOffset{};
  };

  std::vector<LoadedTask> runtime_tasks(RuntimeAccess& access);
  void runtime_validate_task_state(std::span<const std::uint64_t> words, TaskKind kind);
  LoadedTask runtime_task(RuntimeAccess& access, TaskId task);
  mapped::SessionDefinition runtime_session_definition(RuntimeAccess& access);
  void runtime_validate_options(const SessionOptions& options, ErrorCode invalidCode);
  void runtime_check_task(const std::shared_ptr<TaskState>& task);
  Result<std::shared_ptr<TaskState>> attach_runtime_task(std::shared_ptr<SessionState> session,
                                                        TaskId task, bool bootstrap = false,
                                                        std::shared_ptr<mapped::TaskLease> bootstrapLease = {});
  Result<PublicationId> runtime_next_publication_id(const std::shared_ptr<TaskState>& task);
  Result<TaskRuntimeInfo> runtime_task_status(RuntimeAccess& access, TaskId task);
} // namespace angaria::detail
