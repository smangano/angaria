#pragma once

#include "angaria/runtime.h"
#include "mutation_journal.h"
#include "session_metadata.h"
#include "schema_metadata.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <set>
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
    std::mutex _endpointMutex;
    std::set<SourceQueueSetId> _openSources;
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
    explicit RuntimeAccess(std::shared_ptr<SessionState> session, bool requireReady = true,
                           mapped::JournalFaultInjector* injector = nullptr);
    std::vector<std::uint64_t> read_words(std::uint64_t offset, std::size_t count) const;
    std::vector<std::byte> read_bytes(std::uint64_t offset, std::uint64_t length) const;
    const mapped::RecordEnvelope& record_envelope(std::uint64_t reference, std::uint16_t kind) const;
    std::vector<std::byte> immutable_record(std::uint64_t reference, std::uint16_t kind) const;
    std::vector<std::uint64_t> record_references(std::uint16_t kind) const;
    std::uint64_t section_offset(mapped::SectionKind kind) const;

    std::shared_ptr<SessionState> _session;
    mapped::MutationGuard _guard;
    mapped::MutationJournal _journal;
    mapped::JournalSnapshot _snapshot;
    mapped::NativeFile* _file{};

  private:
    struct RecordBoundary
    {
      std::uint64_t _offset{};
      mapped::RecordEnvelope _envelope;
    };

    void index_records();
    std::vector<RecordBoundary> _recordBoundaries;
  };

  struct LoadedTask
  {
    mapped::TaskDefinition _definition;
    std::uint64_t _reference{};
    std::uint64_t _stateOffset{};
  };

  struct LoadedField
  {
    mapped::FieldDefinition _definition;
    std::uint64_t _reference{};
    std::uint64_t _indexOffset{};
  };

  struct LoadedSchema
  {
    mapped::SchemaDefinition _definition;
    std::uint64_t _reference{};
    std::uint64_t _indexOffset{};
  };

  std::vector<LoadedTask> runtime_tasks(RuntimeAccess& access);
  void runtime_validate_task_state(std::span<const std::uint64_t> words, TaskKind kind);
  LoadedTask runtime_task(RuntimeAccess& access, TaskId task);
  std::vector<LoadedField> runtime_fields(RuntimeAccess& access);
  std::vector<LoadedSchema> runtime_schemas(RuntimeAccess& access);
  // Reuse Fields already validated under this access's held mutation gate.
  std::vector<LoadedSchema> runtime_schemas(RuntimeAccess& access, std::span<const LoadedField> fields);
  LoadedField runtime_field(RuntimeAccess& access, FieldId field);
  LoadedSchema runtime_schema(RuntimeAccess& access, SchemaId schema);
  void runtime_validate_field_policy(const FieldDescriptor& descriptor, ErrorCode invalidCode);
  mapped::SessionDefinition runtime_session_definition(RuntimeAccess& access);
  void runtime_validate_options(const SessionOptions& options, ErrorCode invalidCode);
  void runtime_check_task(const std::shared_ptr<TaskState>& task);
  void runtime_check_task_owner(const std::shared_ptr<TaskState>& task);
  void runtime_check_task(RuntimeAccess& access, const std::shared_ptr<TaskState>& task);

  // Private qualification seam. The public attachment path has no observer;
  // tests can pause after native acquisition while the mutation gate is held.
  class TaskAttachmentObserver
  {
  public:
    virtual ~TaskAttachmentObserver() = default;
    virtual OperationResult after_native_claim() = 0;
  };

  // Private qualification seam for stable coordination setup before journal
  // preparation. Public registration never supplies an observer or injector.
  class TaskRegistrationObserver
  {
  public:
    virtual ~TaskRegistrationObserver() = default;
    virtual OperationResult claim_file_ready(TaskId task) = 0;
  };

  Result<TaskId> runtime_register_task(std::shared_ptr<SessionState> session,
                                       std::shared_ptr<TaskState> actor,
                                       const TaskDescriptor& descriptor,
                                       mapped::JournalFaultInjector* injector = nullptr,
                                       TaskRegistrationObserver* observer = nullptr);
  Result<FieldId> runtime_register_field(std::shared_ptr<SessionState> session,
                                         std::shared_ptr<TaskState> actor,
                                         const FieldDescriptor& descriptor,
                                         mapped::JournalFaultInjector* injector = nullptr);
  Result<SchemaId> runtime_register_message(std::shared_ptr<SessionState> session,
                                            std::shared_ptr<TaskState> actor,
                                            const MessageDescriptor& descriptor,
                                            mapped::JournalFaultInjector* injector = nullptr);

  Result<std::shared_ptr<TaskState>> attach_runtime_task(std::shared_ptr<SessionState> session,
                                                        TaskId task, bool bootstrap = false,
                                                        std::shared_ptr<mapped::TaskLease> bootstrapLease = {},
                                                        TaskAttachmentObserver* observer = nullptr);
  Result<PublicationId> runtime_next_publication_id(const std::shared_ptr<TaskState>& task);
  Result<TaskRuntimeInfo> runtime_task_status(RuntimeAccess& access, TaskId task);
} // namespace angaria::detail
