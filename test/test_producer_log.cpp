// Private producer-owned storage tests. They deliberately use prepared opaque
// records, not a runnable format-3 Session or application schema validation.
// The exec worker stops actual append operations at the commit boundaries.
#include "producer_log_fixture.h"
#include "native_file.h"
#include "native_process.h"
#include "process_support.h"
#include "result_assertions.h"
#include "wire_fixture.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cerrno>
#include <filesystem>
#include <limits>
#include <optional>
#include <span>
#include <thread>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include <fcntl.h>
#include <signal.h>
#include <sys/file.h>
#include <sys/wait.h>
#include <unistd.h>

#ifndef ANGARIA_PRODUCER_LOG_WORKER
#error ANGARIA_PRODUCER_LOG_WORKER must name the independent producer-log worker
#endif

namespace
{
  using namespace angaria;
  namespace recorded = angaria::detail::recorded;
  namespace mapped = angaria::detail::mapped;
  namespace support = angaria::test_support;
  using support::expect_error;
  using support::require_value;

  constexpr std::array payload{std::byte{0x11}, std::byte{0x22}, std::byte{0x33}, std::byte{0x44}};
  constexpr std::array worker_payload{std::byte{0x31}, std::byte{0x42}, std::byte{0x53}, std::byte{0x64}};

  static_assert(!std::is_copy_constructible_v<recorded::ProducerLog>);
  static_assert(!std::is_copy_constructible_v<recorded::ProducerLogReader>);
  static_assert(std::is_nothrow_move_constructible_v<recorded::ProducerLog>);
  static_assert(std::is_nothrow_move_constructible_v<recorded::ProducerLogReader>);

  std::vector<std::byte> file_bytes(const std::filesystem::path& path)
  {
    auto file = require_value(mapped::NativeFile::open_existing(path, mapped::FileAccess::ReadOnly));
    std::vector<std::byte> bytes(static_cast<std::size_t>(require_value(file.size())));
    require_value(file.read_exact(0, bytes));
    return bytes;
  }

  void corrupt_control(const std::filesystem::path& path, std::uint64_t offset, std::uint64_t value)
  {
    // The fixture has closed all endpoints. Still use native atomic access for
    // the live control word; ordinary helpers are only for immutable byte edits.
    auto file = require_value(mapped::NativeFile::open_existing(path, mapped::FileAccess::ReadWrite));
    auto mapping = require_value(file.map_read_write(offset, 8));
    auto bytes = require_value(mapping.writable_bytes());
    require_value(mapped::AtomicWord::bind(bytes, 0)).store_release(value);
  }

  class ErrorOnce final : public recorded::ProducerLogObserver
  {
  public:
    explicit ErrorOnce(recorded::ProducerLogCheckpoint point) : _point(point)
    {
    }

    OperationResult checkpoint(recorded::ProducerLogCheckpoint point) override
    {
      if (!_fired && point == _point)
      {
        _fired = true;
        return Error{ErrorCode::IoError, "Injected producer append interruption", "producer_log.test"};
      }
      return std::monostate{};
    }

    bool fired() const noexcept
    {
      return _fired;
    }

  private:
    recorded::ProducerLogCheckpoint _point;
    bool _fired{};
  };

  // This one fork-only helper intentionally does not exec: its live child must
  // retain all inherited ordinary file/mapping handles while the atfork claim
  // registry closes just the child's lease descriptors. NativeProcess remains
  // the shared helper for every ordinary independent worker above/below.
  class PausedForkChild
  {
  public:
    PausedForkChild()
    {
      std::array<int, 2> output{-1, -1};
      if (::pipe(output.data()) == -1)
      {
        throw std::runtime_error("Cannot create passive fork-child barrier");
      }
      _pid = ::fork();
      if (_pid == -1)
      {
        ::close(output[0]);
        ::close(output[1]);
        throw std::runtime_error("Cannot fork passive lease observer");
      }
      if (_pid == 0)
      {
        ::alarm(15);
        ::close(output[0]);
        try
        {
          const char ready = 'R';
          support::write_all(output[1], &ready, sizeof(ready), "Cannot report passive child readiness");
          ::close(output[1]);
          for (;;)
          {
            ::pause();
          }
        }
        catch (...)
        {
          ::_exit(1);
        }
      }
      ::close(output[1]);
      try
      {
        char ready{};
        support::receive_exact(output[0], &ready, sizeof(ready),
            {"Passive child deadline", "Passive child unavailable", "Passive child exited before readiness"});
        if (ready != 'R')
        {
          throw std::runtime_error("Unexpected passive child report");
        }
      }
      catch (...)
      {
        ::close(output[0]);
        cleanup();
        throw;
      }
      ::close(output[0]);
    }

    ~PausedForkChild()
    {
      cleanup();
    }

    PausedForkChild(const PausedForkChild&) = delete;
    PausedForkChild& operator=(const PausedForkChild&) = delete;

    bool alive()
    {
      if (_pid <= 0)
      {
        return false;
      }
      int status{};
      pid_t result;
      do
      {
        result = ::waitpid(_pid, &status, WNOHANG);
      } while (result == -1 && errno == EINTR);
      if (result == 0)
      {
        return true;
      }
      _pid = -1;
      return false;
    }

  private:
    void cleanup() noexcept
    {
      if (_pid > 0)
      {
        try
        {
          support::terminate(_pid, {"Cannot terminate passive child", "Cannot reap passive child",
                                   "Passive child cleanup deadline", "Passive child was not killed"});
        }
        catch (...)
        {
        }
      }
    }

    pid_t _pid{-1};
  };

  class ProducerLogTest : public testing::Test
  {
  protected:
    void create()
    {
      require_value(recorded::ProducerLog::create(_paths, _options));
    }

    recorded::ProducerLog writer(recorded::ProducerLogObserver* observer = nullptr)
    {
      return require_value(recorded::ProducerLog::open_writer(_paths, _options, observer));
    }

    recorded::ProducerLogReader reader()
    {
      return require_value(recorded::ProducerLogReader::open(_paths._log, _options));
    }

    TaskClaim worker_claim(support::NativeProcess& worker)
    {
      worker.expect('O');
      return {_options._header._session, _options._header._producer, worker.receive<std::uint64_t>()};
    }

    support::ProducerLogDirectory _directory;
    recorded::ProducerLogPaths _paths{support::producer_log_paths(_directory.path())};
    recorded::ProducerLogOptions _options{support::producer_log_options()};
  };
}

TEST_F(ProducerLogTest, OfflineCreationReservesFilesAndStartsWithAnEmptyCommittedCut)
{
  create();
  EXPECT_EQ(std::filesystem::file_size(_paths._log), _options._header._fileBytes);
  EXPECT_TRUE(std::filesystem::exists(_paths._owner));
  auto replay = reader();
  auto cut = require_value(replay.capture());
  EXPECT_EQ(cut.committed_end(), recorded::producer_header_bytes);
  EXPECT_EQ(cut.record_count(), 0u);
  EXPECT_FALSE(require_value(replay.next(cut)));
  const auto original = file_bytes(_paths._log);
  expect_error(recorded::ProducerLog::create(_paths, _options), ErrorCode::Conflict);
  EXPECT_EQ(file_bytes(_paths._log), original);
}

TEST_F(ProducerLogTest, LocalIdIssuanceDoesNotWriteTheReservedLog)
{
  create();
  auto producer = writer();
  const auto claim = require_value(producer.claim());
  const auto before = file_bytes(_paths._log);
  const auto first = require_value(producer.next_publication_id());
  const auto second = require_value(producer.next_publication_id());
  EXPECT_EQ(first._producer, claim);
  EXPECT_EQ(first._sequence, 1u);
  EXPECT_EQ(second._sequence, 2u);
  EXPECT_NE(claim._incarnation, 0u);
  EXPECT_EQ(file_bytes(_paths._log), before);
  EXPECT_EQ(require_value(producer.record_count()), 0u);
}

TEST_F(ProducerLogTest, AppendCommitsCompleteHeadersPayloadAndIndependentQueueSequences)
{
  create();
  auto producer = writer();
  EXPECT_EQ(require_value(producer.queue_sequence(QueueId{4})), 0u);
  EXPECT_EQ(require_value(producer.queue_sequence(QueueId{5})), 0u);
  const auto firstId = require_value(producer.next_publication_id());
  const auto request = support::producer_append_request(firstId, payload);
  const auto first = require_value(producer.append(request));
  EXPECT_FALSE(first._duplicate);
  EXPECT_EQ(first._recordOrdinal, 1u);
  EXPECT_EQ(first._queueSequence, 1u);
  const auto secondId = require_value(producer.next_publication_id());
  const auto second = require_value(producer.append(
      support::producer_append_request(secondId, payload, QueueId{5}, UtcTimestamp{})));
  EXPECT_EQ(second._recordOrdinal, 2u);
  EXPECT_EQ(second._queueSequence, 1u);
  const auto third = require_value(producer.append(support::producer_append_request(
      require_value(producer.next_publication_id()), payload)));
  EXPECT_EQ(third._queueSequence, 2u);
  EXPECT_EQ(third._recordOrdinal, 3u);
  EXPECT_EQ(require_value(producer.queue_sequence(QueueId{4})), 2u);
  EXPECT_EQ(require_value(producer.queue_sequence(QueueId{5})), 1u);
  expect_error(producer.queue_sequence(QueueId{999}), ErrorCode::InvalidArgument);
  EXPECT_EQ(require_value(producer.committed_end()), third._reference._end);
  EXPECT_EQ(std::filesystem::file_size(_paths._log), _options._header._fileBytes);

  const auto view = require_value(producer.view(first._reference));
  auto expected = request._message._header;
  expected._publication = firstId;
  expected._publishedAt = request._publishedAt;
  EXPECT_EQ(view._header, expected);
  EXPECT_TRUE(std::equal(view._payload.begin(), view._payload.end(), payload.begin(), payload.end()));
  const auto destination = require_value(view.destination_at(0));
  EXPECT_EQ(destination._queue, QueueId{4});
  EXPECT_EQ(destination._queueSequence, 1u);
  EXPECT_EQ(destination._status, DestinationStatus::Accepted);
  EXPECT_EQ(destination._enqueuedAt, request._publishedAt);
  EXPECT_EQ(view._schemaReference, request._schemaReference);
  EXPECT_EQ(view._topologyRevision, request._topologyRevision);
  // Reattachment rebuilds both Queue counters from the shared producer
  // stream, rather than confusing them with its total publication count.
  producer = recorded::ProducerLog{};
  auto restored = writer();
  EXPECT_EQ(require_value(restored.queue_sequence(QueueId{4})), 2u);
  EXPECT_EQ(require_value(restored.queue_sequence(QueueId{5})), 1u);
}

TEST_F(ProducerLogTest, PreparedInputCannotOverridePublicationOrDeliveryIdentity)
{
  create();
  auto producer = writer();
  const auto id = require_value(producer.next_publication_id());
  auto request = support::producer_append_request(id, payload);
  request._message._header._publication = {{{9, 9}, TaskId{7}, 8}, 9};
  request._message._header._publishedAt = UtcTimestamp{std::chrono::nanoseconds{111}};
  request._message._header._queue = QueueId{999};
  request._message._header._queueSequence = 999;
  request._message._header._consumer = TaskClaim{{9, 9}, TaskId{8}, 9};
  request._message._header._enqueuedAt = UtcTimestamp{std::chrono::nanoseconds{222}};
  request._message._header._dequeuedAt = UtcTimestamp{std::chrono::nanoseconds{333}};
  const auto result = require_value(producer.append(request));
  const auto message = require_value(producer.view(result._reference));
  EXPECT_EQ(message._header._publication, id);
  EXPECT_EQ(message._header._publishedAt, request._publishedAt);
  EXPECT_FALSE(message._header._queue);
  EXPECT_FALSE(message._header._consumer);
  EXPECT_FALSE(message._header._enqueuedAt);
  EXPECT_FALSE(message._header._dequeuedAt);
}

TEST_F(ProducerLogTest, IdenticalRetriesReturnTheOriginalAcceptanceWithoutAppending)
{
  create();
  auto producer = writer();
  const auto id = require_value(producer.next_publication_id());
  auto request = support::producer_append_request(id, payload);
  const auto first = require_value(producer.append(request));
  const auto before = file_bytes(_paths._log);
  request._publishedAt = UtcTimestamp{std::chrono::nanoseconds{999}};
  const auto retried = require_value(producer.append(request));
  EXPECT_TRUE(retried._duplicate);
  EXPECT_EQ(retried._reference, first._reference);
  EXPECT_EQ(retried._recordOrdinal, first._recordOrdinal);
  EXPECT_EQ(retried._queueSequence, first._queueSequence);
  EXPECT_EQ(require_value(producer.view(retried._reference))._header._publishedAt,
            UtcTimestamp{std::chrono::nanoseconds{-7}});
  EXPECT_EQ(require_value(producer.record_count()), 1u);
  EXPECT_EQ(file_bytes(_paths._log), before);
  const auto found = require_value(producer.lookup(id));
  ASSERT_TRUE(found);
  EXPECT_EQ(*found, first._reference);
}

TEST_F(ProducerLogTest, ConflictingRetryAndInvalidPreparedInputPreserveAcceptedState)
{
  create();
  auto producer = writer();
  const auto id = require_value(producer.next_publication_id());
  auto request = support::producer_append_request(id, payload);
  const auto committed = require_value(producer.append(request));
  const auto before = file_bytes(_paths._log);
  auto changed = payload;
  changed.front() = std::byte{0xff};
  auto conflict = request;
  conflict._message._payload = changed;
  expect_error(producer.append(conflict), ErrorCode::Conflict);
  conflict = request;
  conflict._message._header._format = PayloadFormat::Text;
  expect_error(producer.append(conflict), ErrorCode::Conflict);
  conflict = request;
  conflict._queue = QueueId{5};
  expect_error(producer.append(conflict), ErrorCode::Conflict);
  conflict._queue = QueueId{999};
  expect_error(producer.append(conflict), ErrorCode::Conflict);
  conflict = request;
  ++conflict._schemaReference;
  expect_error(producer.append(conflict), ErrorCode::Conflict);

  auto invalid = support::producer_append_request(require_value(producer.next_publication_id()), payload);
  ++invalid._message._header._payloadBytes;
  EXPECT_TRUE(std::holds_alternative<Error>(producer.append(invalid)));
  EXPECT_EQ(require_value(producer.committed_end()), committed._reference._end);
  EXPECT_EQ(require_value(producer.record_count()), 1u);
  EXPECT_EQ(file_bytes(_paths._log), before);
}

TEST_F(ProducerLogTest, UnsentOlderIdsCannotBeAcceptedAfterANewerId)
{
  create();
  auto producer = writer();
  const auto older = require_value(producer.next_publication_id());
  const auto newer = require_value(producer.next_publication_id());
  const auto accepted = require_value(producer.append(support::producer_append_request(newer, payload)));
  EXPECT_TRUE(std::holds_alternative<Error>(producer.append(support::producer_append_request(older, payload))));
  EXPECT_FALSE(require_value(producer.lookup(older)));
  EXPECT_EQ(require_value(producer.committed_end()), accepted._reference._end);
  EXPECT_EQ(require_value(producer.record_count()), 1u);
}

TEST_F(ProducerLogTest, ByteAndCountExhaustionRejectWhileKnownRetriesStillResolve)
{
  for (const bool byteLimited : {false, true})
  {
    SCOPED_TRACE(byteLimited);
    support::ProducerLogDirectory directory;
    const auto paths = support::producer_log_paths(directory.path());
    const auto options = byteLimited ? support::producer_log_options(4096 + 384, 2) :
                                       support::producer_log_options(16384, 1);
    require_value(recorded::ProducerLog::create(paths, options));
    auto producer = require_value(recorded::ProducerLog::open_writer(paths, options));
    std::array<std::byte, 64> body{};
    body.fill(std::byte{0x5a});
    const auto id = require_value(producer.next_publication_id());
    const auto request = support::producer_append_request(id, body);
    const auto first = require_value(producer.append(request));
    const auto before = file_bytes(paths._log);
    const auto rejected = support::producer_append_request(require_value(producer.next_publication_id()), body);
    expect_error(producer.append(rejected), ErrorCode::LogFull);
    EXPECT_EQ(file_bytes(paths._log), before);
    EXPECT_EQ(std::filesystem::file_size(paths._log), options._header._fileBytes);
    const auto retry = require_value(producer.append(request));
    EXPECT_TRUE(retry._duplicate);
    EXPECT_EQ(retry._reference, first._reference);
  }
}

TEST_F(ProducerLogTest, ReopeningAdvancesIncarnationAndRebuildsAcceptanceAndQueueOrder)
{
  create();
  PublicationId accepted;
  PublicationId unaccepted;
  recorded::ProducerAppendResult first;
  {
    auto producer = writer();
    accepted = require_value(producer.next_publication_id());
    first = require_value(producer.append(support::producer_append_request(accepted, payload)));
    unaccepted = require_value(producer.next_publication_id());
  }
  auto replacement = writer();
  // Queue credit uses recovered accepted progress, not the new claim's local
  // publication-ID counter or a scan performed by each credit check.
  EXPECT_EQ(require_value(replacement.queue_sequence(QueueId{4})), 1u);
  EXPECT_EQ(require_value(replacement.queue_sequence(QueueId{5})), 0u);
  const auto next = require_value(replacement.next_publication_id());
  EXPECT_GT(next._producer._incarnation, accepted._producer._incarnation);
  EXPECT_EQ(next._sequence, 1u);
  EXPECT_EQ(require_value(replacement.lookup(accepted)), std::optional{first._reference});
  EXPECT_FALSE(require_value(replacement.lookup(unaccepted)));
  EXPECT_TRUE(std::holds_alternative<Error>(replacement.append(support::producer_append_request(unaccepted, payload))));
  const auto duplicate = require_value(replacement.append(support::producer_append_request(accepted, payload)));
  EXPECT_TRUE(duplicate._duplicate);
  EXPECT_EQ(duplicate._reference, first._reference);
  const auto second = require_value(replacement.append(support::producer_append_request(next, payload)));
  EXPECT_EQ(second._recordOrdinal, 2u);
  EXPECT_EQ(second._queueSequence, 2u);
  EXPECT_EQ(require_value(replacement.queue_sequence(QueueId{4})), 2u);
}

TEST_F(ProducerLogTest, ExhaustedOrRolledBackIncarnationsFailWithoutChangingEitherFile)
{
  for (const bool exhausted : {true, false})
  {
    SCOPED_TRACE(exhausted);
    support::ProducerLogDirectory directory;
    const auto paths = support::producer_log_paths(directory.path());
    require_value(recorded::ProducerLog::create(paths, _options));
    if (!exhausted)
    {
      auto producer = require_value(recorded::ProducerLog::open_writer(paths, _options));
      require_value(producer.append(support::producer_append_request(
          require_value(producer.next_publication_id()), payload)));
    }
    // No endpoint is live. A native atomic edit models an exhausted counter or
    // damaged ownership history; recovery must never wrap or discard acceptance.
    constexpr std::uint64_t ownerIncarnationOffset = 256;
    corrupt_control(paths._owner, ownerIncarnationOffset,
                    exhausted ? std::numeric_limits<std::uint64_t>::max() : 0);
    const auto ownerBefore = file_bytes(paths._owner);
    const auto logBefore = file_bytes(paths._log);
    expect_error(recorded::ProducerLog::open_writer(paths, _options),
                 exhausted ? ErrorCode::CapacityExceeded : ErrorCode::IoError);
    EXPECT_EQ(file_bytes(paths._owner), ownerBefore);
    EXPECT_EQ(file_bytes(paths._log), logBefore);
  }
}

TEST_F(ProducerLogTest, ReplayCutsStayFrozenAndOwningCopiesSurviveClosedEndpoints)
{
  create();
  recorded::OwnedPublicationRecord saved;
  MessageHeader expected;
  {
    auto producer = writer();
    const auto first = require_value(producer.append(support::producer_append_request(
        require_value(producer.next_publication_id()), payload)));
    auto replay = reader();
    auto oldCut = require_value(replay.capture());
    EXPECT_EQ(oldCut.record_count(), 1u);
    const auto borrowed = require_value(replay.view(first._reference));
    expected = borrowed._header;
    const auto* originalAddress = borrowed._payload.data();
    std::array<std::byte, 4> nextPayload{std::byte{9}, std::byte{8}, std::byte{7}, std::byte{6}};
    require_value(producer.append(support::producer_append_request(
        require_value(producer.next_publication_id()), nextPayload)));
    const auto oldMessage = require_value(replay.next(oldCut));
    ASSERT_TRUE(oldMessage);
    EXPECT_EQ(oldMessage->_header, expected);
    EXPECT_FALSE(require_value(replay.next(oldCut)));
    EXPECT_EQ(borrowed._payload.data(), originalAddress);
    EXPECT_TRUE(std::equal(borrowed._payload.begin(), borrowed._payload.end(), payload.begin(), payload.end()));
    auto newCut = require_value(replay.capture());
    EXPECT_EQ(newCut.record_count(), 2u);
    EXPECT_TRUE(require_value(replay.next(newCut)));
    EXPECT_TRUE(require_value(replay.next(newCut)));
    EXPECT_FALSE(require_value(replay.next(newCut)));
    EXPECT_TRUE(std::holds_alternative<Error>(replay.copy_record(first._reference, payload.size() - 1)));
    saved = require_value(replay.copy_record(first._reference, payload.size()));
  }
  EXPECT_EQ(saved._header, expected);
  EXPECT_EQ(saved._catalogRevision, _options._header._catalogRevision);
  EXPECT_EQ(saved._schemaReference, 4096u);
  EXPECT_EQ(saved._topologyRevision, 9u);
  ASSERT_EQ(saved._destinations.size(), 1u);
  EXPECT_EQ(saved._destinations.front()._queueSequence, 1u);
  EXPECT_TRUE(std::equal(saved._payload.begin(), saved._payload.end(), payload.begin(), payload.end()));
}

TEST_F(ProducerLogTest, ReplayRejectsForgedRecordBoundariesAndForeignPhysicalCursors)
{
  create();
  auto producer = writer();
  const auto first = require_value(producer.append(support::producer_append_request(
      require_value(producer.next_publication_id()), payload)));
  auto replay = reader();
  auto cut = require_value(replay.capture());
  auto forged = first._reference;
  forged._offset += recorded::publication_alignment;
  EXPECT_TRUE(std::holds_alternative<Error>(replay.view(forged)));
  const auto otherPaths = support::producer_log_paths(_directory.path(), "other");
  require_value(recorded::ProducerLog::create(otherPaths, _options));
  auto other = require_value(recorded::ProducerLogReader::open(otherPaths._log, _options));
  EXPECT_TRUE(std::holds_alternative<Error>(other.next(cut)));
  expect_error(other.poll_next(cut), ErrorCode::InvalidArgument);
  recorded::ProducerReplayCursor unbound;
  expect_error(replay.poll_next(unbound), ErrorCode::InvalidArgument);
  const auto valid = require_value(replay.poll_next(cut));
  ASSERT_TRUE(valid);
  EXPECT_EQ(valid->_reference, first._reference);
}

TEST_F(ProducerLogTest, LivePollingValidatesOneNewRecordAndPreservesOtherFrozenCuts)
{
  create();
  auto producer = writer();
  auto replay = reader();
  auto live = require_value(replay.capture());
  auto frozen = live;
  EXPECT_FALSE(require_value(replay.poll_next(live)));
  EXPECT_EQ(live.record_count(), 0u);
  EXPECT_EQ(live.committed_end(), recorded::producer_header_bytes);

  std::array<recorded::ProducerAppendResult, 3> accepted;
  for (std::size_t index = 0; index != accepted.size(); ++index)
  {
    const auto queue = index == 1 ? QueueId{5} : QueueId{4};
    accepted[index] = require_value(producer.append(support::producer_append_request(
        require_value(producer.next_publication_id()), payload, queue)));
  }
  EXPECT_EQ(require_value(replay.committed_end()), accepted.back()._reference._end);
  const auto original = file_bytes(_paths._log);
  for (std::size_t index = 0; index != accepted.size(); ++index)
  {
    const auto record = require_value(replay.poll_next(live));
    ASSERT_TRUE(record);
    EXPECT_EQ(record->_reference, accepted[index]._reference);
    EXPECT_EQ(record->_recordOrdinal, index + 1);
    EXPECT_EQ(live.record_count(), index + 1);
    EXPECT_EQ(live.committed_end(), accepted[index]._reference._end);
    // A later accepted record must remain unindexed until its own poll. The
    // full atomic tail is visible, but does not make this an unbounded capture.
    if (index + 1 != accepted.size())
    {
      expect_error(replay.view(accepted[index + 1]._reference), ErrorCode::NotFound);
    }
    EXPECT_FALSE(require_value(replay.next(live)));
    EXPECT_FALSE(require_value(replay.next(frozen)));
  }
  EXPECT_EQ(frozen.record_count(), 0u);
  EXPECT_EQ(frozen.committed_end(), recorded::producer_header_bytes);
  EXPECT_FALSE(require_value(replay.poll_next(live)));
  EXPECT_FALSE(require_value(replay.poll_next(live)));
  EXPECT_EQ(file_bytes(_paths._log), original);
}

TEST_F(ProducerLogTest, LivePollingReusesValidatedHistoryWithoutExtendingOtherCuts)
{
  create();
  auto producer = writer();
  std::array<recorded::ProducerAppendResult, 4> accepted;
  for (std::size_t index = 0; index != 2; ++index)
  {
    accepted[index] = require_value(producer.append(support::producer_append_request(
        require_value(producer.next_publication_id()), payload)));
  }
  auto replay = reader();
  auto live = require_value(replay.capture());
  auto frozen = live;
  for (std::size_t index = 2; index != accepted.size(); ++index)
  {
    accepted[index] = require_value(producer.append(support::producer_append_request(
        require_value(producer.next_publication_id()), payload)));
  }
  // Polling existing entries does not inspect the new suffix or enlarge the
  // caller's original cut. The ordinary next() operation remains frozen.
  for (std::size_t index = 0; index != 2; ++index)
  {
    const auto polled = require_value(replay.poll_next(live));
    ASSERT_TRUE(polled);
    EXPECT_EQ(polled->_reference, accepted[index]._reference);
    EXPECT_EQ(live.record_count(), 2u);
    expect_error(replay.view(accepted[2]._reference), ErrorCode::NotFound);
    const auto replayed = require_value(replay.next(frozen));
    ASSERT_TRUE(replayed);
    EXPECT_EQ(replayed->_reference, accepted[index]._reference);
  }
  EXPECT_FALSE(require_value(replay.next(frozen)));

  // A different capture may validate ahead. Live extension still exposes only
  // one extra entry at a time, reusing that already validated index directly.
  auto full = require_value(replay.capture());
  EXPECT_EQ(full.record_count(), accepted.size());
  for (std::size_t index = 2; index != accepted.size(); ++index)
  {
    const auto polled = require_value(replay.poll_next(live));
    ASSERT_TRUE(polled);
    EXPECT_EQ(polled->_reference, accepted[index]._reference);
    EXPECT_EQ(live.record_count(), index + 1);
    EXPECT_EQ(live.committed_end(), accepted[index]._reference._end);
    EXPECT_FALSE(require_value(replay.next(frozen)));
  }
  EXPECT_FALSE(require_value(replay.poll_next(live)));
}

TEST_F(ProducerLogTest, LivePollingFailureRemainsClosedAfterTheDamagedRecordIsRepaired)
{
  create();
  auto producer = writer();
  const auto first = require_value(producer.append(support::producer_append_request(
      require_value(producer.next_publication_id()), payload)));
  auto replay = reader();
  auto live = require_value(replay.capture());
  auto frozen = live;
  const auto initial = require_value(replay.poll_next(live));
  ASSERT_TRUE(initial);
  EXPECT_EQ(initial->_reference, first._reference);
  const auto second = require_value(producer.append(support::producer_append_request(
      require_value(producer.next_publication_id()), payload)));
  const auto third = require_value(producer.append(support::producer_append_request(
      require_value(producer.next_publication_id()), payload)));
  producer = recorded::ProducerLog{};
  auto file = require_value(mapped::NativeFile::open_existing(_paths._log, mapped::FileAccess::ReadWrite));
  std::array<std::byte, 1> savedMagic{};
  require_value(file.read_exact(third._reference._offset, savedMagic));
  const std::array damagedMagic{std::byte{0xff}};
  // Deliberate serialized fault injection, with the producer closed and no
  // concurrent reader operation. Atomic controls and the valid prefix remain.
  require_value(file.write_exact(third._reference._offset, damagedMagic));
  const auto valid = require_value(replay.poll_next(live));
  ASSERT_TRUE(valid);
  EXPECT_EQ(valid->_reference, second._reference);
  EXPECT_EQ(live.record_count(), 2u);
  expect_error(replay.poll_next(live), ErrorCode::IoError);
  require_value(file.write_exact(third._reference._offset, savedMagic));
  expect_error(replay.poll_next(live), ErrorCode::IoError);
  expect_error(replay.capture(), ErrorCode::IoError);
  expect_error(replay.next(frozen), ErrorCode::IoError);
  expect_error(replay.view(first._reference), ErrorCode::IoError);
  expect_error(replay.view(second._reference), ErrorCode::IoError);
  EXPECT_EQ(live.record_count(), 2u);
  EXPECT_EQ(live.committed_end(), second._reference._end);

  auto reopened = reader();
  auto repaired = require_value(reopened.capture());
  EXPECT_EQ(repaired.record_count(), 3u);
  for (const auto& accepted : {first, second, third})
  {
    const auto record = require_value(reopened.next(repaired));
    ASSERT_TRUE(record);
    EXPECT_EQ(record->_reference, accepted._reference);
  }
  EXPECT_FALSE(require_value(reopened.next(repaired)));
}

TEST_F(ProducerLogTest, AnExistingWriterCannotBeClaimedByAnotherOpenOrExecProcess)
{
  create();
  {
    auto producer = writer();
    const auto before = file_bytes(_paths._owner);
    expect_error(recorded::ProducerLog::open_writer(_paths, _options), ErrorCode::OwnershipBusy);
    EXPECT_EQ(file_bytes(_paths._owner), before);
  }
  support::NativeProcess worker({ANGARIA_PRODUCER_LOG_WORKER, _directory.path().string(), "hold"});
  const auto claimed = worker_claim(worker);
  EXPECT_NE(claimed._incarnation, 0u);
  expect_error(recorded::ProducerLog::open_writer(_paths, _options), ErrorCode::OwnershipBusy);
  worker.proceed();
  worker.wait_success();
  auto successor = writer();
  EXPECT_GT(require_value(successor.claim())._incarnation, claimed._incarnation);
}

TEST_F(ProducerLogTest, AForeignThreadCannotUseTheWriterOrChangeItsCommit)
{
  create();
  auto producer = writer();
  const auto request = support::producer_append_request(require_value(producer.next_publication_id()), payload);
  std::optional<ErrorCode> claimError;
  std::optional<ErrorCode> appendError;
  std::thread other([&]
  {
    const auto claim = producer.claim();
    if (const auto* error = std::get_if<Error>(&claim))
    {
      claimError = error->_code;
    }
    const auto appended = producer.append(request);
    if (const auto* error = std::get_if<Error>(&appended))
    {
      appendError = error->_code;
    }
  });
  other.join();
  EXPECT_EQ(claimError, ErrorCode::WrongOwner);
  EXPECT_EQ(appendError, ErrorCode::WrongOwner);
  EXPECT_EQ(require_value(producer.record_count()), 0u);
  EXPECT_EQ(require_value(producer.committed_end()), recorded::producer_header_bytes);
  EXPECT_FALSE(require_value(producer.append(request))._duplicate);
}

TEST_F(ProducerLogTest, AlternateOwnerPathsAndHardLinkedDataCannotBypassAnActiveWriter)
{
  create();
  auto producer = writer();
  auto alias = _paths;
  alias._owner = _directory.path() / "copied.owner";
  std::filesystem::copy_file(_paths._owner, alias._owner);
  const auto copied = file_bytes(alias._owner);
  // An alternate owner argument cannot bypass the data inode's lifetime claim.
  expect_error(recorded::ProducerLog::open_writer(alias, _options), ErrorCode::OwnershipBusy);
  // Hardlinked coordination resources are unsupported. Reject them before
  // attachment rather than treating an alias as another producer resource.
  alias._log = _directory.path() / "alias.log";
  std::filesystem::create_hard_link(_paths._log, alias._log);
  expect_error(recorded::ProducerLog::open_writer(alias, _options), ErrorCode::IoError);
  EXPECT_EQ(file_bytes(alias._owner), copied);
  EXPECT_EQ(require_value(producer.record_count()), 0u);
}

TEST_F(ProducerLogTest, AnOwnerFromAnotherPhysicalLogRejectsBeforeReattachment)
{
  create();
  const auto otherPaths = support::producer_log_paths(_directory.path(), "other");
  require_value(recorded::ProducerLog::create(otherPaths, _options));
  auto wrongPair = _paths;
  wrongPair._owner = otherPaths._owner;
  const auto before = file_bytes(otherPaths._owner);
  expect_error(recorded::ProducerLog::open_writer(wrongPair, _options), ErrorCode::ConfigurationMismatch);
  EXPECT_EQ(file_bytes(otherPaths._owner), before);
}

TEST_F(ProducerLogTest, ForkedInheritedHandlesRejectWithoutUnlockingTheParent)
{
  create();
  auto producer = writer();
  auto replay = reader();
  const auto child = ::fork();
  ASSERT_GE(child, 0);
  if (child == 0)
  {
    ::alarm(3);
    const bool writerRejected = support::has_error(producer.claim(), ErrorCode::WrongOwner);
    const bool readerRejected = support::has_error(replay.committed_end(), ErrorCode::WrongOwner);
    // Destruction in the child must never unlock the parent's open description.
    producer = recorded::ProducerLog{};
    replay = recorded::ProducerLogReader{};
    ::_exit((writerRejected ? 1 : 0) | (readerRejected ? 2 : 0));
  }
  const auto status = support::wait_for_exit(child, "Cannot reap inherited-handle child", "Inherited-handle child deadline");
  ASSERT_EQ(support::exit_code(status), 3);
  expect_error(recorded::ProducerLog::open_writer(_paths, _options), ErrorCode::OwnershipBusy);
  EXPECT_FALSE(require_value(producer.append(support::producer_append_request(
      require_value(producer.next_publication_id()), payload)))._duplicate);
}

TEST_F(ProducerLogTest, AStillLiveForkChildCannotKeepTheEndedParentsLease)
{
  create();
  auto producer = writer();
  const auto previous = require_value(producer.claim());
  PausedForkChild child;
  producer = recorded::ProducerLog{};
  // No exec or inherited-handle destruction happens in the paused child. Only
  // atfork closure of the separate claim descriptor can release this lease.
  auto replacement = writer();
  EXPECT_TRUE(child.alive());
  EXPECT_GT(require_value(replacement.claim())._incarnation, previous._incarnation);
}

TEST_F(ProducerLogTest, ObserverErrorsRespectTheSingleCommitAuthority)
{
  for (const auto point : {recorded::ProducerLogCheckpoint::PartialMaterialization,
                           recorded::ProducerLogCheckpoint::BeforeCommit,
                           recorded::ProducerLogCheckpoint::AfterCommit})
  {
    SCOPED_TRACE(static_cast<int>(point));
    support::ProducerLogDirectory directory;
    const auto paths = support::producer_log_paths(directory.path());
    const auto options = support::producer_log_options();
    require_value(recorded::ProducerLog::create(paths, options));
    ErrorOnce observer{point};
    auto producer = require_value(recorded::ProducerLog::open_writer(paths, options, &observer));
    const auto id = require_value(producer.next_publication_id());
    const auto request = support::producer_append_request(id, payload);
    const bool committed = point == recorded::ProducerLogCheckpoint::AfterCommit;
    expect_error(producer.append(request), committed ? ErrorCode::OutcomeUnknown : ErrorCode::IoError);
    EXPECT_TRUE(observer.fired());
    EXPECT_EQ(require_value(producer.record_count()), committed ? 1u : 0u);
    EXPECT_EQ(require_value(producer.queue_sequence(QueueId{4})), committed ? 1u : 0u);
    EXPECT_EQ(require_value(producer.lookup(id)).has_value(), committed);
    auto replay = require_value(recorded::ProducerLogReader::open(paths._log, options));
    auto cut = require_value(replay.capture());
    EXPECT_EQ(cut.record_count(), committed ? 1u : 0u);
    const auto retry = require_value(producer.append(request));
    EXPECT_EQ(retry._duplicate, committed);
    EXPECT_EQ(retry._recordOrdinal, 1u);
    EXPECT_EQ(retry._queueSequence, 1u);
  }
}

TEST_F(ProducerLogTest, KilledProvisionalAppendPreservesThePrefixAndReusesOnlyItsSuffix)
{
  for (const std::string mode : {"partial", "before"})
  {
    SCOPED_TRACE(mode);
    support::ProducerLogDirectory directory;
    const auto paths = support::producer_log_paths(directory.path());
    const auto options = support::producer_log_options();
    require_value(recorded::ProducerLog::create(paths, options));
    PublicationId originalId;
    recorded::ProducerAppendResult original;
    {
      auto seed = require_value(recorded::ProducerLog::open_writer(paths, options));
      originalId = require_value(seed.next_publication_id());
      original = require_value(seed.append(support::producer_append_request(originalId, payload)));
    }
    const auto committedBytes = file_bytes(paths._log);
    support::NativeProcess worker({ANGARIA_PRODUCER_LOG_WORKER, directory.path().string(), mode});
    worker.expect('O');
    const TaskClaim abandoned{options._header._session, options._header._producer, worker.receive<std::uint64_t>()};
    worker.expect('R');
    auto replay = require_value(recorded::ProducerLogReader::open(paths._log, options));
    auto cut = require_value(replay.capture());
    EXPECT_EQ(cut.record_count(), 1u);
    EXPECT_EQ(cut.committed_end(), original._reference._end);
    const auto bytes = file_bytes(paths._log);
    EXPECT_TRUE(std::equal(committedBytes.begin(), committedBytes.begin() + original._reference._end, bytes.begin()));
    EXPECT_TRUE(std::any_of(bytes.begin() + original._reference._end,
                           bytes.begin() + original._reference._end + 8,
                           [](std::byte byte)
                           {
                             return byte != std::byte{};
                           }));
    worker.terminate();
    auto replacement = require_value(recorded::ProducerLog::open_writer(paths, options));
    EXPECT_FALSE(require_value(replacement.lookup(PublicationId{abandoned, 1})));
    const auto id = require_value(replacement.next_publication_id());
    EXPECT_GT(id._producer._incarnation, abandoned._incarnation);
    const auto appended = require_value(replacement.append(support::producer_append_request(id, worker_payload)));
    EXPECT_EQ(appended._reference._offset, original._reference._end);
    EXPECT_EQ(appended._recordOrdinal, 2u);
    EXPECT_EQ(appended._queueSequence, 2u);
    EXPECT_EQ(std::filesystem::file_size(paths._log), options._header._fileBytes);
    EXPECT_EQ(require_value(replacement.view(original._reference))._header._publication, originalId);
    auto recovered = require_value(replay.capture());
    EXPECT_EQ(recovered.record_count(), 2u);
  }
}

TEST_F(ProducerLogTest, ACommittedRecordIsDiscoverableWhileItsLiveWriterHasNotReturned)
{
  create();
  support::NativeProcess worker({ANGARIA_PRODUCER_LOG_WORKER, _directory.path().string(), "after"});
  const auto claim = worker_claim(worker);
  worker.expect('R');
  auto replay = reader();
  auto cut = require_value(replay.capture());
  ASSERT_EQ(cut.record_count(), 1u);
  const auto message = require_value(replay.next(cut));
  ASSERT_TRUE(message);
  EXPECT_EQ(message->_header._publication, (PublicationId{claim, 1}));
  EXPECT_TRUE(std::equal(message->_payload.begin(), message->_payload.end(), worker_payload.begin(), worker_payload.end()));
  // The native claim stays owned while paused; readers do not take it over.
  expect_error(recorded::ProducerLog::open_writer(_paths, _options), ErrorCode::OwnershipBusy);
  worker.proceed();
  worker.expect('A');
  EXPECT_EQ(worker.receive<std::uint64_t>(), 1u);
  EXPECT_EQ(worker.receive<std::uint64_t>(), 1u);
  worker.wait_success();
  auto producer = writer();
  EXPECT_TRUE(require_value(producer.lookup(PublicationId{claim, 1})));
  EXPECT_EQ(require_value(producer.record_count()), 1u);
}

TEST_F(ProducerLogTest, KillingAfterCommitCannotTurnAcceptanceIntoRejection)
{
  create();
  support::NativeProcess worker({ANGARIA_PRODUCER_LOG_WORKER, _directory.path().string(), "after"});
  const auto claim = worker_claim(worker);
  worker.expect('R');
  worker.terminate();
  auto replacement = writer();
  const auto id = PublicationId{claim, 1};
  const auto found = require_value(replacement.lookup(id));
  ASSERT_TRUE(found);
  const auto retry = require_value(replacement.append(support::producer_append_request(id, worker_payload)));
  EXPECT_TRUE(retry._duplicate);
  EXPECT_EQ(retry._reference, *found);
  EXPECT_EQ(require_value(replacement.record_count()), 1u);
  EXPECT_EQ(require_value(replacement.view(*found))._header._publication, id);
}

TEST_F(ProducerLogTest, AStoppedStreamAndHeldAdministrativeNameDoNotBlockAnotherOwner)
{
  create();
  support::NativeProcess worker({ANGARIA_PRODUCER_LOG_WORKER, _directory.path().string(), "before"});
  (void)worker_claim(worker);
  worker.expect('R');
  auto gate = require_value(mapped::NativeFile::create_exclusive(_directory.path() / "mutation.lock", 8));
  const int fd = require_value(gate.native_handle());
  ASSERT_EQ(::flock(fd, LOCK_EX | LOCK_NB), 0);
  const auto otherPaths = support::producer_log_paths(_directory.path(), "independent");
  auto otherOptions = _options;
  otherOptions._header._producer = TaskId{6};
  otherOptions._queues = {QueueId{7}};
  require_value(recorded::ProducerLog::create(otherPaths, otherOptions));
  auto independent = require_value(recorded::ProducerLog::open_writer(otherPaths, otherOptions));
  const auto id = require_value(independent.next_publication_id());
  const auto accepted = require_value(independent.append(support::producer_append_request(id, payload, QueueId{7})));
  EXPECT_EQ(accepted._recordOrdinal, 1u);
  EXPECT_EQ(accepted._queueSequence, 1u);
  auto replay = require_value(recorded::ProducerLogReader::open(otherPaths._log, otherOptions));
  auto cut = require_value(replay.capture());
  EXPECT_EQ(cut.record_count(), 1u);
  EXPECT_EQ(require_value(replay.next(cut))->_header._publication, id);
  worker.terminate();
}

TEST_F(ProducerLogTest, ACorruptCommittedBoundaryOrRecordFailsColdRecovery)
{
  create();
  recorded::ProducerAppendResult accepted;
  {
    auto producer = writer();
    accepted = require_value(producer.append(support::producer_append_request(
        require_value(producer.next_publication_id()), payload)));
  }
  // Aligned but inside a record: shape validation alone must not authorize it.
  corrupt_control(_paths._log, recorded::committed_tail_offset,
                  accepted._reference._offset + recorded::publication_alignment);
  expect_error(recorded::ProducerLogReader::open(_paths._log, _options), ErrorCode::IoError);
  expect_error(recorded::ProducerLog::open_writer(_paths, _options), ErrorCode::IoError);
  corrupt_control(_paths._log, recorded::committed_tail_offset, accepted._reference._end);
  auto file = require_value(mapped::NativeFile::open_existing(_paths._log, mapped::FileAccess::ReadWrite));
  const std::array corrupt{std::byte{0xff}};
  require_value(file.write_exact(accepted._reference._offset, corrupt));
  expect_error(recorded::ProducerLogReader::open(_paths._log, _options), ErrorCode::IoError);
  expect_error(recorded::ProducerLog::open_writer(_paths, _options), ErrorCode::IoError);
}

TEST_F(ProducerLogTest, IncrementalReplayFailureRemainsClosedAfterTheSuffixIsRepaired)
{
  create();
  auto producer = writer();
  const auto first = require_value(producer.append(support::producer_append_request(
      require_value(producer.next_publication_id()), payload)));
  auto replay = reader();
  auto oldCut = require_value(replay.capture());
  ASSERT_EQ(oldCut.record_count(), 1u);
  EXPECT_EQ(require_value(replay.view(first._reference))._recordOrdinal, 1u);

  // Both records are accepted after this reader's last capture. The next
  // capture must install the valid second record before finding damage in the
  // third, so failure leaves partially advanced local index/Queue progress.
  const auto second = require_value(producer.append(support::producer_append_request(
      require_value(producer.next_publication_id()), payload)));
  const auto third = require_value(producer.append(support::producer_append_request(
      require_value(producer.next_publication_id()), payload)));
  producer = recorded::ProducerLog{};
  const auto original = file_bytes(_paths._log);
  auto file = require_value(mapped::NativeFile::open_existing(_paths._log, mapped::FileAccess::ReadWrite));
  std::array<std::byte, 1> savedMagic{};
  require_value(file.read_exact(third._reference._offset, savedMagic));
  const std::array damagedMagic{std::byte{0xff}};
  // Deliberate, serialized fixture corruption: no writer or reader operation
  // runs during the byte edits, and the live atomic tail is never modified.
  require_value(file.write_exact(third._reference._offset, damagedMagic));
  expect_error(replay.capture(), ErrorCode::IoError);
  require_value(file.write_exact(third._reference._offset, savedMagic));
  EXPECT_EQ(file_bytes(_paths._log), original);

  // Repair must not silently reactivate a failed handle or expose any part of
  // its local index. The old cut is deliberately unconsumed; returning its
  // previously valid record would violate the permanent failure guarantee.
  expect_error(replay.capture(), ErrorCode::IoError);
  expect_error(replay.next(oldCut), ErrorCode::IoError);
  expect_error(replay.poll_next(oldCut), ErrorCode::IoError);
  for (const auto& accepted : {first, second, third})
  {
    expect_error(replay.view(accepted._reference), ErrorCode::IoError);
    expect_error(replay.copy_record(accepted._reference, payload.size()), ErrorCode::IoError);
  }
  expect_error(replay.committed_end(), ErrorCode::IoError);

  // Only reopening establishes a newly validated reader. All three accepted
  // records remain intact, including the suffix that was partly indexed.
  auto reopened = reader();
  auto repairedCut = require_value(reopened.capture());
  EXPECT_EQ(repairedCut.record_count(), 3u);
  EXPECT_EQ(repairedCut.committed_end(), third._reference._end);
  for (const auto& accepted : {first, second, third})
  {
    const auto record = require_value(reopened.next(repairedCut));
    ASSERT_TRUE(record);
    EXPECT_EQ(record->_reference, accepted._reference);
    EXPECT_EQ(record->_recordOrdinal, accepted._recordOrdinal);
    EXPECT_EQ(require_value(record->destination_at(0))._queueSequence, accepted._queueSequence);
    EXPECT_TRUE(std::equal(record->_payload.begin(), record->_payload.end(), payload.begin(), payload.end()));
  }
  EXPECT_FALSE(require_value(reopened.next(repairedCut)));
  EXPECT_EQ(file_bytes(_paths._log), original);
}
