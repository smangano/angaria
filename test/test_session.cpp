// These tests cover the legacy v0 Producer/Consumer queue used by queue_demo.
// Its filesystem publication and fork behavior are prototype-specific; read
// test_runtime_session.cpp for the format-2 Session API and native exec boundary.

#include "angaria/session.h"
#include "session_log.h"
#include "role_lock.h"

#include <gtest/gtest.h>

#include <barrier>
#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <future>
#include <memory>
#include <thread>
#include <sstream>

#include <sys/wait.h>
#include <unistd.h>

namespace
{
  namespace fs = std::filesystem;

  std::optional<std::int32_t> consume_reading(const fs::path& session)
  {
    // A convenience for tests that want complete prototype consumption. Tests
    // about redelivery use Consumer directly so they can leave work unacknowledged.
    angaria::Consumer consumer(session);
    const auto delivery = consumer.receive_reading();
    if (!delivery)
    {
      return std::nullopt;
    }
    consumer.acknowledge(delivery->_sequence);
    return delivery->_value;
  }

  [[noreturn]] void crash_now()
  {
    ::kill(::getpid(), SIGKILL);
    ::_exit(2);
  }

  class SessionTest : public testing::Test
  {
  protected:
    void SetUp() override
    {
      auto path = (fs::temp_directory_path() / "angaria-test-XXXXXX").string();
      const auto directory = ::mkdtemp(path.data());
      ASSERT_NE(directory, nullptr);
      _root = directory;
      _session = _root / "demo";
      _log = _session / angaria::detail::SESSION_LOG_NAME;
      angaria::create_session(_session);
    }

    void TearDown() override
    {
      std::error_code ignored;
      if (!_root.empty())
      {
        fs::remove_all(_root, ignored);
      }
    }

    void kill_after(const std::function<void()>& action)
    {
      // SIGKILL deliberately bypasses endpoint destructors. A normal scope exit
      // would exercise graceful cleanup instead of recovery after process death.
      const pid_t child = ::fork();
      ASSERT_NE(child, -1);
      if (child == 0)
      {
        try
        {
          action();
        }
        catch (...)
        {
          ::_exit(1);
        }
        ::_exit(2);
      }
      int status{};
      pid_t result;
      do
      {
        result = ::waitpid(child, &status, 0);
      } while (result == -1 && errno == EINTR);
      ASSERT_EQ(result, child);
      ASSERT_TRUE(WIFSIGNALED(status)) << "Child exited with status " << status;
      ASSERT_EQ(WTERMSIG(status), SIGKILL);
    }

    fs::path _root;
    fs::path _session;
    fs::path _log;
  };

  // The v0 prototype can commit a log record before adding its queue entry. Kill
  // at that boundary to show that the unpublished record remains inspectable,
  // while the consumer sees only published entries and later sends can proceed.
  TEST_F(SessionTest, ProducerKilledAfterCommitBeforePublication)
  {
    ASSERT_EQ(angaria::send_reading(_session, 17), angaria::SendResult::Sent);
    const pid_t child = ::fork();
    ASSERT_NE(child, -1);

    if (child == 0)
    {
      using namespace angaria::detail;
      try
      {
        angaria::Producer producer(_session);
        SessionLog log(_log, LogMode::Update);
        if (log.append_reading(log.read_header(), 23))
        {
          // Keep the log open to exercise release of the operation lock on process death.
          ::kill(::getpid(), SIGKILL);
        }
      }
      catch (...)
      {
        ::_exit(1);
      }
      ::_exit(2);
    }

    int status{};
    pid_t result;
    do
    {
      result = ::waitpid(child, &status, 0);
    } while (result == -1 && errno == EINTR);
    ASSERT_EQ(result, child);
    ASSERT_TRUE(WIFSIGNALED(status));
    EXPECT_EQ(WTERMSIG(status), SIGKILL);

    EXPECT_EQ(consume_reading(_session), 17);
    EXPECT_FALSE(consume_reading(_session));
    ASSERT_EQ(angaria::send_reading(_session, 42), angaria::SendResult::Sent);
    EXPECT_EQ(consume_reading(_session), 42);

    std::ostringstream output;
    angaria::inspect_session(_log, output);
    EXPECT_NE(output.str().find("sequence=2 timestamp_ns="), std::string::npos);
    EXPECT_NE(output.str().find("type=Reading value=23\n"), std::string::npos);
    EXPECT_NE(output.str().find("sequence=3 timestamp_ns="), std::string::npos);
  }

  // Append a deliberately truncated record to emulate an interrupted v0 write.
  // Inspection ignores only the incomplete tail, and the next send replaces that
  // tail while preserving earlier committed deliveries.
  TEST_F(SessionTest, PartialTailIsIgnoredAndReplacedByNextSend)
  {
    ASSERT_EQ(angaria::send_reading(_session, -5), angaria::SendResult::Sent);
    const auto committedSize = fs::file_size(_log);
    {
      std::ofstream file(_log, std::ios::binary | std::ios::app);
      ASSERT_TRUE(file.write("\x1e\x00\x00", 3));
    }
    ASSERT_EQ(fs::file_size(_log), committedSize + 3);

    std::ostringstream before;
    angaria::inspect_session(_log, before);
    EXPECT_NE(before.str().find("type=Reading value=-5\n"), std::string::npos);
    EXPECT_EQ(before.str().find("sequence=2"), std::string::npos);

    ASSERT_EQ(angaria::send_reading(_session, 6), angaria::SendResult::Sent);
    EXPECT_EQ(fs::file_size(_log), committedSize + angaria::detail::SESSION_RECORD_SIZE);
    EXPECT_EQ(consume_reading(_session), -5);
    EXPECT_EQ(consume_reading(_session), 6);
  }

  // An independently encoded log describes a different queue/message name and
  // schema identity. Inspection must use that stored schema rather than hard-code
  // the demo Reading layout.
  TEST_F(SessionTest, InspectorUsesStoredSchema)
  {
    constexpr char fixture[] = "ANGARIA\0"
                               "\x01\x00"                         // Version 1.
                               "\x2a\x00"                         // 42-byte header.
                               "\x48\x00\x00\x00\x00\x00\x00\x00" // 72-byte limit.
                               "\x03\x00\x01\x04\x07\x05"         // Schema 3, int32, name lengths.
                               "demoCountercount"
                               "\x1e\x00\x00\x00"                 // 30-byte record.
                               "\x01\x00\x00\x00\x00\x00\x00\x00" // Sequence 1.
                               "\x7b\x00\x00\x00\x00\x00\x00\x00" // Timestamp 123 ns.
                               "\x03\x00\xd6\xff\xff\xff"         // Schema 3 and value -42.
                               "CMIT";
    static_assert(sizeof(fixture) - 1 == 72);
    const auto path = _root / "fixture.ang";
    {
      std::ofstream file(path, std::ios::binary);
      ASSERT_TRUE(file.write(fixture, sizeof(fixture) - 1));
    }

    std::ostringstream output;
    angaria::inspect_session(path, output);
    EXPECT_EQ(output.str(), "queue=demo sequence=1 timestamp_ns=123 type=Counter count=-42\n");
  }

  // A complete-sized but corrupt record is not an ignorable partial tail. The
  // inspector must fail before presenting it as a committed delivery.
  TEST_F(SessionTest, InspectorRejectsFullRecordWithInvalidCommitMarker)
  {
    ASSERT_EQ(angaria::send_reading(_session, 1), angaria::SendResult::Sent);
    {
      std::fstream file(_log, std::ios::binary | std::ios::in | std::ios::out);
      ASSERT_TRUE(file.seekp(-1, std::ios::end).put('!'));
    }

    std::ostringstream output;
    EXPECT_THROW(angaria::inspect_session(_log, output), std::runtime_error);
    EXPECT_TRUE(output.str().empty());
  }
  // One prototype Producer and one Consumer may coexist, but a second owner of
  // either role is rejected locally and in another process. Queue removal also
  // requires those retained endpoint owners to close first.
  TEST_F(SessionTest, RolesAreExclusiveAndIndependent)
  {
    angaria::Producer producer(_session);
    angaria::Consumer consumer(_session);
    EXPECT_EQ(producer.identity()._taskId, 1);
    EXPECT_EQ(consumer.identity()._taskId, 2);
    EXPECT_EQ(producer.identity()._incarnation, 1u);
    EXPECT_EQ(consumer.identity()._incarnation, 1u);
    EXPECT_EQ(producer.identity()._pid, static_cast<std::uint64_t>(::getpid()));
    EXPECT_THROW(angaria::Producer{_session}, angaria::RoleBusy);
    EXPECT_THROW(angaria::Consumer{_session}, angaria::RoleBusy);
    EXPECT_THROW(angaria::remove_queue(_session), angaria::RoleBusy);

    kill_after(
        [&]
        {
          bool producerBusy = false;
          bool consumerBusy = false;
          try
          {
            angaria::Producer duplicate(_session);
          }
          catch (const angaria::RoleBusy&)
          {
            producerBusy = true;
          }
          try
          {
            angaria::Consumer duplicate(_session);
          }
          catch (const angaria::RoleBusy&)
          {
            consumerBusy = true;
          }
          if (!producerBusy || !consumerBusy)
          {
            throw std::runtime_error("a child acquired an occupied role");
          }
          crash_now();
        });
  }

  // Start competing prototype owners together and keep the winner alive until
  // both attempts finish. This prevents sequential success from disguising a
  // failure of exclusive role acquisition.
  TEST_F(SessionTest, SimultaneousRoleClaimsHaveOneWinner)
  {
    std::barrier gate(2);
    auto claim = [&]
    {
      gate.arrive_and_wait();
      std::unique_ptr<angaria::Producer> producer;
      try
      {
        producer = std::make_unique<angaria::Producer>(_session);
      }
      catch (const angaria::RoleBusy&)
      {
      }
      catch (...)
      {
        gate.arrive_and_drop();
        throw;
      }
      gate.arrive_and_wait();
      return producer != nullptr;
    };
    auto first = std::async(std::launch::async, claim);
    auto second = std::async(std::launch::async, claim);
    EXPECT_NE(first.get(), second.get());
  }

  // An abandoned temporary role-state file must not become the authoritative
  // attachment. Reopening advances the last committed incarnation and removes
  // the incomplete replacement file.
  TEST_F(SessionTest, InterruptedRoleStateWritePreservesIncarnation)
  {
    {
      angaria::Producer producer(_session);
      EXPECT_EQ(producer.identity()._incarnation, 1u);
    }
    {
      std::ofstream temporary(_session / "producer.state.tmp");
      ASSERT_TRUE(temporary << "incomplete replacement state");
    }
    angaria::Producer replacement(_session);
    EXPECT_EQ(replacement.identity()._incarnation, 2u);
    EXPECT_FALSE(fs::exists(_session / "producer.state.tmp"));
  }

  // The prototype consumer reads the same pending delivery until it acknowledges
  // that exact sequence. Invalid or repeated acknowledgments cannot make progress;
  // valid acknowledgment permits the next queued delivery and frees capacity.
  TEST_F(SessionTest, OneDeliveryRemainsPendingUntilAcknowledged)
  {
    angaria::Producer producer(_session);
    angaria::Consumer consumer(_session);
    ASSERT_EQ(producer.send_reading(10), angaria::SendResult::Sent);
    ASSERT_EQ(producer.send_reading(20), angaria::SendResult::Sent);
    const auto first = consumer.receive_reading();
    ASSERT_TRUE(first);
    EXPECT_EQ(first->_value, 10);
    const auto again = consumer.receive_reading();
    ASSERT_TRUE(again);
    EXPECT_EQ(again->_sequence, first->_sequence);
    EXPECT_EQ(again->_timestampNs, first->_timestampNs);
    EXPECT_EQ(producer.send_reading(30), angaria::SendResult::QueueFull);
    EXPECT_THROW(consumer.acknowledge(first->_sequence + 1), std::invalid_argument);
    EXPECT_EQ(producer.send_reading(30), angaria::SendResult::QueueFull);
    consumer.acknowledge(first->_sequence);
    EXPECT_THROW(consumer.acknowledge(first->_sequence), std::invalid_argument);
    ASSERT_EQ(producer.send_reading(30), angaria::SendResult::Sent);

    for (const auto value : {20, 30})
    {
      const auto delivery = consumer.receive_reading();
      ASSERT_TRUE(delivery);
      EXPECT_EQ(delivery->_value, value);
      consumer.acknowledge(delivery->_sequence);
    }
    EXPECT_FALSE(consumer.receive_reading());
  }

  // Producer wrappers can close and reattach without recreating the Consumer.
  // The producer incarnation changes, while queue sequence order continues and
  // the surviving consumer claim remains unchanged.
  TEST_F(SessionTest, ProducerRestartsWhileConsumerStaysAttached)
  {
    angaria::Consumer consumer(_session);
    for (std::uint64_t incarnation = 1; incarnation <= 2; ++incarnation)
    {
      angaria::Producer producer(_session);
      EXPECT_EQ(producer.identity()._incarnation, incarnation);
      ASSERT_EQ(producer.send_reading(42), angaria::SendResult::Sent);
    }
    for (std::uint64_t sequence = 1; sequence <= 2; ++sequence)
    {
      const auto delivery = consumer.receive_reading();
      ASSERT_TRUE(delivery);
      EXPECT_EQ(delivery->_sequence, sequence);
      consumer.acknowledge(sequence);
    }
    EXPECT_EQ(consumer.identity()._incarnation, 1u);
  }

  // Let a Consumer close with an unacknowledged delivery, then attach a new one.
  // The replacement receives that same sequence and original timestamp before
  // acknowledging it; the Producer does not need to restart.
  TEST_F(SessionTest, ConsumerRestartsWhileProducerStaysAttached)
  {
    angaria::Producer producer(_session);
    ASSERT_EQ(producer.send_reading(42), angaria::SendResult::Sent);
    std::int64_t timestamp{};
    {
      angaria::Consumer consumer(_session);
      EXPECT_EQ(consumer.identity()._incarnation, 1u);
      const auto delivery = consumer.receive_reading();
      ASSERT_TRUE(delivery);
      timestamp = delivery->_timestampNs;
    }

    angaria::Consumer replacement(_session);
    EXPECT_EQ(replacement.identity()._incarnation, 2u);
    const auto delivery = replacement.receive_reading();
    ASSERT_TRUE(delivery);
    EXPECT_EQ(delivery->_sequence, 1u);
    EXPECT_EQ(delivery->_timestampNs, timestamp);
    replacement.acknowledge(delivery->_sequence);
    EXPECT_FALSE(replacement.receive_reading());
    EXPECT_EQ(producer.identity()._incarnation, 1u);
  }

  // Receiving alone does not complete a delivery obligation. Abrupt consumer
  // death releases its native role and the replacement receives the unacknowledged
  // message again under a new incarnation.
  TEST_F(SessionTest, ConsumerKilledBeforeAcknowledgmentRedelivers)
  {
    angaria::Producer producer(_session);
    ASSERT_EQ(producer.send_reading(42), angaria::SendResult::Sent);
    kill_after(
        [&]
        {
          angaria::Consumer consumer(_session);
          const auto delivery = consumer.receive_reading();
          if (!delivery || delivery->_value != 42)
          {
            throw std::runtime_error("missing delivery before consumer death");
          }
          crash_now();
        });

    angaria::Consumer replacement(_session);
    EXPECT_EQ(replacement.identity()._incarnation, 2u);
    const auto delivery = replacement.receive_reading();
    ASSERT_TRUE(delivery);
    EXPECT_EQ(delivery->_sequence, 1u);
    replacement.acknowledge(delivery->_sequence);
    EXPECT_FALSE(replacement.receive_reading());
  }

  // Make the child acknowledge before killing it. The replacement must resume
  // after that committed prototype progress point, preserving the acknowledgment
  // and delivering the next message.
  TEST_F(SessionTest, ConsumerKilledAfterAcknowledgmentResumesNextMessage)
  {
    angaria::Producer producer(_session);
    ASSERT_EQ(producer.send_reading(42), angaria::SendResult::Sent);
    ASSERT_EQ(producer.send_reading(43), angaria::SendResult::Sent);
    kill_after(
        [&]
        {
          angaria::Consumer consumer(_session);
          const auto delivery = consumer.receive_reading();
          if (!delivery || delivery->_sequence != 1)
          {
            throw std::runtime_error("missing delivery before acknowledgment");
          }
          consumer.acknowledge(delivery->_sequence);
          crash_now();
        });

    angaria::Consumer replacement(_session);
    EXPECT_EQ(replacement.identity()._incarnation, 2u);
    const auto delivery = replacement.receive_reading();
    ASSERT_TRUE(delivery);
    EXPECT_EQ(delivery->_sequence, 2u);
    EXPECT_EQ(delivery->_value, 43);
    replacement.acknowledge(delivery->_sequence);
  }

  // Publish the prototype queue entry and kill its producer before any subsequent
  // notification. The waiting consumer must discover the published entry anyway;
  // notification is not the authoritative record of available work.
  TEST_F(SessionTest, ProducerKilledAfterPublicationNeedsNoNotification)
  {
    angaria::Consumer consumer(_session);
    EXPECT_FALSE(consumer.receive_reading());
    const pid_t child = ::fork();
    ASSERT_NE(child, -1);
    if (child == 0)
    {
      try
      {
        angaria::Producer producer(_session);
        std::this_thread::sleep_for(std::chrono::milliseconds{30});
        angaria::detail::SessionLog log(_log, angaria::detail::LogMode::Update);
        const auto offset = log.append_reading(log.read_header(), 42);
        if (offset && fs::create_directory(_session / "pending" / std::to_string(*offset)))
        {
          ::kill(::getpid(), SIGKILL);
        }
      }
      catch (...)
      {
        ::_exit(1);
      }
      ::_exit(2);
    }

    const auto delivery = consumer.receive_reading(std::chrono::seconds{2});
    int status{};
    pid_t result;
    do
    {
      result = ::waitpid(child, &status, 0);
    } while (result == -1 && errno == EINTR);
    ASSERT_EQ(result, child);
    ASSERT_TRUE(WIFSIGNALED(status));
    EXPECT_EQ(WTERMSIG(status), SIGKILL);
    ASSERT_TRUE(delivery);
    EXPECT_EQ(delivery->_value, 42);
    consumer.acknowledge(delivery->_sequence);
    angaria::Producer replacement(_session);
    EXPECT_EQ(replacement.identity()._incarnation, 2u);
    EXPECT_EQ(replacement.send_reading(43), angaria::SendResult::Sent);
  }

  // An inherited prototype endpoint cannot act as its parent, and its copied
  // descriptor must not keep the role busy after the parent closes. The fresh-child
  // reattachment here is legacy behavior, not the format-2 fork/exec contract.
  TEST_F(SessionTest, ForkedChildDoesNotRetainParentRole)
  {
    auto producer = std::make_unique<angaria::Producer>(_session);
    const pid_t child = ::fork();
    ASSERT_NE(child, -1);
    if (child == 0)
    {
      try
      {
        producer->send_reading(42);
      }
      catch (const std::logic_error&)
      {
        ::raise(SIGSTOP);
        // The parent detaches while this child still has the inherited object.
        try
        {
          angaria::Producer replacement(_session);
          ::_exit(replacement.identity()._incarnation == 2 ? 0 : 3);
        }
        catch (...)
        {
          ::_exit(4);
        }
      }
      ::_exit(5);
    }

    int status{};
    ASSERT_EQ(::waitpid(child, &status, WUNTRACED), child);
    ASSERT_TRUE(WIFSTOPPED(status));
    producer.reset();
    ASSERT_EQ(::kill(child, SIGCONT), 0);
    ASSERT_EQ(::waitpid(child, &status, 0), child);
    ASSERT_TRUE(WIFEXITED(status));
    EXPECT_EQ(WEXITSTATUS(status), 0);
  }

  // An invalid pending entry makes prototype attachment fail. Removing the
  // corruption must permit a fresh attachment with no leaked role lock; receive
  // timeouts also distinguish invalid durations from an ordinary empty queue.
  TEST_F(SessionTest, AttachmentRejectsCorruptionAndReleasesRole)
  {
    const auto path = _session / "pending" / "999";
    fs::create_directory(path);
    EXPECT_THROW(angaria::Producer{_session}, std::runtime_error);
    EXPECT_THROW(angaria::Consumer{_session}, std::runtime_error);
    fs::remove(path);
    angaria::Producer producer(_session);
    angaria::Consumer consumer(_session);
    EXPECT_EQ(producer.identity()._incarnation, 1u);
    EXPECT_EQ(consumer.identity()._incarnation, 1u);
    EXPECT_THROW(consumer.receive_reading(std::chrono::milliseconds{-1}), std::invalid_argument);
    EXPECT_FALSE(consumer.receive_reading(std::chrono::milliseconds{20}));
  }
} // namespace
