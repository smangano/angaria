#pragma once

#include "angaria/runtime.h"

#include "angaria/detail/error_support.h"
#include "mapped_read_window.h"
#include "mutation_journal.h"
#include "session_metadata.h"
#include "schema_metadata.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <map>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

namespace angaria::detail
{
  namespace recorded
  {
    class ProducerLog;
  }
  struct RecordedSessionResources;
  struct SourceReservation;
  struct RuntimeFailure
  {
    Error _error;
  };

  template <typename T>
  T runtime_value(Result<T> result)
  {
    return value_or_throw<RuntimeFailure>(std::move(result));
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
    std::shared_ptr<const RecordedSessionResources> _recordedResources;
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
    std::uint64_t _ownerPid{};
    // A Task is the producer serialization domain. Its writers share one log
    // and issuer; unrelated Tasks and consumer handles have independent locks.
    std::mutex _producerMutex;
    std::shared_ptr<recorded::ProducerLog> _producerLog;
    std::shared_ptr<const RecordedSessionResources> _recordedResources;
    std::mutex _endpointMutex;
    std::map<SourceQueueSetId, std::shared_ptr<SourceReservation>> _openSources;
  };

  inline constexpr std::uint64_t session_ready_flag = 1;
  inline constexpr std::uint64_t task_attached_flag = 1;
  inline constexpr std::uint64_t task_attachment_time_flag = 2;
  inline constexpr std::uint64_t task_detachment_time_flag = 4;

  // Private qualification seam for immutable read requests. Called after bounds
  // checks, before allocation/I/O, under the operation gate. It must not reenter
  // or mutate the Session. Production operations do not supply an observer.
  class RuntimeAccessReadObserver
  {
  public:
    virtual ~RuntimeAccessReadObserver() = default;
    virtual OperationResult before_read(std::uint64_t offset, std::uint64_t length) = 0;
  };

  // A recovered, gate-bound view for one public operation. Immutable record
  // reads exclude readiness words; mutable words are read atomically.
  class RuntimeAccess
  {
  public:
    explicit RuntimeAccess(std::shared_ptr<SessionState> session, bool requireReady = true,
                           mapped::JournalFaultInjector* injector = nullptr,
                           RuntimeAccessReadObserver* observer = nullptr);
    // Adopt a guard obtained by a bounded try-acquire loop. Recovery and
    // prefix validation complete before callers inspect or mutate the Session.
    RuntimeAccess(std::shared_ptr<SessionState> session, mapped::MutationGuard guard,
                  bool requireReady = true, mapped::JournalFaultInjector* injector = nullptr,
                  RuntimeAccessReadObserver* observer = nullptr);
    std::vector<std::uint64_t> read_words(std::uint64_t offset, std::size_t count) const;
    std::vector<std::byte> read_bytes(std::uint64_t offset, std::uint64_t length) const;
    const mapped::RecordEnvelope& record_envelope(std::uint64_t reference, std::uint16_t kind) const;
    std::vector<std::byte> immutable_record(std::uint64_t reference, std::uint16_t kind) const;
    std::vector<std::uint64_t> record_references(std::uint16_t kind) const;
    std::uint64_t section_offset(mapped::SectionKind kind) const;
    // First attempt reuses this operation's validated framing under its own
    // continuously held guard. Later attempts use fresh journal validation.
    // Replacing the guard/session or writing metadata directly breaks that
    // private operation contract; use the generic journal API for raw fixtures.
    Result<mapped::TransactionResult> execute(const mapped::TransactionPlan& plan);

    std::shared_ptr<SessionState> _session;
    mapped::MutationGuard _guard;
    mapped::MutationJournal _journal;
    mapped::JournalSnapshot _snapshot;
    mapped::NativeFile* _file{};

  private:
    std::vector<mapped::JournalRecordRange> _recordBoundaries;
    mapped::JournalSnapshot _validatedSnapshot;
    bool _validatedForExecute{};
    RuntimeAccessReadObserver* _readObserver{};
    // Separate last windows keep atomic fixed state and immutable history from
    // evicting one another. Both end at the original recovered file size and
    // are destroyed before the operation's guard, including on unwind.
    mutable std::optional<mapped::MappedReadWindow> _wordWindow;
    mutable std::optional<mapped::MappedReadWindow> _byteWindow;
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

  // Owning metadata for one freshly resolved schema and its checked Fields.
  // This carries no Task ownership or authority to mutate the live Session.
  struct LoadedSchemaContext
  {
    LoadedSchema _schema;
    std::vector<LoadedField> _fields;
  };

  std::vector<LoadedTask> runtime_tasks(RuntimeAccess& access);
  void runtime_validate_task_state(std::span<const std::uint64_t> words, TaskKind kind);
  LoadedTask runtime_task(RuntimeAccess& access, TaskId task);
  std::vector<LoadedField> runtime_fields(RuntimeAccess& access);
  // Trusted operation-local overloads accept the complete unmodified Tasks
  // returned by runtime_tasks(access) under this same continuously held gate,
  // without intervening Session mutation. No cross-operation cache is added;
  // generic overloads continue loading and checking current Tasks.
  std::vector<LoadedField> runtime_fields(RuntimeAccess& access, std::span<const LoadedTask> tasks);
  std::vector<LoadedSchema> runtime_schemas(RuntimeAccess& access);
  std::vector<LoadedSchema> runtime_schemas(RuntimeAccess& access, std::span<const LoadedTask> tasks);
  // Reuse Fields already validated under this access's held mutation gate.
  std::vector<LoadedSchema> runtime_schemas(RuntimeAccess& access, std::span<const LoadedField> fields);
  std::vector<LoadedSchema> runtime_schemas(RuntimeAccess& access, std::span<const LoadedField> fields,
                                              std::span<const LoadedTask> tasks);
  LoadedField runtime_field(RuntimeAccess& access, FieldId field);
  LoadedSchemaContext runtime_schema_context(RuntimeAccess& access, SchemaId schema);
  LoadedSchema runtime_schema(RuntimeAccess& access, SchemaId schema);
  void runtime_validate_field_policy(const FieldDescriptor& descriptor, ErrorCode invalidCode);
  mapped::SessionDefinition runtime_session_definition(RuntimeAccess& access);
  void runtime_validate_options(const SessionOptions& options, ErrorCode invalidCode);
  void runtime_check_task(const std::shared_ptr<TaskState>& task);
  void runtime_check_task_owner(const std::shared_ptr<TaskState>& task);
  void runtime_check_messaging_owner(const std::shared_ptr<TaskState>& task);
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
