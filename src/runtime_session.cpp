#include "runtime_state.h"
#include "topology_state.h"
#include "configuration_runtime.h"

#include "build_version.h"
#include "native_atomic.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <random>
#include <stdexcept>
#include <string>
#include <system_error>

#include <fcntl.h>
#include <pwd.h>
#include <unistd.h>
#if defined(__APPLE__)
#include <sys/stdio.h>
#elif defined(__linux__)
#include <linux/fs.h>
#include <sys/syscall.h>
#endif

namespace angaria
{
  namespace
  {
    namespace fs = std::filesystem;
    using namespace detail;

    // Only this invocation's exclusively created staging directory is removed.
    // The final path is never removed, including after a conflicting creation.
    struct CreationDirectory
    {
      fs::path _path;
      bool _published{};

      ~CreationDirectory()
      {
        if (!_published && !_path.empty())
        {
          try
          {
            std::error_code ignored;
            fs::remove_all(_path, ignored);
          }
          catch (...)
          {
          }
        }
      }
    };

    fs::path creation_path(const fs::path& requested)
    {
      if (requested.empty() || requested.native().find('\0') != std::string::npos ||
          requested.filename().empty() || requested.filename() == "." || requested.filename() == "..")
      {
        runtime_fail(ErrorCode::InvalidConfiguration, "session.directory", "Session needs a nonempty directory name");
      }
      const auto parent = fs::canonical(requested.has_parent_path() ? requested.parent_path() : fs::path{"."});
      const auto target = parent / requested.filename();
      std::error_code error;
      const auto status = fs::symlink_status(target, error);
      if (error && error != std::errc::no_such_file_or_directory)
      {
        runtime_fail(ErrorCode::IoError, "session.directory", error.message());
      }
      if (status.type() != fs::file_type::not_found && status.type() != fs::file_type::none)
      {
        runtime_fail(ErrorCode::Conflict, "session.directory", "Session path already exists");
      }
      return target;
    }

    void create_staging_directory(CreationDirectory& staging, const fs::path& target)
    {
      auto pattern = (target.parent_path() / ".angaria-create-XXXXXX").string();
      if (::mkdtemp(pattern.data()) == nullptr)
      {
        runtime_fail(ErrorCode::IoError, "session.directory", std::strerror(errno));
      }
      try
      {
        staging._path = pattern;
      }
      catch (...)
      {
        // No resources have been created inside this exclusive directory yet.
        ::rmdir(pattern.c_str());
        throw;
      }
    }

    void publish_directory(CreationDirectory& staging, const fs::path& target)
    {
      int result{};
#if defined(__APPLE__)
      result = ::renameatx_np(AT_FDCWD, staging._path.c_str(), AT_FDCWD, target.c_str(), RENAME_EXCL);
#elif defined(__linux__)
      result = static_cast<int>(::syscall(SYS_renameat2, AT_FDCWD, staging._path.c_str(),
                                          AT_FDCWD, target.c_str(), RENAME_NOREPLACE));
#else
      runtime_fail(ErrorCode::UnsupportedPolicy, "session.directory", "Exclusive directory publication is unsupported");
#endif
      if (result != 0)
      {
        const auto saved = errno;
        runtime_fail(saved == EEXIST || saved == ENOTEMPTY ? ErrorCode::Conflict : ErrorCode::IoError,
                     "session.directory", std::strerror(saved));
      }
      staging._published = true;
    }

    SessionId new_identity()
    {
      std::random_device source;
      auto word = [&]()
      {
        std::uint64_t value{};
        for (unsigned index = 0; index < 8; ++index)
        {
          value = (value << 8) | (static_cast<std::uint64_t>(source()) & 0xff);
        }
        return value;
      };
      SessionId result{word(), word()};
      if (result == SessionId{})
      {
        result._low = 1;
      }
      return result;
    }

    std::string creator_user()
    {
      std::vector<char> buffer(1024);
      for (;;)
      {
        passwd user{};
        passwd* found{};
        const auto result = ::getpwuid_r(::geteuid(), &user, buffer.data(), buffer.size(), &found);
        if (result == 0)
        {
          return found && user.pw_name && *user.pw_name ? std::string{user.pw_name} : std::to_string(::geteuid());
        }
        if (result != ERANGE || buffer.size() > 1048576)
        {
          runtime_fail(ErrorCode::IoError, "session.creator_user", "Cannot read effective user's account name");
        }
        buffer.resize(buffer.size() * 2);
      }
    }

    std::uint64_t record_end(std::uint64_t offset, std::size_t bodyBytes)
    {
      const auto body = mapped::checked_align(bodyBytes, 8);
      const auto required = body ? mapped::checked_add(*body, 64) : std::nullopt;
      const auto allocation = required ? mapped::checked_align(*required, 64) : std::nullopt;
      const auto end = allocation ? mapped::checked_add(offset, *allocation) : std::nullopt;
      if (!end || *end > mapped::max_log_bytes)
      {
        runtime_fail(ErrorCode::LogFull, "session.max_log_bytes", "Bootstrap records exceed format bounds");
      }
      return *end;
    }

    template <typename T>
    T configuration_value(Result<T> result)
    {
      if (auto* error = std::get_if<Error>(&result))
      {
        if (error->_code == ErrorCode::InvalidArgument)
        {
          error->_code = ErrorCode::InvalidConfiguration;
        }
      }
      return runtime_value(std::move(result));
    }

    void check_plain_session(const std::shared_ptr<SessionState>& state)
    {
      if (!state || !state->_controller)
      {
        throw std::logic_error("Session has no live state");
      }
      if (const auto result = state->_controller->check_owner(); std::holds_alternative<Error>(result))
      {
        throw std::logic_error(std::get<Error>(result)._message);
      }
    }

    void check_plain_task(const std::shared_ptr<TaskState>& state)
    {
      try
      {
        runtime_check_task(state);
      }
      catch (...)
      {
        throw std::logic_error(runtime_caught_error()._message);
      }
    }
  }

  Session::Session(std::shared_ptr<detail::SessionState> state) : _state(std::move(state))
  {
  }

  Task::Task(std::shared_ptr<detail::TaskState> state) : _state(std::move(state))
  {
  }

  Registry::Registry(std::shared_ptr<detail::SessionState> state, std::shared_ptr<detail::TaskState> actor)
      : _state(std::move(state)), _actor(std::move(actor))
  {
  }

  Result<CreatedSession> Session::create(const SessionOptions& options, const TaskDescriptor& creator)
  {
    return create_configured(options, creator, nullptr);
  }

  Result<CreatedSession> Session::create_from_file(const fs::path& configuration, std::string_view creator)
  {
    try
    {
      runtime_value(mapped::validate_metadata_text(creator, true, true, "creator", ErrorCode::InvalidConfiguration));
      auto plan = runtime_value(load_configuration(configuration, creator));
      return create_configured(plan._options, plan._tasks.front()._descriptor, &plan);
    }
    catch (...)
    {
      return runtime_caught_error();
    }
  }

  Result<CreatedSession> Session::create_configured(const SessionOptions& options, const TaskDescriptor& creator,
                                                     const ConfigurationPlan* initialConfiguration)
  {
    try
    {
      runtime_validate_options(options, ErrorCode::InvalidConfiguration);
      if (creator._kind != TaskKind::Process || creator._process)
      {
        runtime_fail(ErrorCode::InvalidConfiguration, "creator", "Session creator must be a Process Task without a parent");
      }
      const auto profile = runtime_value(mapped::qualify_native_profile());
      mapped::SuperblockConfiguration configuration;
      configuration._session = new_identity();
      configuration._maxLogBytes = options._maxLogBytes;
      configuration._limits = options._limits;
      configuration._liveAbiProfile = profile._liveAbiProfile;
      const auto layout = *mapped::make_fixed_layout(options._limits);
      if (layout._arenaBegin > options._maxLogBytes)
      {
        runtime_fail(ErrorCode::LogFull, "session.max_log_bytes", "Fixed reservations exceed the log byte ceiling");
      }

      mapped::SessionDefinition sessionDefinition;
      sessionDefinition._options = options;
      sessionDefinition._metadata = SessionMetadata{configuration._session, TaskId{1},
          UtcTimestamp{std::chrono::nanoseconds{runtime_now_ns()}}, framework_version,
          options._applicationVersion, creator_user()};
      mapped::TaskDefinition taskDefinition{TaskId{1}, creator,
          RegistrationMetadata{TaskId{1}, sessionDefinition._metadata._createdAt, 1}};
      const auto sessionOffset = layout._arenaBegin;
      // Validate raw directory UTF-8 before resolving its parent. A malformed
      // configuration path must not be mistaken for an I/O lookup failure.
      (void)configuration_value(mapped::encode_session_definition(sessionDefinition, sessionOffset));
      const auto target = creation_path(options._directory);
      sessionDefinition._options._directory = target;
      auto sessionBody = configuration_value(mapped::encode_session_definition(sessionDefinition, sessionOffset));
      const auto taskOffset = record_end(sessionOffset, sessionBody.size());
      auto taskBody = configuration_value(mapped::encode_task_definition(taskDefinition, taskOffset));
      const auto manifestOffset = record_end(taskOffset, taskBody.size());

      const auto indexOffset = layout._sections[0]._offset;
      const std::array<std::uint64_t, 1> root{sessionOffset};
      const std::array<std::uint64_t, 4> index{1, taskOffset, 1, 0};
      mapped::Manifest manifest;
      manifest._actor = TaskId{1};
      manifest._actorIncarnation = 1;
      manifest._newRevision = 1;
      manifest._records = {sessionOffset, taskOffset};
      manifest._commands = {runtime_write_words(72, root), runtime_write_words(indexOffset, index)};
      const auto manifestBody = configuration_value(mapped::encode_manifest_body(manifest, manifestOffset));
      const auto bootstrapTail = record_end(manifestOffset, manifestBody.size());
      if (bootstrapTail > options._maxLogBytes)
      {
        runtime_fail(ErrorCode::LogFull, "session.max_log_bytes", "Complete Session bootstrap exceeds the log byte ceiling");
      }
      if (initialConfiguration)
      {
        preflight_configuration(*initialConfiguration, bootstrapTail);
      }
      const auto prefix = runtime_value(mapped::encode_initial_superblock(configuration));

      CreationDirectory staging;
      create_staging_directory(staging, target);
      fs::create_directory(staging._path / "claims");
      {
        auto file = runtime_value(mapped::NativeFile::create_exclusive(staging._path / "session.ang", options._maxLogBytes));
        runtime_value(file.resize(layout._arenaBegin));
        runtime_value(file.write_exact(0, prefix));
        auto gate = runtime_value(mapped::NativeFile::create_exclusive(staging._path / "mutation.lock"));
        (void)gate;
      }
      auto state = std::make_shared<SessionState>();
      state->_directory = target;
      state->_controller = runtime_value(mapped::SessionController::open_existing(staging._path));
      {
        RuntimeAccess access(state, false);
        runtime_value(state->_controller->create_task_claim_file(access._guard, TaskId{1}));
      }
      auto creatorLease = runtime_value(state->_controller->try_claim_task(TaskId{1}));
      {
        RuntimeAccess access(state, false);
        mapped::TransactionPlan plan;
        plan._base = access._snapshot;
        plan._kind = mapped::JournalKind::Append;
        plan._actor = TaskId{1};
        plan._actorIncarnation = 1;
        plan._newRevision = 1;
        plan._records.push_back(mapped::JournalRecord{1, 1, 0, std::move(sessionBody), {}});
        plan._records.push_back(mapped::JournalRecord{2, 1, 0, std::move(taskBody), {}});
        plan._commands = std::move(manifest._commands);
        runtime_value(access._journal.execute(access._guard, plan));
      }
      auto taskState = runtime_value(attach_runtime_task(state, TaskId{1}, true, std::move(creatorLease)));
      // Prepare every owning return object before the final publication syscall.
      CreatedSession result{Session{std::move(state)}, Task{std::move(taskState)}};
      if (initialConfiguration)
      {
        // Only the creator can see this unpublished Session. Any registration,
        // activation, or I/O failure destroys its handles before staging cleanup.
        apply_configuration(result, *initialConfiguration);
      }
      publish_directory(staging, target);
      return result;
    }
    catch (...)
    {
      return runtime_caught_error();
    }
  }

  Result<Session> Session::attach(const fs::path& directory)
  {
    try
    {
      if (directory.empty() || directory.native().find('\0') != std::string::npos)
      {
        runtime_fail(ErrorCode::InvalidArgument, "session.directory", "Invalid Session directory");
      }
      auto state = std::make_shared<SessionState>();
      state->_directory = fs::canonical(directory);
      state->_controller = runtime_value(mapped::SessionController::open_existing(state->_directory));
      {
        RuntimeAccess access(state);
        (void)runtime_session_definition(access);
        (void)runtime_schemas(access);
        (void)runtime_topology(access);
      }
      return Session{std::move(state)};
    }
    catch (...)
    {
      return runtime_caught_error();
    }
  }

  Result<Session> Session::attach_from_file(const fs::path& configuration)
  {
    try
    {
      const auto requested = runtime_value(load_configuration_options(configuration));
      auto session = runtime_value(attach(requested._directory));
      const auto recorded = runtime_value(session.options());
      match_configuration_options(requested, recorded);
      auto registry = session.registry();
      const auto plan = runtime_value(load_configuration(configuration, {}, &registry));
      // The second parse resolves references against the live Registry. Recheck
      // its settings and location if another process replaced the TOML file.
      match_configuration_options(plan._options, recorded);
      if (fs::canonical(plan._options._directory) != session._state->_directory)
      {
        runtime_fail(ErrorCode::ConfigurationMismatch, "session.directory", "Configuration directory changed during attachment");
      }
      return session;
    }
    catch (...)
    {
      return runtime_caught_error();
    }
  }

  SessionId Session::identity() const
  {
    check_plain_session(_state);
    return _state->_controller->session_id();
  }

  Result<SessionMetadata> Session::metadata() const
  {
    try
    {
      RuntimeAccess access(_state);
      return runtime_session_definition(access)._metadata;
    }
    catch (...)
    {
      return runtime_caught_error();
    }
  }

  Result<SessionOptions> Session::options() const
  {
    try
    {
      RuntimeAccess access(_state);
      auto options = runtime_session_definition(access)._options;
      options._directory = _state->_directory;
      return options;
    }
    catch (...)
    {
      return runtime_caught_error();
    }
  }

  Registry Session::registry() const
  {
    check_plain_session(_state);
    return Registry{_state};
  }

  Result<Task> Session::attach_task(TaskId task)
  {
    auto result = attach_runtime_task(_state, task);
    if (const auto* error = std::get_if<Error>(&result))
    {
      return *error;
    }
    return Task{std::get<std::shared_ptr<TaskState>>(std::move(result))};
  }

  TaskClaim Task::claim() const
  {
    check_plain_task(_state);
    return _state->_claim;
  }

  Registry Task::registry() const
  {
    check_plain_task(_state);
    return Registry{_state->_session, _state};
  }

  Result<PublicationId> Task::next_publication_id()
  {
    return runtime_next_publication_id(_state);
  }

  Result<TaskId> Registry::find_task(std::string_view name) const
  {
    try
    {
      if (_actor)
      {
        runtime_check_task(_actor);
      }
      RuntimeAccess access(_state);
      for (const auto& task : runtime_tasks(access))
      {
        if (task._definition._descriptor._name == name)
        {
          return task._definition._id;
        }
      }
      runtime_fail(ErrorCode::NotFound, "task.name", "Task name is not registered");
    }
    catch (...)
    {
      return runtime_caught_error();
    }
  }

  Result<TaskDescriptor> Registry::describe_task(TaskId id) const
  {
    try
    {
      if (_actor)
      {
        runtime_check_task(_actor);
      }
      RuntimeAccess access(_state);
      return runtime_task(access, id)._definition._descriptor;
    }
    catch (...)
    {
      return runtime_caught_error();
    }
  }

  Result<RegistrationMetadata> Registry::metadata(TaskId id) const
  {
    try
    {
      if (_actor)
      {
        runtime_check_task(_actor);
      }
      RuntimeAccess access(_state);
      return runtime_task(access, id)._definition._registration;
    }
    catch (...)
    {
      return runtime_caught_error();
    }
  }

  Result<TaskRuntimeInfo> Registry::task_status(TaskId id) const
  {
    try
    {
      if (_actor)
      {
        runtime_check_task(_actor);
      }
      RuntimeAccess access(_state);
      return runtime_task_status(access, id);
    }
    catch (...)
    {
      return runtime_caught_error();
    }
  }
} // namespace angaria
