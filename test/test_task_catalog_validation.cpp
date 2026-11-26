// Private corruption fixtures qualify historical inspection. Ordinary programs
// register through Task::registry(); they never rewrite a retained definition.
#include "result_helpers.h"
#include "angaria/runtime.h"
#include "runtime_state.h"

#include <gtest/gtest.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <variant>

namespace
{
  using namespace angaria;
  namespace runtime = angaria::detail;
  namespace mapped = angaria::detail::mapped;
  namespace fs = std::filesystem;

  using angaria::test_support::require_value;

  class TaskCatalogValidationTest : public testing::Test
  {
  protected:
    void SetUp() override
    {
      auto pattern = (fs::temp_directory_path() / "angaria-task-catalog-XXXXXX").string();
      const auto directory = ::mkdtemp(pattern.data());
      if (!directory)
      {
        throw std::runtime_error("Cannot create Task catalog fixture directory");
      }
      _root = directory;
      SessionOptions options;
      options._name = "Task history validation";
      options._directory = _root / "session";
      options._maxLogBytes = 1048576;
      options._limits = CapacityLimits::empty().tasks(8);
      TaskDescriptor creator;
      creator._name = "Setup";
      _created = std::make_unique<CreatedSession>(require_value(Session::create(options, creator)));
    }

    void TearDown() override
    {
      _created.reset();
      std::error_code ignored;
      fs::remove_all(_root, ignored);
    }

    TaskId process(const std::string& name)
    {
      TaskDescriptor descriptor;
      descriptor._name = name;
      return require_value(_created->_creator.registry().register_task(descriptor));
    }

    void rewrite(TaskId task, const std::function<void(mapped::TaskDefinition&)>& change)
    {
      auto state = std::make_shared<runtime::SessionState>();
      state->_directory = _root / "session";
      state->_controller = require_value(mapped::SessionController::open_existing(state->_directory));
      runtime::RuntimeAccess access(state);
      const auto loaded = runtime::runtime_task(access, task);
      auto definition = loaded._definition;
      change(definition);
      const auto body = require_value(mapped::encode_task_definition(definition, loaded._reference));
      require_value(access._file->write_exact(loaded._reference + mapped::record_envelope_bytes, body));
    }

    std::string bytes() const
    {
      std::ifstream input(_root / "session" / "session.ang", std::ios::binary);
      if (!input)
      {
        throw std::runtime_error("Cannot read Task catalog fixture");
      }
      return {std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
    }

    template <typename T>
    void expect_rejection(const Result<T>& result, const char* path)
    {
      ASSERT_TRUE(std::holds_alternative<Error>(result));
      EXPECT_EQ(std::get<Error>(result)._code, ErrorCode::IoError);
      EXPECT_EQ(std::get<Error>(result)._path, path);
    }

    fs::path _root;
    std::unique_ptr<CreatedSession> _created;
  };

  TEST_F(TaskCatalogValidationTest, RejectsCreatorRegisteredAfterItsAttributedTask)
  {
    const auto early = process("Early");
    const auto later = process("Later");
    // Both identities exist now, but Later was absent at Early's revision.
    // Changing only attribution keeps the codec shape valid while making the
    // retained history impossible to interpret as a consistent Registry.
    rewrite(early, [later](auto& definition)
    {
      definition._registration._createdBy = later;
    });
    const auto invalid = bytes();
    expect_rejection(_created->_session.registry().snapshot(), "task.created_by");
    expect_rejection(_created->_session.registry().snapshot(1), "task.created_by");
    expect_rejection(Session::attach(_root / "session"), "task.created_by");
    EXPECT_EQ(bytes(), invalid);
  }

  TEST_F(TaskCatalogValidationTest, RejectsThreadParentRegisteredAfterTheThread)
  {
    const auto parent = process("Parent");
    TaskDescriptor worker;
    worker._name = "Worker";
    worker._kind = TaskKind::Thread;
    worker._process = parent;
    const auto thread = require_value(_created->_creator.registry().register_task(worker));
    const auto later = process("LaterParent");
    rewrite(thread, [later](auto& definition)
    {
      definition._descriptor._process = later;
    });
    const auto invalid = bytes();
    expect_rejection(_created->_session.registry().snapshot(), "task.process");
    expect_rejection(_created->_session.registry().snapshot(1), "task.process");
    expect_rejection(Session::attach(_root / "session"), "task.process");
    EXPECT_EQ(bytes(), invalid);
  }

  TEST_F(TaskCatalogValidationTest, RejectsPredecessorLinksOnImmutableTaskDefinitions)
  {
    const auto early = process("Early");
    const auto later = process("Later");
    {
      auto state = std::make_shared<runtime::SessionState>();
      state->_directory = _root / "session";
      state->_controller = require_value(mapped::SessionController::open_existing(state->_directory));
      runtime::RuntimeAccess access(state);
      const auto previous = runtime::runtime_task(access, early)._reference;
      const auto reference = runtime::runtime_task(access, later)._reference;
      auto envelope = access.record_envelope(reference, 2);
      // Generic framing permits an earlier record offset. Task definitions are
      // immutable registrations, so they must not acquire a predecessor chain.
      envelope._previous = previous;
      const auto frame = require_value(mapped::encode_record_envelope(envelope));
      require_value(access._file->write_exact(reference, frame));
    }
    const auto invalid = bytes();
    expect_rejection(_created->_session.registry().snapshot(), "task.index");
    expect_rejection(_created->_session.registry().snapshot(1), "task.index");
    expect_rejection(Session::attach(_root / "session"), "task.index");
    EXPECT_EQ(bytes(), invalid);
  }
} // namespace
