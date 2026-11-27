// Private current-log experiment, not an admitted public delivery mode. Setup
// and accepted records use real public APIs; direct byte edits below are
// explicitly corrupt sidecar descriptors, never synthetic accepted messages.
#include "recorded_spsc.h"

#include "angaria/detail/append_layout.h"
#include "angaria/detail/byte_operations.h"
#include "native_file.h"
#include "native_fixture.h"
#include "process_support.h"
#include "publication_metadata.h"
#include "source_fixture.h"
#include "transport_support.h"

#include <gtest/gtest.h>

#include <array>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <limits>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

#ifndef ANGARIA_RECORDED_SPSC_WORKER
#error ANGARIA_RECORDED_SPSC_WORKER must name the independent crash worker
#endif

extern char** environ;

namespace
{
  using namespace angaria;
  namespace support = angaria::test_support;
  namespace mapped = angaria::detail::mapped;
  namespace spsc = angaria::benchmarks::spsc;
  using angaria::benchmarks::BenchmarkFailure;
  using support::require_value;
  using namespace std::chrono_literals;

  spsc::Options options_for(const support::SourceScenario& scenario, std::uint64_t capacity = 2)
  {
    spsc::Options options;
    options._directory = require_value(scenario._created._session.options())._directory;
    options._session = scenario._created._session.identity();
    options._queue = scenario._queue;
    options._producer = scenario._created._creator.claim()._task;
    options._schema = scenario._schema;
    options._payloadBytes = 4;
    options._capacity = capacity;
    options._maxLogBytes = 2 * 1024 * 1024;
    return options;
  }

  struct Appended
  {
    PublicationId _publication;
    std::uint64_t _reference;
  };

  Appended append(support::SourceScenario& scenario, QueueWriter& writer,
                    spsc::RecordedSpsc& producer, std::uint32_t number, bool expose = true)
  {
    if (!producer.has_credit())
    {
      throw std::runtime_error("Test must preflight ring credit before issuing an ID or appending");
    }
    const auto bytes = require_value(support::source_codec(scenario).encode({number}));
    const auto publication = require_value(scenario._created._creator.next_publication_id());
    const auto reference = producer.next_record_reference();
    const auto result = require_value(writer.send(publication, support::source_message(scenario._schema, bytes)));
    if (result._status != AcceptanceStatus::Accepted)
    {
      throw std::runtime_error("Test publication was not accepted by the real log");
    }
    if (expose)
    {
      producer.publish_committed(reference, publication);
    }
    return {publication, reference};
  }

  std::uint32_t number(const MessageView& message, const BinaryPayloadCodec& codec)
  {
    const auto values = require_value(codec.decode(message._payload));
    return std::get<std::uint32_t>(values.front());
  }

  template <typename Operation>
  void expect_prototype_error(Operation operation, ErrorCode expected)
  {
    try
    {
      operation();
      FAIL() << "Corrupt prototype connection or invalid ownership was accepted";
    }
    catch (const BenchmarkFailure& failure)
    {
      EXPECT_EQ(failure._error._code, expected) << failure._error._message;
    }
  }

  void corrupt_descriptor_word(const spsc::Options& options, std::uint64_t offset, std::uint64_t value)
  {
    // Quiescent corruption fixture: ordinary byte encoding is deliberately
    // independent of the descriptor writer and no reader runs concurrently.
    auto file = require_value(mapped::NativeFile::open_existing(
        options._directory / "recorded-spsc.connection", mapped::FileAccess::ReadWrite));
    std::array<std::byte, 8> bytes{};
    angaria::detail::write_unsigned_le(bytes, 0, value);
    require_value(file.write_exact(offset, bytes));
  }

  class CrashWorker
  {
  public:
    CrashWorker(const std::filesystem::path& directory, const std::string& mode)
    {
      std::array<int, 2> output{};
      if (::pipe(output.data()) == -1)
      {
        throw std::runtime_error("Cannot create recorded-SPSC worker pipe");
      }
      for (const auto descriptor : output)
      {
        if (::fcntl(descriptor, F_SETFD, FD_CLOEXEC) == -1)
        {
          ::close(output[0]);
          ::close(output[1]);
          throw std::runtime_error("Cannot mark recorded-SPSC pipe close-on-exec");
        }
      }
      posix_spawn_file_actions_t actions;
      auto error = ::posix_spawn_file_actions_init(&actions);
      if (error == 0)
      {
        error = ::posix_spawn_file_actions_adddup2(&actions, output[1], STDOUT_FILENO);
        error = error == 0 ? ::posix_spawn_file_actions_addclose(&actions, output[0]) : error;
        error = error == 0 ? ::posix_spawn_file_actions_addclose(&actions, output[1]) : error;
        const std::array<std::string, 3> values{ANGARIA_RECORDED_SPSC_WORKER, directory.string(), mode};
        std::array<char*, 4> arguments{};
        for (std::size_t index = 0; index != values.size(); ++index)
        {
          arguments[index] = const_cast<char*>(values[index].c_str());
        }
        error = error == 0 ? ::posix_spawn(&_pid, values[0].c_str(), &actions, nullptr,
                                           arguments.data(), environ) : error;
        ::posix_spawn_file_actions_destroy(&actions);
      }
      ::close(output[1]);
      if (error != 0)
      {
        ::close(output[0]);
        throw std::system_error(error, std::generic_category(), "Spawn recorded-SPSC worker");
      }
      _output = output[0];
    }

    ~CrashWorker()
    {
      if (_pid > 0)
      {
        ::kill(_pid, SIGKILL);
        while (::waitpid(_pid, nullptr, 0) == -1 && errno == EINTR)
        {
        }
      }
      support::close_descriptor(_output);
    }

    void kill_at_checkpoint()
    {
      char ready{};
      support::receive_exact(_output, &ready, 1,
          {"Recorded-SPSC checkpoint deadline expired", "Recorded-SPSC checkpoint unavailable",
           "Recorded-SPSC worker exited before checkpoint"});
      if (ready != 'R')
      {
        throw std::runtime_error("Unexpected recorded-SPSC checkpoint report");
      }
      support::terminate(_pid, {"Cannot kill recorded-SPSC worker", "Cannot reap recorded-SPSC worker",
          "Recorded-SPSC termination deadline expired", "Recorded-SPSC worker was not killed"});
    }

  private:
    pid_t _pid{-1};
    int _output{-1};
  };

  class RecordedSpscTest : public testing::Test
  {
  protected:
    void SetUp() override
    {
      auto pattern = (std::filesystem::temp_directory_path() / "angaria-recorded-spsc-XXXXXX").string();
      ASSERT_NE(::mkdtemp(pattern.data()), nullptr);
      _root = pattern;
      _directory = _root / "session";
    }

    void TearDown() override
    {
      std::error_code ignored;
      std::filesystem::remove_all(_root, ignored);
    }

    std::filesystem::path _root;
    std::filesystem::path _directory;
  };

  TEST_F(RecordedSpscTest, BorrowedLoggedMessageDoesNotConsumeOrAcknowledgeLegacyReceipt)
  {
    auto scenario = support::create_source_scenario(_directory);
    const auto options = options_for(scenario);
    spsc::RecordedSpsc::create(options);
    auto writer = require_value(scenario._created._creator.open_writer(scenario._queue));
    auto consumerTask = require_value(scenario._created._session.attach_task(scenario._consumer));
    spsc::RecordedSpsc producer(options, spsc::Role::Producer, scenario._created._creator.claim());
    spsc::RecordedSpsc consumer(options, spsc::Role::Consumer, consumerTask.claim());
    const auto accepted = append(scenario, writer, producer, 42);
    const auto received = consumer.try_receive();
    ASSERT_TRUE(received);
    EXPECT_EQ(received->_header._publication, accepted._publication);
    EXPECT_EQ(received->_header._queue, scenario._queue);
    EXPECT_EQ(received->_header._queueSequence, 1u);
    EXPECT_EQ(received->_header._schema, scenario._schema);
    EXPECT_EQ(received->_header._consumer, consumerTask.claim());
    ASSERT_TRUE(received->_header._enqueuedAt);
    EXPECT_EQ(received->_header._publishedAt, *received->_header._enqueuedAt);
    EXPECT_TRUE(received->_header._dequeuedAt);
    EXPECT_EQ(number(*received, support::source_codec(scenario)), 42u);

    // The experiment owns separate cursors. An ordinary acknowledged Source
    // can still issue exactly the original Pending receipt through public APIs.
    auto legacy = require_value(consumerTask.open_source_set(scenario._source));
    const auto delivery = legacy.receive(0ns);
    ASSERT_TRUE(std::holds_alternative<Delivery>(delivery));
    EXPECT_EQ(std::get<Delivery>(delivery)._message._header._publication, accepted._publication);
    require_value(legacy.acknowledge(std::get<Delivery>(delivery)._token));
    require_value(legacy.close());
    EXPECT_FALSE(consumer.try_receive());
    const auto replay = spsc::RecordedSpsc::replay(options);
    ASSERT_EQ(replay.size(), 1u);
    EXPECT_EQ(replay.front()._header._publication, accepted._publication);
    EXPECT_FALSE(replay.front()._header._consumer);
    EXPECT_FALSE(replay.front()._header._dequeuedAt);
  }

  TEST_F(RecordedSpscTest, RingCreditIsCheckedBeforeAcceptanceAndWrapKeepsEarlierBorrowStable)
  {
    auto scenario = support::create_source_scenario(_directory);
    const auto options = options_for(scenario);
    spsc::RecordedSpsc::create(options);
    auto writer = require_value(scenario._created._creator.open_writer(scenario._queue));
    auto consumerTask = require_value(scenario._created._session.attach_task(scenario._consumer));
    spsc::RecordedSpsc producer(options, spsc::Role::Producer, scenario._created._creator.claim());
    spsc::RecordedSpsc consumer(options, spsc::Role::Consumer, consumerTask.claim());
    append(scenario, writer, producer, 1);
    append(scenario, writer, producer, 2);
    ASSERT_FALSE(producer.has_credit());
    const auto before = std::filesystem::file_size(_directory / "session.ang");
    if (producer.has_credit())
    {
      append(scenario, writer, producer, 99);
    }
    EXPECT_EQ(std::filesystem::file_size(_directory / "session.ang"), before);
    const auto first = consumer.try_receive();
    ASSERT_TRUE(first);
    const auto savedHeader = first->_header;
    EXPECT_TRUE(producer.has_credit());
    append(scenario, writer, producer, 3);
    const auto second = consumer.try_receive();
    const auto third = consumer.try_receive();
    ASSERT_TRUE(second);
    ASSERT_TRUE(third);
    const auto codec = support::source_codec(scenario);
    EXPECT_EQ(number(*second, codec), 2u);
    EXPECT_EQ(number(*third, codec), 3u);
    EXPECT_EQ(third->_header._publication._sequence, 3u);
    EXPECT_EQ(number(*first, codec), 1u);
    EXPECT_EQ(first->_header, savedHeader);
    EXPECT_FALSE(consumer.try_receive());
    EXPECT_TRUE(producer.has_credit());
  }

  TEST_F(RecordedSpscTest, OwningCopyKeepsExactHeaderAndSchemaAfterEndpointsAndSessionClose)
  {
    std::optional<Message> saved;
    std::optional<BinaryPayloadCodec> codec;
    MessageHeader header;
    SessionId session;
    {
      auto scenario = support::create_source_scenario(_directory);
      const auto options = options_for(scenario);
      session = options._session;
      spsc::RecordedSpsc::create(options);
      auto writer = require_value(scenario._created._creator.open_writer(scenario._queue));
      auto consumerTask = require_value(scenario._created._session.attach_task(scenario._consumer));
      spsc::RecordedSpsc producer(options, spsc::Role::Producer, scenario._created._creator.claim());
      spsc::RecordedSpsc consumer(options, spsc::Role::Consumer, consumerTask.claim());
      append(scenario, writer, producer, 73);
      const auto view = consumer.try_receive();
      ASSERT_TRUE(view);
      header = view->_header;
      saved.emplace(require_value(view->copy_message(4)));
      codec.emplace(support::source_codec(scenario));
    }
    ASSERT_TRUE(saved);
    EXPECT_EQ(saved->_header, header);
    EXPECT_EQ(saved->schema_session(), session);
    EXPECT_EQ(std::get<std::uint32_t>(require_value(saved->decode(*codec)).front()), 73u);
  }

  TEST_F(RecordedSpscTest, CommitWithoutDescriptorIsDiscoveredAndLateDescriptorDoesNotDuplicateIt)
  {
    auto scenario = support::create_source_scenario(_directory);
    const auto options = options_for(scenario);
    spsc::RecordedSpsc::create(options);
    auto writer = require_value(scenario._created._creator.open_writer(scenario._queue));
    auto consumerTask = require_value(scenario._created._session.attach_task(scenario._consumer));
    spsc::RecordedSpsc producer(options, spsc::Role::Producer, scenario._created._creator.claim());
    spsc::RecordedSpsc consumer(options, spsc::Role::Consumer, consumerTask.claim());
    const auto accepted = append(scenario, writer, producer, 99, false);
    const auto view = consumer.try_receive();
    ASSERT_TRUE(view);
    EXPECT_EQ(view->_header._publication, accepted._publication);
    EXPECT_EQ(number(*view, support::source_codec(scenario)), 99u);
    producer.publish_committed(accepted._reference, accepted._publication);
    EXPECT_FALSE(consumer.try_receive());
    EXPECT_TRUE(producer.has_credit());
    ASSERT_EQ(spsc::RecordedSpsc::replay(options).size(), 1u);
  }

  TEST_F(RecordedSpscTest, AcknowledgedPreloadIsOutsideTheNewConnectionReplayRange)
  {
    auto scenario = support::create_source_scenario(_directory);
    auto writer = require_value(scenario._created._creator.open_writer(scenario._queue));
    support::send_source_value(scenario, writer, 7);
    {
      auto task = require_value(scenario._created._session.attach_task(scenario._consumer));
      auto source = require_value(task.open_source_set(scenario._source));
      const auto result = source.receive(0ns);
      ASSERT_TRUE(std::holds_alternative<Delivery>(result));
      require_value(source.acknowledge(std::get<Delivery>(result)._token));
      require_value(source.close());
    }
    const auto options = options_for(scenario);
    spsc::RecordedSpsc::create(options);
    auto consumerTask = require_value(scenario._created._session.attach_task(scenario._consumer));
    spsc::RecordedSpsc producer(options, spsc::Role::Producer, scenario._created._creator.claim());
    spsc::RecordedSpsc consumer(options, spsc::Role::Consumer, consumerTask.claim());
    EXPECT_FALSE(consumer.try_receive());
    const auto accepted = append(scenario, writer, producer, 8);
    const auto received = consumer.try_receive();
    ASSERT_TRUE(received);
    EXPECT_EQ(received->_header._queueSequence, 2u);
    const auto replay = spsc::RecordedSpsc::replay(options);
    ASSERT_EQ(replay.size(), 1u);
    EXPECT_EQ(replay.front()._header._publication, accepted._publication);
  }

  TEST_F(RecordedSpscTest, ReplacedConsumerReplaysFromStreamStartWithoutClaimingExactlyOnceProcessing)
  {
    auto scenario = support::create_source_scenario(_directory);
    const auto options = options_for(scenario);
    spsc::RecordedSpsc::create(options);
    auto writer = require_value(scenario._created._creator.open_writer(scenario._queue));
    spsc::RecordedSpsc producer(options, spsc::Role::Producer, scenario._created._creator.claim());
    const auto accepted = append(scenario, writer, producer, 12);
    TaskClaim oldClaim;
    {
      auto task = require_value(scenario._created._session.attach_task(scenario._consumer));
      oldClaim = task.claim();
      spsc::RecordedSpsc consumer(options, spsc::Role::Consumer, oldClaim);
      ASSERT_TRUE(consumer.try_receive());
    }
    auto replacement = require_value(scenario._created._session.attach_task(scenario._consumer));
    spsc::RecordedSpsc consumer(options, spsc::Role::Consumer, replacement.claim());
    const auto replayed = consumer.try_receive();
    ASSERT_TRUE(replayed);
    EXPECT_EQ(replayed->_header._publication, accepted._publication);
    EXPECT_EQ(replayed->_header._consumer, replacement.claim());
    EXPECT_GT(replacement.claim()._incarnation, oldClaim._incarnation);
    EXPECT_FALSE(consumer.try_receive());
  }

  TEST_F(RecordedSpscTest, DuplicateRoleOwnershipAndWrongSessionBindingAreRejected)
  {
    auto scenario = support::create_source_scenario(_directory);
    const auto options = options_for(scenario);
    spsc::RecordedSpsc::create(options);
    const auto claim = scenario._created._creator.claim();
    spsc::RecordedSpsc producer(options, spsc::Role::Producer, claim);
    expect_prototype_error([&]
    {
      spsc::RecordedSpsc duplicate(options, spsc::Role::Producer, claim);
    }, ErrorCode::OwnershipBusy);
    auto wrong = options;
    ++wrong._session._low;
    expect_prototype_error([&]
    {
      spsc::RecordedSpsc stale(wrong, spsc::Role::Producer, claim);
    }, ErrorCode::IoError);
    wrong = options;
    wrong._schema._version = 2;
    EXPECT_ANY_THROW(spsc::RecordedSpsc stale(wrong, spsc::Role::Producer, claim));
  }

  TEST_F(RecordedSpscTest, StaleTicketGenerationPublicationAndOutOfBoundsReferencesFailBeforeDelivery)
  {
    for (const auto& [word, corrupted] : std::array<std::pair<std::uint64_t, std::uint64_t>, 4>{
        {{0, std::numeric_limits<std::uint64_t>::max()}, {16, 999}, {32, 0}, {40, 2}}})
    {
      const auto directory = _directory / std::to_string(word);
      std::filesystem::create_directories(directory.parent_path());
      auto scenario = support::create_source_scenario(directory);
      const auto options = options_for(scenario);
      spsc::RecordedSpsc::create(options);
      auto writer = require_value(scenario._created._creator.open_writer(scenario._queue));
      auto task = require_value(scenario._created._session.attach_task(scenario._consumer));
      spsc::RecordedSpsc producer(options, spsc::Role::Producer, scenario._created._creator.claim());
      spsc::RecordedSpsc consumer(options, spsc::Role::Consumer, task.claim());
      append(scenario, writer, producer, 1);
      // Private descriptor format: slots begin at320, 64 bytes each. These
      // independent offsets are deliberately not obtained from the writer.
      corrupt_descriptor_word(options, 320 + word, corrupted);
      expect_prototype_error([&]
      {
        (void)consumer.try_receive();
      }, ErrorCode::IoError);
    }
  }

  TEST_F(RecordedSpscTest, ShortCommittedPublicationIsRejectedBeforeReadingItsFixedBody)
  {
    auto scenario = support::create_source_scenario(_directory);
    const auto options = options_for(scenario);
    spsc::RecordedSpsc::create(options);
    auto writer = require_value(scenario._created._creator.open_writer(scenario._queue));
    auto task = require_value(scenario._created._session.attach_task(scenario._consumer));
    spsc::RecordedSpsc producer(options, spsc::Role::Producer, scenario._created._creator.claim());
    spsc::RecordedSpsc consumer(options, spsc::Role::Consumer, task.claim());
    const auto accepted = append(scenario, writer, producer, 5);
    auto file = require_value(mapped::NativeFile::open_existing(_directory / "session.ang", mapped::FileAccess::ReadWrite));
    std::array<std::byte, 16> original{};
    require_value(file.read_exact(accepted._reference + 8, original));
    // Deliberate immutable-envelope corruption: totalBytes=64 and immutable=0
    // with the genuine permanent readiness mark left intact. No reader/writer
    // operates concurrently with this private fixture edit.
    std::array<std::byte, 16> shortRecord{};
    angaria::detail::write_unsigned_le(shortRecord, 0, std::uint64_t{64});
    require_value(file.write_exact(accepted._reference + 8, shortRecord));
    expect_prototype_error([&]
    {
      (void)consumer.try_receive();
    }, ErrorCode::IoError);
    // Restore the actual accepted log before native Task cleanup validates it.
    require_value(file.write_exact(accepted._reference + 8, original));
  }

  TEST_F(RecordedSpscTest, KilledProducerAfterCommitStillHasLiveAndOwningReplayWithoutDescriptor)
  {
    spsc::Options options;
    {
      auto scenario = support::create_source_scenario(_directory);
      options = options_for(scenario);
      spsc::RecordedSpsc::create(options);
    }
    CrashWorker worker(_directory, "commit_before_publish");
    worker.kill_at_checkpoint();
    auto session = require_value(Session::attach(_directory));
    auto task = require_value(session.attach_task("Consumer"));
    spsc::RecordedSpsc consumer(options, spsc::Role::Consumer, task.claim());
    const auto view = consumer.try_receive();
    ASSERT_TRUE(view);
    EXPECT_EQ(angaria::detail::read_unsigned_le<std::uint32_t>(view->_payload, 0), 99u);
    EXPECT_EQ(view->_header._publication._producer._incarnation, 2u);
    EXPECT_FALSE(consumer.try_receive());
    const auto replay = spsc::RecordedSpsc::replay(options);
    ASSERT_EQ(replay.size(), 1u);
    EXPECT_EQ(replay.front()._header._publication, view->_header._publication);
  }

  TEST_F(RecordedSpscTest, LogFullRejectsAnotherPublicationAndKeepsAcceptedStreamReplayable)
  {
    auto scenario = support::create_source_scenario(_directory);
    auto writer = require_value(scenario._created._creator.open_writer(scenario._queue));
    const auto initial = support::journal_snapshot(_directory)._tail;
    constexpr std::uint64_t maxBytes = 2 * 1024 * 1024;
    // Private sizing only: prefill is still a real public opaque Text message,
    // acknowledged before the experiment's start. Leave exactly one ordinary
    // four-byte Publication+manifest, avoiding thousands of quadratic appends.
    auto planned_tail = [&](std::uint64_t payload, std::uint64_t limit) -> std::optional<std::uint64_t>
    {
      const auto layout = mapped::plan_publication_layout(initial, 1, payload, maxBytes);
      if (std::holds_alternative<Error>(layout))
      {
        return std::nullopt;
      }
      mapped::AppendLayoutPlanner planner(initial);
      planner.add_record(std::get<mapped::PublicationLayout>(layout)._immutableBytes, 128);
      const auto complete = planner.finish(144, limit);
      return complete ? std::optional{complete->_tail} : std::nullopt;
    };
    const auto small = planned_tail(4, maxBytes);
    ASSERT_TRUE(small);
    const auto target = maxBytes - (*small - initial);
    std::uint64_t low{}, high = maxBytes - initial;
    while (low < high)
    {
      const auto middle = low + (high - low + 1) / 2;
      if (planned_tail(middle, target))
      {
        low = middle;
      }
      else
      {
        high = middle - 1;
      }
    }
    ASSERT_EQ(planned_tail(low, target), target);
    const std::vector<std::byte> padding(static_cast<std::size_t>(low), std::byte{'x'});
    const auto prefill = require_value(scenario._created._creator.next_publication_id());
    require_value(writer.send(prefill, support::source_message(scenario._schema, padding, PayloadFormat::Text)));
    {
      auto task = require_value(scenario._created._session.attach_task(scenario._consumer));
      auto source = require_value(task.open_source_set(scenario._source));
      const auto delivered = source.receive(0ns);
      ASSERT_TRUE(std::holds_alternative<Delivery>(delivered));
      require_value(source.acknowledge(std::get<Delivery>(delivered)._token));
      require_value(source.close());
    }
    const auto options = options_for(scenario);
    spsc::RecordedSpsc::create(options);
    auto task = require_value(scenario._created._session.attach_task(scenario._consumer));
    spsc::RecordedSpsc producer(options, spsc::Role::Producer, scenario._created._creator.claim());
    spsc::RecordedSpsc consumer(options, spsc::Role::Consumer, task.claim());
    const auto accepted = append(scenario, writer, producer, 42);
    ASSERT_EQ(std::filesystem::file_size(_directory / "session.ang"), maxBytes);
    const auto received = consumer.try_receive();
    ASSERT_TRUE(received);
    ASSERT_TRUE(producer.has_credit());
    const auto rejected = require_value(scenario._created._creator.next_publication_id());
    const auto bytes = require_value(support::source_codec(scenario).encode({std::uint32_t{43}}));
    const auto outcome = writer.send(rejected, support::source_message(scenario._schema, bytes));
    EXPECT_TRUE(support::has_error(outcome, ErrorCode::LogFull));
    EXPECT_EQ(std::filesystem::file_size(_directory / "session.ang"), maxBytes);
    const auto replay = spsc::RecordedSpsc::replay(options);
    ASSERT_EQ(replay.size(), 1u);
    EXPECT_EQ(replay.front()._header._publication, accepted._publication);
    EXPECT_EQ(number(replay.front().view(), support::source_codec(scenario)), 42u);
    EXPECT_FALSE(consumer.try_receive());
  }

  TEST_F(RecordedSpscTest, KilledIncompleteAppendRollsBackAndNeverAppearsInReplay)
  {
    spsc::Options options;
    std::uint64_t before{};
    {
      auto scenario = support::create_source_scenario(_directory);
      options = options_for(scenario);
      spsc::RecordedSpsc::create(options);
      before = std::filesystem::file_size(_directory / "session.ang");
    }
    CrashWorker worker(_directory, "partial_append");
    worker.kill_at_checkpoint();
    const auto replay = spsc::RecordedSpsc::replay(options);
    EXPECT_TRUE(replay.empty());
    EXPECT_EQ(std::filesystem::file_size(_directory / "session.ang"), before);
    auto session = require_value(Session::attach(_directory));
    auto task = require_value(session.attach_task("Consumer"));
    spsc::RecordedSpsc consumer(options, spsc::Role::Consumer, task.claim());
    EXPECT_FALSE(consumer.try_receive());
  }
} // namespace
