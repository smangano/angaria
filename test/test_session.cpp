#include "angaria/session.h"
#include "session_log.h"

#include <gtest/gtest.h>

#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <fstream>
#include <sstream>

#include <sys/wait.h>
#include <unistd.h>

namespace
{
  namespace fs = std::filesystem;

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

    fs::path _root;
    fs::path _session;
    fs::path _log;
  };

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

    EXPECT_EQ(angaria::receive_reading(_session), 17);
    EXPECT_FALSE(angaria::receive_reading(_session));
    ASSERT_EQ(angaria::send_reading(_session, 42), angaria::SendResult::Sent);
    EXPECT_EQ(angaria::receive_reading(_session), 42);

    std::ostringstream output;
    angaria::inspect_session(_log, output);
    EXPECT_NE(output.str().find("sequence=2 timestamp_ns="), std::string::npos);
    EXPECT_NE(output.str().find("type=Reading value=23\n"), std::string::npos);
    EXPECT_NE(output.str().find("sequence=3 timestamp_ns="), std::string::npos);
  }

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
    EXPECT_EQ(angaria::receive_reading(_session), -5);
    EXPECT_EQ(angaria::receive_reading(_session), 6);
  }

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
} // namespace
