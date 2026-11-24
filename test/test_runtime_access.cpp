// Private lookup qualification beneath Session/Registry inspection. These
// fixtures append definitions directly to control batch geometry and exercise
// record lookup independently of public registration; usage examples live in
// test_task_registration.cpp.

#include "runtime_state.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace
{
  using namespace angaria;
  namespace runtime = angaria::detail;
  namespace mapped = angaria::detail::mapped;
  namespace fs = std::filesystem;

  template <typename T>
  T require_value(Result<T> result)
  {
    if (const auto* error = std::get_if<Error>(&result))
    {
      throw std::runtime_error(error->_path + ": " + error->_message);
    }
    return std::get<T>(std::move(result));
  }

  template <typename Operation>
  void expect_io_error(Operation operation)
  {
    try
    {
      operation();
      FAIL() << "Expected an invalid record reference to fail";
    }
    catch (const runtime::RuntimeFailure& failure)
    {
      EXPECT_EQ(failure._error._code, ErrorCode::IoError);
    }
  }

  class RuntimeAccessTest : public testing::Test
  {
  protected:
    void SetUp() override
    {
      auto pattern = (fs::temp_directory_path() / "angaria-boundaries-XXXXXX").string();
      ASSERT_NE(::mkdtemp(pattern.data()), nullptr);
      _root = pattern;
      SessionOptions options;
      options._name = "Boundary lookups";
      options._directory = _root / "session";
      options._maxLogBytes = 2097152;
      options._limits = CapacityLimits::empty().tasks(65);
      TaskDescriptor creator;
      creator._name = "Creator";
      _created.emplace(require_value(Session::create(options, creator)));
      _state = std::make_shared<runtime::SessionState>();
      _state->_directory = fs::canonical(options._directory);
      _state->_controller = require_value(mapped::SessionController::open_existing(_state->_directory));
      runtime::RuntimeAccess access(_state);
      _references.push_back(access.read_words(access.section_offset(mapped::SectionKind::TaskIndex) + 8, 1)[0]);
    }

    void TearDown() override
    {
      _state.reset();
      _created.reset();
      std::error_code ignored;
      fs::remove_all(_root, ignored);
    }

    mapped::TransactionPlan append_plan(runtime::RuntimeAccess& access) const
    {
      mapped::TransactionPlan plan;
      plan._base = access._snapshot;
      plan._kind = mapped::JournalKind::Append;
      plan._actor = TaskId{1};
      plan._actorIncarnation = access.read_words(access.section_offset(mapped::SectionKind::TaskState), 1)[0];
      plan._newRevision = access._snapshot._revision + 1;
      return plan;
    }

    void append_threads(unsigned count)
    {
      runtime::RuntimeAccess access(_state);
      auto plan = append_plan(access);
      auto offset = access._snapshot._tail;
      const auto index = access.section_offset(mapped::SectionKind::TaskIndex);
      for (unsigned thread = 0; thread < count; ++thread)
      {
        const auto id = static_cast<std::uint16_t>(_references.size() + thread + 1);
        mapped::TaskDefinition definition;
        definition._id = static_cast<TaskId>(id);
        definition._descriptor._name = "Thread" + std::to_string(id);
        definition._descriptor._kind = TaskKind::Thread;
        definition._descriptor._process = TaskId{1};
        definition._descriptor._doc = std::string(257, 'd');
        definition._registration = RegistrationMetadata{TaskId{1}, UtcTimestamp{std::chrono::nanoseconds{123}},
                                                        plan._newRevision};
        mapped::JournalRecord record;
        record._kind = 2;
        record._immutable = require_value(mapped::encode_task_definition(definition, offset));
        const std::array<std::uint64_t, 4> row{id, offset, plan._newRevision, 0};
        plan._commands.push_back(runtime::runtime_write_words(index + (id - 1) * mapped::index_entry_bytes, row));
        offset += *mapped::checked_align(64 + *mapped::checked_align(record._immutable.size(), 8), 64);
        plan._records.push_back(std::move(record));
      }
      const auto result = require_value(access._journal.execute(access._guard, plan));
      _references.insert(_references.end(), result._records.begin(), result._records.end());
    }

    std::uint64_t append_ring_with_interior_envelope()
    {
      runtime::RuntimeAccess access(_state);
      auto plan = append_plan(access);
      const auto interior = access._snapshot._tail + mapped::record_envelope_bytes;
      mapped::RecordEnvelope envelope;
      envelope._kind = 2;
      envelope._totalBytes = 192;
      envelope._immutableBytes = 128;
      envelope._generation = access._snapshot._generation + 1;
      envelope._registryRevision = plan._newRevision;
      envelope._readiness = 2;
      const auto encoded = require_value(mapped::encode_record_envelope(envelope));
      mapped::TaskDefinition definition;
      definition._id = TaskId{65};
      definition._descriptor._name = "Forged";
      definition._registration = RegistrationMetadata{TaskId{1}, UtcTimestamp{}, plan._newRevision};
      const auto body = require_value(mapped::encode_task_definition(definition, interior));
      mapped::JournalRecord ring;
      ring._kind = 8;
      ring._mutable.resize(512);
      std::copy(encoded.begin(), encoded.end(), ring._mutable.begin());
      std::copy(body.begin(), body.end(), ring._mutable.begin() + mapped::record_envelope_bytes);
      plan._records.push_back(std::move(ring));
      const std::array ready{runtime::session_ready_flag};
      plan._commands.push_back(runtime::runtime_write_words(112, ready));
      require_value(access._journal.execute(access._guard, plan));
      return interior;
    }

    fs::path _root;
    std::optional<CreatedSession> _created;
    std::shared_ptr<runtime::SessionState> _state;
    std::vector<std::uint64_t> _references;
  };

  // One operation can inspect many Task definitions and validate their shared
  // Process parent. The same owning envelope entries survive repeated forward,
  // reverse, and complete-table lookups; no timing threshold is needed to verify reuse.
  TEST_F(RuntimeAccessTest, DefinitionLookupsReuseOneGateBoundEnvelopeIndex)
  {
    append_threads(48);
    runtime::RuntimeAccess access(_state);
    std::vector<const mapped::RecordEnvelope*> envelopes;
    for (const auto reference : _references)
    {
      envelopes.push_back(&access.record_envelope(reference, 2));
    }
    for (unsigned pass = 0; pass < 3; ++pass)
    {
      const auto tasks = runtime::runtime_tasks(access);
      ASSERT_EQ(tasks.size(), _references.size());
      for (std::size_t remaining = _references.size(); remaining != 0; --remaining)
      {
        const auto index = remaining - 1;
        const auto definition = require_value(mapped::decode_task_definition(
            access.immutable_record(_references[index], 2), _references[index]));
        EXPECT_EQ(definition._id, static_cast<TaskId>(index + 1));
        EXPECT_EQ(definition._registration._revision, envelopes[index]->_registryRevision);
        EXPECT_EQ(&access.record_envelope(_references[index], 2), envelopes[index]);
      }
    }
  }

  // A plausible aligned header inside another record is still not a record
  // boundary. This synthetic ring carries a complete fake Task envelope/body;
  // neither its apparent kind nor readiness can authorize following that address.
  TEST_F(RuntimeAccessTest, IndexedLookupRejectsAlignedInteriorAndWrongKindReferences)
  {
    const auto interior = append_ring_with_interior_envelope();
    runtime::RuntimeAccess access(_state);
    expect_io_error([&]
    {
      static_cast<void>(access.immutable_record(interior, 2));
    });
    expect_io_error([&]
    {
      static_cast<void>(access.immutable_record(_references[0], 1));
    });
    for (const auto reference : {std::uint64_t{0}, _references[0] + 1, access._snapshot._tail})
    {
      expect_io_error([&]
      {
        static_cast<void>(access.immutable_record(reference, 2));
      });
    }
    EXPECT_EQ(require_value(mapped::decode_task_definition(
                  access.immutable_record(_references[0], 2), _references[0]))._id, TaskId{1});
  }

  // Cache scope follows the recovered operation, not the process or controller.
  // After an Append completes and its gate owner closes, the next access must
  // index the new definition while preserving the older committed references.
  TEST_F(RuntimeAccessTest, ANewAccessIndexesDefinitionsAddedAfterThePreviousSnapshot)
  {
    mapped::RecordEnvelope original;
    {
      runtime::RuntimeAccess access(_state);
      original = access.record_envelope(_references[0], 2);
    }
    append_threads(1);
    runtime::RuntimeAccess access(_state);
    EXPECT_EQ(access.record_envelope(_references[0], 2), original);
    ASSERT_EQ(_references.size(), 2u);
    const auto definition = require_value(mapped::decode_task_definition(
        access.immutable_record(_references[1], 2), _references[1]));
    EXPECT_EQ(definition._id, TaskId{2});
    EXPECT_EQ(runtime::runtime_task(access, TaskId{2})._reference, _references[1]);
  }
} // namespace
