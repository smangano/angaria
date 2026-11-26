// Private qualification of the implementation behind QueueWriter::send. The
// public API examples live in test_publication_api.cpp; this suite injects I/O
// failures at actual journal checkpoints and inspects reserved storage directly.

#include "publication_runtime.h"

#include "delivery_progress.h"
#include "delivery_progress_fixture.h"
#include "mapped_payload.h"
#include "result_assertions.h"

#include <gtest/gtest.h>

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <span>
#include <variant>
#include <vector>

namespace
{
  using namespace angaria;
  namespace runtime = angaria::detail;
  namespace mapped = angaria::detail::mapped;
  namespace support = angaria::test_support;

  class PublicationFixture
  {
  public:
    PublicationFixture() : _storage(0)
    {
      // Unlike a synthetic diagnostic claim, attach_runtime_task acquires the
      // real native producer lease. Only fault injection bypasses the wrapper.
      _producer = support::require_value(runtime::attach_runtime_task(_storage._state, _storage._producer));
      _first = support::require_value(runtime::runtime_next_publication_id(_producer));
      _second = support::require_value(runtime::runtime_next_publication_id(_producer));
    }

    MessageView message() const
    {
      MessageHeader header;
      header._schema = _storage._schema;
      header._format = PayloadFormat::Text;
      header._payloadBytes = _payload.size();
      header._flags = 37;
      return MessageView{header, _payload};
    }

    Result<PublicationReport> send(PublicationId id, mapped::JournalFaultInjector* injector = nullptr)
    {
      try
      {
        runtime::RuntimeAccess access(_storage._state, true, injector);
        return runtime::runtime_send(access, _producer->_claim, _storage._queue,
                                       id, message(), _outcomes);
      }
      catch (...)
      {
        return runtime::runtime_caught_error();
      }
    }

    PublicationReport status(PublicationId id)
    {
      runtime::RuntimeAccess access(_storage._state);
      return runtime::runtime_publication_status(access, _producer->_claim, id, _outcomes);
    }

    std::vector<std::uint64_t> queue_words() const
    {
      runtime::RuntimeAccess access(_storage._state);
      return access.read_words(_storage._queueState, 32);
    }

    void expect_one_publication()
    {
      runtime::RuntimeAccess access(_storage._state);
      (void)runtime::runtime_topology(access);
      const auto references = access.record_references(9);
      ASSERT_EQ(references.size(), 1u);
      const auto metadata = runtime::runtime_publication_metadata(access, references.front());
      ASSERT_EQ(metadata._destinations.size(), 1u);
      EXPECT_EQ(metadata._producer, _producer->_claim._task);
      EXPECT_EQ(metadata._producerIncarnation, _producer->_claim._incarnation);
      EXPECT_EQ(metadata._publicationSequence, _first._sequence);
      EXPECT_EQ(metadata._destinations.front()._queue, _storage._queue);
      EXPECT_EQ(metadata._destinations.front()._queueSequence, 1u);
      EXPECT_EQ(metadata._destinations.front()._enqueuedAt, metadata._publishedAt);
      const auto payload = support::require_value(runtime::map_publication_payload(access, references.front()));
      const auto bytes = support::require_value(payload.bytes());
      EXPECT_EQ(std::vector<std::byte>(bytes.begin(), bytes.end()), _payload);
      const auto queue = access.read_words(_storage._queueState, 12);
      EXPECT_EQ(queue[0], 1u);
      EXPECT_EQ(queue[1], metadata._layout._destinationsOffset);
      EXPECT_EQ(queue[9], metadata._layout._destinationsOffset);
    }

    support::DeliveryProgressFixture _storage;
    std::shared_ptr<runtime::TaskState> _producer;
    PublicationId _first;
    PublicationId _second;
    const std::vector<std::byte> _payload{std::byte{0x41}, std::byte{0x00}, std::byte{0x5a}};
    std::array<DestinationOutcome, 1> _outcomes;
  };

  class PublicationIoFailure final : public mapped::JournalFaultInjector
  {
  public:
    PublicationIoFailure(mapped::JournalCheckpoint point, std::uint64_t index = 0,
                             bool persistent = false)
        : _point(point), _index(index), _persistent(persistent)
    {
    }

    OperationResult checkpoint(mapped::JournalCheckpoint point, std::uint64_t index) override
    {
      if (point == _point && index == _index && (_persistent || _failures == 0))
      {
        ++_failures;
        return Error{ErrorCode::IoError, "Injected publication I/O failure", "test.publication"};
      }
      return std::monostate{};
    }

    unsigned _failures{};

  private:
    mapped::JournalCheckpoint _point;
    std::uint64_t _index{};
    bool _persistent{};
  };

  TEST(PublicationRuntimeTest, PrecommitFailuresProveRollbackWithoutAnAcceptedDelivery)
  {
    // Failure before committed authority returns an ordinary error only after
    // recovery proves the original tail, Queue roots, and ring are unchanged.
    for (const auto point : {mapped::JournalCheckpoint::JournalBodyWord,
                            mapped::JournalCheckpoint::Prepared,
                            mapped::JournalCheckpoint::FileGrowth,
                            mapped::JournalCheckpoint::RecordBodyWord,
                            mapped::JournalCheckpoint::RecordReady})
    {
      SCOPED_TRACE(static_cast<unsigned>(point));
      PublicationFixture fixture;
      const auto base = support::journal_snapshot(fixture._storage._directory);
      const auto queue = fixture.queue_words();
      PublicationIoFailure failure(point);
      support::expect_error(fixture.send(fixture._first, &failure), ErrorCode::IoError);
      EXPECT_EQ(failure._failures, 1u);
      EXPECT_EQ(support::journal_snapshot(fixture._storage._directory), base);
      EXPECT_EQ(fixture.queue_words(), queue);
      const auto status = fixture.status(fixture._first);
      EXPECT_EQ(status._error, ErrorCode::None);
      EXPECT_EQ(status._status, PublicationStatus::Rejected);
      EXPECT_TRUE(status._destinations.empty());
      // The same issued ID remains usable because rollback created no message.
      EXPECT_EQ(support::require_value(fixture.send(fixture._first))._status, PublicationStatus::Accepted);
      fixture.expect_one_publication();
    }
  }

  TEST(PublicationRuntimeTest, PostcommitFailuresReturnTheRecoveredAcceptance)
  {
    // Once committed, a one-shot failure must settle to the accepted result.
    // The caller can repeat the same ID without a second Queue sequence.
    for (const auto point : {mapped::JournalCheckpoint::Committed,
                            mapped::JournalCheckpoint::RedoWord,
                            mapped::JournalCheckpoint::RecordCommitted,
                            mapped::JournalCheckpoint::EpilogueWord,
                            mapped::JournalCheckpoint::Applied,
                            mapped::JournalCheckpoint::Idle})
    {
      SCOPED_TRACE(static_cast<unsigned>(point));
      PublicationFixture fixture;
      const auto base = support::journal_snapshot(fixture._storage._directory);
      PublicationIoFailure failure(point);
      const auto accepted = support::require_value(fixture.send(fixture._first, &failure));
      EXPECT_EQ(failure._failures, 1u);
      EXPECT_EQ(accepted._error, ErrorCode::None);
      EXPECT_EQ(accepted._status, PublicationStatus::Accepted);
      fixture.expect_one_publication();
      const auto committed = support::journal_snapshot(fixture._storage._directory);
      EXPECT_EQ(committed._generation, base._generation + 1);
      EXPECT_GT(committed._tail, base._tail);
      EXPECT_EQ(support::require_value(fixture.send(fixture._first))._status, PublicationStatus::Accepted);
      EXPECT_EQ(support::journal_snapshot(fixture._storage._directory), committed);
    }
  }

  TEST(PublicationRuntimeTest, EveryAbsoluteQueueAndRingRedoWordCanBeRecovered)
  {
    // Seven producer-root words, the oldest-pending link, and four ring words
    // form a single acceptance. Partial installation never exposes half a send.
    for (std::uint64_t index = 0; index != 12; ++index)
    {
      SCOPED_TRACE(index);
      PublicationFixture fixture;
      PublicationIoFailure failure(mapped::JournalCheckpoint::RedoWord, index);
      EXPECT_EQ(support::require_value(fixture.send(fixture._first, &failure))._status,
                PublicationStatus::Accepted);
      EXPECT_EQ(failure._failures, 1u);
      fixture.expect_one_publication();
    }
  }

  TEST(PublicationRuntimeTest, UnresolvedPostcommitRepairReturnsUnknownThenStatusSettlesIt)
  {
    // A persistent store failure prevents the operation from proving its result.
    // The ID remains the recovery key; a later clean lookup returns acceptance.
    PublicationFixture fixture;
    PublicationIoFailure failure(mapped::JournalCheckpoint::RedoWord, 0, true);
    support::expect_error(fixture.send(fixture._first, &failure), ErrorCode::OutcomeUnknown);
    EXPECT_GE(failure._failures, 2u);
    const auto status = fixture.status(fixture._first);
    EXPECT_EQ(status._error, ErrorCode::None);
    EXPECT_EQ(status._status, PublicationStatus::Accepted);
    fixture.expect_one_publication();
    const auto committed = support::journal_snapshot(fixture._storage._directory);
    EXPECT_EQ(support::require_value(fixture.send(fixture._first))._status, PublicationStatus::Accepted);
    EXPECT_EQ(support::journal_snapshot(fixture._storage._directory), committed);
  }

  TEST(PublicationRuntimeTest, IdenticalRepeatStillWorksWhenTheLogIsCompletelyFull)
  {
    PublicationFixture fixture;
    const auto first = support::require_value(fixture.send(fixture._first));
    fixture._storage.fill_log();
    const auto full = support::journal_snapshot(fixture._storage._directory);
    EXPECT_EQ(full._tail, fixture._storage._configuration._maxLogBytes);
    const auto repeat = support::require_value(fixture.send(fixture._first));
    EXPECT_EQ(repeat._status, PublicationStatus::Accepted);
    EXPECT_EQ(repeat._topologyRevision, first._topologyRevision);
    support::expect_error(fixture.send(fixture._second), ErrorCode::LogFull);
    EXPECT_EQ(support::journal_snapshot(fixture._storage._directory), full);
    fixture.expect_one_publication();
  }

  TEST(PublicationRuntimeTest, IdenticalRepeatPrecedesFreshOperationGenerationChecks)
  {
    PublicationFixture fixture;
    EXPECT_EQ(support::require_value(fixture.send(fixture._first))._status, PublicationStatus::Accepted);
    // Deliberately exhaust the global operation counter after acceptance. This
    // boundary fixture remains internally consistent: retained generations are
    // older, while a fresh journal operation cannot increment the maximum.
    fixture._storage.write_word(120, std::numeric_limits<std::uint64_t>::max());
    const auto exhausted = support::journal_snapshot(fixture._storage._directory);
    EXPECT_EQ(support::require_value(fixture.send(fixture._first))._status, PublicationStatus::Accepted);
    support::expect_error(fixture.send(fixture._second), ErrorCode::CapacityExceeded);
    EXPECT_EQ(support::journal_snapshot(fixture._storage._directory), exhausted);
    fixture.expect_one_publication();
  }

  TEST(PublicationRuntimeTest, FreshSendReopensPendingProgressAfterThePreviousDeliveryWasAcknowledged)
  {
    // Public receive arrives in step 4.7. These private primitives establish
    // an already acknowledged prefix so send can demonstrate that it preserves
    // the terminal watermark/time while installing the next oldest delivery.
    PublicationFixture fixture;
    support::require_value(fixture.send(fixture._first));
    auto consumer = support::require_value(runtime::attach_runtime_task(
        fixture._storage._state, fixture._storage._consumer));
    const auto acknowledgedAt = UtcTimestamp{std::chrono::nanoseconds{-17}};
    {
      runtime::RuntimeAccess access(fixture._storage._state);
      const auto progress = support::require_value(runtime::runtime_issue_delivery(access,
          consumer->_claim, fixture._storage._source, fixture._storage._queue,
          UtcTimestamp{std::chrono::nanoseconds{-23}}));
      support::require_value(runtime::runtime_acknowledge_delivery(access, progress._token, acknowledgedAt));
    }
    const auto before = fixture.queue_words();
    EXPECT_EQ(before[8], 1u);
    EXPECT_EQ(before[9], 0u);
    EXPECT_EQ(before[10], std::bit_cast<std::uint64_t>(std::int64_t{-17}));
    EXPECT_EQ(before[11], 1u);
    EXPECT_EQ(support::require_value(fixture.send(fixture._second))._status, PublicationStatus::Accepted);
    const auto after = fixture.queue_words();
    EXPECT_EQ(after[0], 2u);
    EXPECT_EQ(after[8], before[8]);
    EXPECT_EQ(after[9], after[1]);
    EXPECT_EQ(after[10], before[10]);
    EXPECT_EQ(after[11], before[11]);
    fixture._storage.validate();
  }
} // namespace
