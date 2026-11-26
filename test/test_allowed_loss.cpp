// Public allowed-loss examples. Real Registry setup and send/receive calls
// establish every message and claim. Private snapshots, counters, and failure
// hooks below qualify stored guarantees without becoming application APIs.
#include "source_fixture.h"

#include "delivery_loss.h"
#include "delivery_words.h"
#include "native_fixture.h"
#include "native_log_budget.h"
#include "result_assertions.h"
#include "source_runtime_state.h"
#include "topology_state.h"

#include <gtest/gtest.h>

#include <array>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <system_error>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

namespace
{
  using namespace angaria;
  namespace runtime = angaria::detail;
  namespace mapped = runtime::mapped;
  namespace support = angaria::test_support;
  namespace fs = std::filesystem;
  using support::expect_error;
  using support::require_value;
  using namespace std::chrono_literals;

  Delivery receive_delivery(SourceQueueSet& source)
  {
    auto result = source.receive();
    if (const auto* error = std::get_if<Error>(&result))
    {
      throw std::runtime_error(error->_path + ": " + error->_message);
    }
    if (!std::holds_alternative<Delivery>(result))
    {
      throw std::runtime_error("Expected an available message after any reported gap");
    }
    return std::get<Delivery>(std::move(result));
  }

  void expect_gap(const ReceiveResult& result, QueueId queue, std::uint64_t first, std::uint64_t last)
  {
    ASSERT_TRUE(std::holds_alternative<DeliveryGap>(result));
    const auto& gap = std::get<DeliveryGap>(result);
    EXPECT_EQ(gap._queue, queue);
    EXPECT_EQ(gap._firstSequence, first);
    EXPECT_EQ(gap._lastSequence, last);
  }

  void expect_receive_error(const ReceiveResult& result, ErrorCode expected)
  {
    ASSERT_TRUE(std::holds_alternative<Error>(result));
    EXPECT_EQ(std::get<Error>(result)._code, expected) << std::get<Error>(result)._message;
  }

  struct ProgressSnapshot
  {
    std::uint64_t _sourceOffset{};
    std::vector<std::uint64_t> _source;
    std::vector<std::uint64_t> _queue;
    std::vector<mapped::Receipt> _receipts;
  };

  ProgressSnapshot progress_snapshot(const support::SourceScenario& scenario)
  {
    const auto options = require_value(scenario._created._session.options());
    runtime::RuntimeAccess access(support::open_runtime_state(options._directory));
    const auto topology = runtime::runtime_topology(access);
    ProgressSnapshot result;
    for (const auto& source : topology._sources)
    {
      if (source._definition._id == scenario._source)
      {
        result._sourceOffset = source._stateOffset;
        result._source = access.read_words(source._stateOffset, 8);
      }
    }
    for (const auto& queue : topology._queues)
    {
      if (queue._definition._id == scenario._queue)
      {
        result._queue = access.read_words(queue._stateOffset, 12);
      }
    }
    for (const auto reference : access.record_references(9))
    {
      const auto publication = runtime::runtime_publication_metadata(access, reference);
      const auto& destination = publication._destinations.front();
      if (destination._queue == scenario._queue)
      {
        result._receipts.push_back(require_value(mapped::decode_receipt_words(
            access.read_words(destination._receiptReference, 16), mapped::ReceiptProfile::AllowLoss)));
      }
    }
    return result;
  }

  class AllowedLossTest : public testing::Test
  {
  protected:
    void SetUp() override
    {
      auto pattern = (fs::temp_directory_path() / "angaria-allowed-loss-XXXXXX").string();
      ASSERT_NE(::mkdtemp(pattern.data()), nullptr);
      _root = pattern;
      _directory = _root / "session";
    }

    void TearDown() override
    {
      std::error_code ignored;
      fs::remove_all(_root, ignored);
    }

    support::SourceScenario create(DeliveryPolicy delivery = DeliveryPolicy::AllowLoss) const
    {
      return support::create_source_scenario(_directory, true, TaskKind::Process, delivery);
    }

    static std::vector<PublicationId> send_values(support::SourceScenario& scenario,
                                                   QueueWriter& writer, std::uint32_t first,
                                                   std::uint32_t last)
    {
      std::vector<PublicationId> ids;
      for (auto value = first; value <= last; ++value)
      {
        ids.push_back(support::send_source_value(scenario, writer, value));
      }
      return ids;
    }

    fs::path _root;
    fs::path _directory;
  };

  TEST_F(AllowedLossTest, ReportsTheOverwrittenPrefixBeforeIssuingACurrentMessage)
  {
    auto scenario = create();
    auto writer = require_value(scenario._created._creator.open_writer(scenario._queue));
    const auto publications = send_values(scenario, writer, 1, 3);
    auto consumer = require_value(scenario._created._session.attach_task(scenario._consumer));
    auto source = require_value(consumer.open_source_set(scenario._source));
    const auto before = support::journal_snapshot(_directory);
    const auto initial = progress_snapshot(scenario);

    // Capacity two retains sequences 2 and 3 in its current window. The lost
    // range is exact, and the gap owns no Source token or payload mapping.
    expect_gap(source.receive(), scenario._queue, 1, 1);
    const auto lost = progress_snapshot(scenario);
    EXPECT_EQ(lost._source, initial._source);
    ASSERT_EQ(lost._receipts.size(), 3u);
    EXPECT_EQ(lost._receipts[0]._state, mapped::ReceiptState::Lost);
    EXPECT_EQ(lost._receipts[0]._presence, 4u);
    EXPECT_EQ(lost._queue[8], 1u);
    const auto after = support::journal_snapshot(_directory);
    EXPECT_EQ(after._tail, before._tail);
    EXPECT_EQ(after._revision, before._revision);
    EXPECT_EQ(after._generation, before._generation + 1);

    // Loss ends a delivery obligation, not its original acceptance. Querying
    // or identically repeating the producer's ID does not recreate delivery.
    EXPECT_EQ(require_value(scenario._created._creator.publication_status(publications[0]))._status,
              AcceptanceStatus::Accepted);
    const auto bytes = require_value(support::source_codec(scenario).encode({std::uint32_t{1}}));
    EXPECT_EQ(require_value(writer.send(publications[0], support::source_message(scenario._schema, bytes)))._status,
              AcceptanceStatus::Accepted);
    EXPECT_EQ(support::journal_snapshot(_directory), after);

    require_value(source.close());
    auto reopened = require_value(consumer.open_source_set(scenario._source));
    const auto first = receive_delivery(reopened);
    EXPECT_EQ(first._message._header._queueSequence, 2u);
    EXPECT_EQ(first._message._header._publication, publications[1]);
    EXPECT_EQ(first._token._issuance, 1u); // The gap did not allocate an issuance.
    require_value(reopened.acknowledge(first._token));
    const auto second = receive_delivery(reopened);
    EXPECT_EQ(second._message._header._queueSequence, 3u);
    require_value(reopened.acknowledge(second._token));
    EXPECT_TRUE(std::holds_alternative<ReceiveTimeout>(reopened.receive()));
  }

  TEST_F(AllowedLossTest, RecoveryPolicyKeepsTheEntireBacklogAfterRingOverwrite)
  {
    auto scenario = create(DeliveryPolicy::RecoverFromLog);
    auto writer = require_value(scenario._created._creator.open_writer(scenario._queue));
    send_values(scenario, writer, 1, 6);
    auto consumer = require_value(scenario._created._session.attach_task(scenario._consumer));
    auto source = require_value(consumer.open_source_set(scenario._source));
    for (std::uint64_t sequence = 1; sequence != 7; ++sequence)
    {
      const auto next = receive_delivery(source);
      EXPECT_EQ(next._message._header._queueSequence, sequence);
      require_value(source.acknowledge(next._token));
    }
    EXPECT_TRUE(std::holds_alternative<ReceiveTimeout>(source.receive()));
  }

  TEST_F(AllowedLossTest, AnIssuedPinSurvivesOverwriteAndReleasedHistoryKeepsItsTagsWhenLost)
  {
    auto scenario = create();
    auto writer = require_value(scenario._created._creator.open_writer(scenario._queue));
    support::send_source_value(scenario, writer, 1);
    auto consumer = require_value(scenario._created._session.attach_task(scenario._consumer));
    auto source = require_value(consumer.open_source_set(scenario._source));
    const auto first = receive_delivery(source);
    const auto header = first._message._header;
    const auto address = first._message._payload.data();
    const auto saved = require_value(first._message.copy_message(4));
    send_values(scenario, writer, 2, 5);
    const auto stable = support::recovered_image(_directory);
    const auto repeated = receive_delivery(source);
    EXPECT_EQ(repeated._token, first._token);
    EXPECT_EQ(repeated._message._header, header);
    EXPECT_EQ(repeated._message._payload.data(), address);
    EXPECT_EQ(support::recovered_image(_directory), stable);

    require_value(source.release(first._token));
    expect_gap(source.receive(), scenario._queue, 1, 3);
    const auto lost = progress_snapshot(scenario);
    EXPECT_EQ(lost._source[0], 1u);
    EXPECT_EQ(lost._receipts[0]._state, mapped::ReceiptState::Lost);
    EXPECT_EQ(lost._receipts[0]._presence, 5u);
    EXPECT_EQ(lost._receipts[0]._consumerTask, first._token._consumer._task);
    EXPECT_EQ(lost._receipts[0]._issuance, first._token._issuance);
    EXPECT_EQ(lost._receipts[0]._dequeuedAt, *header._dequeuedAt);
    EXPECT_EQ(lost._receipts[0]._acknowledgedAt, UtcTimestamp{});
    EXPECT_EQ(lost._receipts[1]._presence, 4u);
    expect_error(source.acknowledge(first._token), ErrorCode::StaleDelivery);
    // The independent copy still describes the old delivery, even though its
    // native receipt is now terminal Lost and the original borrow has ended.
    EXPECT_EQ(saved._header, header);
    EXPECT_EQ(std::get<std::uint32_t>(require_value(saved.decode(support::source_codec(scenario))).front()), 1u);
    const auto current = receive_delivery(source);
    EXPECT_EQ(current._message._header._queueSequence, 4u);
    EXPECT_EQ(current._token._issuance, 2u);
    require_value(source.acknowledge(current._token));
  }

  TEST_F(AllowedLossTest, LossPreservesTheLastActualAcknowledgmentTimeAndReceipt)
  {
    auto scenario = create();
    auto writer = require_value(scenario._created._creator.open_writer(scenario._queue));
    support::send_source_value(scenario, writer, 1);
    auto consumer = require_value(scenario._created._session.attach_task(scenario._consumer));
    auto source = require_value(consumer.open_source_set(scenario._source));
    const auto first = receive_delivery(source);
    require_value(source.acknowledge(first._token));
    const auto acknowledged = progress_snapshot(scenario);
    send_values(scenario, writer, 2, 5);
    expect_gap(source.receive(), scenario._queue, 2, 3);
    const auto lost = progress_snapshot(scenario);
    EXPECT_EQ(lost._queue[8], 3u);
    EXPECT_EQ(lost._queue[10], acknowledged._queue[10]);
    EXPECT_EQ(lost._queue[11], acknowledged._queue[11]);
    EXPECT_EQ(lost._receipts[0], acknowledged._receipts[0]);
    EXPECT_EQ(lost._receipts[1]._state, mapped::ReceiptState::Lost);
    EXPECT_EQ(lost._receipts[2]._state, mapped::ReceiptState::Lost);
    const auto next = receive_delivery(source);
    EXPECT_EQ(next._message._header._queueSequence, 4u);
    require_value(source.acknowledge(next._token));
  }

  TEST_F(AllowedLossTest, ClosingAndReopeningEndsTheOldPinBeforeAnOverwrittenPrefixCanBeLost)
  {
    auto scenario = create();
    auto writer = require_value(scenario._created._creator.open_writer(scenario._queue));
    support::send_source_value(scenario, writer, 1);
    auto consumer = require_value(scenario._created._session.attach_task(scenario._consumer));
    auto source = require_value(consumer.open_source_set(scenario._source));
    const auto first = receive_delivery(source);
    send_values(scenario, writer, 2, 4);
    require_value(source.close());
    auto reopened = require_value(consumer.open_source_set(scenario._source));
    expect_gap(reopened.receive(), scenario._queue, 1, 2);
    const auto next = receive_delivery(reopened);
    EXPECT_EQ(next._message._header._queueSequence, 3u);
    EXPECT_GT(next._token._issuance, first._token._issuance);
    expect_error(reopened.release(first._token), ErrorCode::StaleDelivery);
    require_value(reopened.acknowledge(next._token));
  }

  TEST_F(AllowedLossTest, ConsumerReplacementKeepsLossAndUsesANewClaimForLaterDelivery)
  {
    auto scenario = create();
    auto writer = require_value(scenario._created._creator.open_writer(scenario._queue));
    send_values(scenario, writer, 1, 4);
    TaskClaim oldClaim;
    {
      auto consumer = require_value(scenario._created._session.attach_task(scenario._consumer));
      oldClaim = consumer.claim();
      auto source = require_value(consumer.open_source_set(scenario._source));
      expect_gap(source.receive(), scenario._queue, 1, 2);
      const auto issued = receive_delivery(source);
      EXPECT_EQ(issued._message._header._queueSequence, 3u);
      // Destructor/claim cleanup releases this issuance without undoing loss.
    }
    auto replacement = require_value(scenario._created._session.attach_task(scenario._consumer));
    EXPECT_EQ(replacement.claim()._incarnation, oldClaim._incarnation + 1);
    auto source = require_value(replacement.open_source_set(scenario._source));
    const auto next = receive_delivery(source);
    EXPECT_EQ(next._message._header._queueSequence, 3u);
    EXPECT_EQ(next._token._consumer, replacement.claim());
    require_value(source.acknowledge(next._token));
    EXPECT_EQ(progress_snapshot(scenario)._receipts[0]._state, mapped::ReceiptState::Lost);
  }

  TEST_F(AllowedLossTest, ConsumerReplacementEndsAnOverwrittenPinBeforeReportingItsNewGap)
  {
    auto scenario = create();
    auto writer = require_value(scenario._created._creator.open_writer(scenario._queue));
    support::send_source_value(scenario, writer, 1);
    DeliveryToken oldToken;
    MessageHeader oldHeader;
    {
      auto consumer = require_value(scenario._created._session.attach_task(scenario._consumer));
      auto source = require_value(consumer.open_source_set(scenario._source));
      const auto first = receive_delivery(source);
      oldToken = first._token;
      oldHeader = first._message._header;
      send_values(scenario, writer, 2, 5);
      // The old pin still offers sequence 1 after overwrite. Ending its owning
      // Task releases it; a replacement may then lose this pending prefix.
      EXPECT_EQ(receive_delivery(source)._token, oldToken);
    }
    auto replacement = require_value(scenario._created._session.attach_task(scenario._consumer));
    EXPECT_EQ(replacement.claim()._incarnation, oldToken._consumer._incarnation + 1);
    auto source = require_value(replacement.open_source_set(scenario._source));
    expect_gap(source.receive(), scenario._queue, 1, 3);
    const auto lost = progress_snapshot(scenario);
    EXPECT_EQ(lost._receipts[0]._state, mapped::ReceiptState::Lost);
    EXPECT_EQ(lost._receipts[0]._consumerIncarnation, oldToken._consumer._incarnation);
    EXPECT_EQ(lost._receipts[0]._issuance, oldToken._issuance);
    EXPECT_EQ(lost._receipts[0]._dequeuedAt, *oldHeader._dequeuedAt);
    expect_error(source.acknowledge(oldToken), ErrorCode::StaleDelivery);
    const auto current = receive_delivery(source);
    EXPECT_EQ(current._message._header._queueSequence, 4u);
    EXPECT_EQ(current._token._consumer, replacement.claim());
    EXPECT_EQ(current._token._issuance, 1u); // Each consumer claim has its own counter.
    require_value(source.acknowledge(current._token));
  }

  TEST_F(AllowedLossTest, LossAndSubsequentDeliveryProgressWorkAtTheExactLogCeiling)
  {
    auto scenario = create();
    auto writer = require_value(scenario._created._creator.open_writer(scenario._queue));
    send_values(scenario, writer, 1, 5);
    auto consumer = require_value(scenario._created._session.attach_task(scenario._consumer));
    auto source = require_value(consumer.open_source_set(scenario._source));
    support::fill_log_with_queue_revision(support::open_runtime_state(_directory), scenario._queue,
                                            scenario._created._creator.claim());
    const auto full = support::journal_snapshot(_directory);
    EXPECT_EQ(full._tail, require_value(scenario._created._session.options())._maxLogBytes);
    expect_gap(source.receive(), scenario._queue, 1, 3);
    const auto first = receive_delivery(source);
    require_value(source.release(first._token));
    const auto repeated = receive_delivery(source);
    EXPECT_EQ(repeated._message._header._queueSequence, 4u);
    require_value(source.acknowledge(repeated._token));
    const auto last = receive_delivery(source);
    require_value(source.acknowledge(last._token));
    require_value(source.close());
    const auto after = support::journal_snapshot(_directory);
    EXPECT_EQ(after._tail, full._tail);
    EXPECT_EQ(after._fileBytes, full._fileBytes);
    EXPECT_EQ(after._revision, full._revision);
  }

  TEST_F(AllowedLossTest, MultipleActiveMembersRejectBeforeAnyLossButDraftMembersAreIgnored)
  {
    auto scenario = create();
    auto writer = require_value(scenario._created._creator.open_writer(scenario._queue));
    send_values(scenario, writer, 1, 3);
    auto registry = scenario._created._creator.registry();
    QueueDescriptor other;
    other._name = "OtherUpdates";
    other._producer = scenario._created._creator.claim()._task;
    other._sourceSet = scenario._source;
    other._delivery = DeliveryPolicy::AllowLoss;
    const auto otherId = require_value(registry.register_queue(other));
    auto consumer = require_value(scenario._created._session.attach_task(scenario._consumer));
    auto source = require_value(consumer.open_source_set(scenario._source));
    expect_gap(source.receive(), scenario._queue, 1, 1);
    const auto pinned = receive_delivery(source);
    require_value(registry.activate_queue(otherId));
    const auto repeated = receive_delivery(source);
    EXPECT_EQ(repeated._token, pinned._token);
    require_value(source.release(pinned._token));
    send_values(scenario, writer, 4, 5);
    const auto settled = support::recovered_image(_directory);
    expect_receive_error(source.receive(), ErrorCode::UnsupportedPolicy);
    EXPECT_EQ(support::recovered_image(_directory), settled);
    EXPECT_EQ(progress_snapshot(scenario)._receipts[1]._state, mapped::ReceiptState::Pending);
  }

  TEST_F(AllowedLossTest, NoGapIsReportedWhenPendingMessagesRemainInTheCurrentWindow)
  {
    auto scenario = create();
    auto writer = require_value(scenario._created._creator.open_writer(scenario._queue));
    send_values(scenario, writer, 1, 2);
    auto consumer = require_value(scenario._created._session.attach_task(scenario._consumer));
    auto source = require_value(consumer.open_source_set(scenario._source));
    for (std::uint64_t sequence = 1; sequence != 3; ++sequence)
    {
      const auto next = receive_delivery(source);
      EXPECT_EQ(next._message._header._queueSequence, sequence);
      require_value(source.acknowledge(next._token));
    }
    const auto settled = support::recovered_image(_directory);
    EXPECT_TRUE(std::holds_alternative<ReceiveTimeout>(source.receive()));
    EXPECT_EQ(support::recovered_image(_directory), settled);
    send_values(scenario, writer, 3, 4);
    const auto next = receive_delivery(source);
    EXPECT_EQ(next._message._header._queueSequence, 3u);
    require_value(source.acknowledge(next._token));
  }

  class DelayReadOnce final : public runtime::RuntimeAccessReadObserver
  {
  public:
    OperationResult before_read(std::uint64_t, std::uint64_t) override
    {
      if (_enabled && !_delayed)
      {
        _delayed = true;
        std::this_thread::sleep_for(25ms);
      }
      return std::monostate{};
    }

    bool _enabled{true};
    bool _delayed{};
  };

  TEST_F(AllowedLossTest, InvalidAndExpiredTimeoutsDoNotCommitLoss)
  {
    auto scenario = create();
    auto writer = require_value(scenario._created._creator.open_writer(scenario._queue));
    send_values(scenario, writer, 1, 4);
    auto consumer = require_value(scenario._created._session.attach_task(scenario._consumer));
    DelayReadOnce observer;
    runtime::SourceOperationHooks hooks;
    hooks._reads = &observer;
    auto source = require_value(consumer.open_source_set(scenario._source));
    const auto before = support::recovered_image(_directory);
    expect_receive_error(source.receive(-1ns), ErrorCode::InvalidArgument);
    expect_receive_error(source.receive(std::chrono::nanoseconds::max()), ErrorCode::InvalidArgument);
    runtime::SourceAccess::set_hooks(source, &hooks);
    EXPECT_TRUE(std::holds_alternative<ReceiveTimeout>(source.receive(5ms)));
    runtime::SourceAccess::set_hooks(source, nullptr);
    EXPECT_TRUE(observer._delayed);
    EXPECT_EQ(support::recovered_image(_directory), before);
    expect_gap(source.receive(), scenario._queue, 1, 2);
  }

  TEST_F(AllowedLossTest, ADeadlineThatExpiresInsideLossPreparationDoesNotStartTheMutation)
  {
    auto scenario = create();
    auto writer = require_value(scenario._created._creator.open_writer(scenario._queue));
    send_values(scenario, writer, 1, 4);
    auto consumer = require_value(scenario._created._session.attach_task(scenario._consumer));
    auto source = require_value(consumer.open_source_set(scenario._source));
    const auto task = runtime::runtime_source_task(runtime::SourceAccess::state(source));
    DelayReadOnce observer;
    observer._enabled = false;
    const auto before = support::journal_snapshot(_directory);
    {
      runtime::RuntimeAccess access(task->_session, true, nullptr, &observer);
      observer._enabled = true;
      const auto result = require_value(runtime::runtime_lose_delivery_prefix(access, task,
          scenario._source, scenario._queue, UtcTimestamp{}, std::chrono::steady_clock::now() + 5ms));
      EXPECT_FALSE(result);
      EXPECT_TRUE(observer._delayed);
      EXPECT_EQ(access._snapshot, before);
    }
    EXPECT_EQ(support::journal_snapshot(_directory), before);
    expect_gap(source.receive(), scenario._queue, 1, 2);
  }

  TEST_F(AllowedLossTest, GapProgressNeedsNoFreshSourceIssuanceCounter)
  {
    auto scenario = create();
    auto writer = require_value(scenario._created._creator.open_writer(scenario._queue));
    send_values(scenario, writer, 1, 4);
    auto consumer = require_value(scenario._created._session.attach_task(scenario._consumer));
    auto source = require_value(consumer.open_source_set(scenario._source));
    const auto initial = progress_snapshot(scenario);
    // Deliberately exhaust an idle Source's fresh counter. This private boundary
    // fixture separates gap progress from issuing a new borrowed message.
    support::set_atomic_word(_directory, initial._sourceOffset, std::numeric_limits<std::uint64_t>::max());
    support::set_atomic_word(_directory, initial._sourceOffset + 8, consumer.claim()._incarnation);
    expect_gap(source.receive(), scenario._queue, 1, 2);
    const auto after = progress_snapshot(scenario);
    EXPECT_EQ(after._source[0], std::numeric_limits<std::uint64_t>::max());
    EXPECT_EQ(after._source[2], 0u);
    expect_receive_error(source.receive(), ErrorCode::CapacityExceeded);
    require_value(source.close());
  }

  class LossFailure final : public mapped::JournalFaultInjector
  {
  public:
    LossFailure(mapped::JournalCheckpoint point, bool persistent)
        : _point(point), _persistent(persistent)
    {
    }

    OperationResult checkpoint(mapped::JournalCheckpoint point, std::uint64_t index) override
    {
      if (point == _point && index == 0 && (_persistent || _failures == 0))
      {
        ++_failures;
        return Error{ErrorCode::IoError, "Injected allowed-loss journal failure", "test.loss"};
      }
      return std::monostate{};
    }

    unsigned _failures{};

  private:
    mapped::JournalCheckpoint _point;
    bool _persistent;
  };

  TEST_F(AllowedLossTest, ProvenPrecommitFailureLeavesPendingReceiptsForAnExactRetry)
  {
    auto scenario = create();
    auto writer = require_value(scenario._created._creator.open_writer(scenario._queue));
    send_values(scenario, writer, 1, 4);
    auto consumer = require_value(scenario._created._session.attach_task(scenario._consumer));
    LossFailure failure(mapped::JournalCheckpoint::Prepared, false);
    runtime::SourceOperationHooks hooks;
    hooks._journal = &failure;
    auto source = require_value(consumer.open_source_set(scenario._source));
    const auto before = support::journal_snapshot(_directory);
    const auto original = progress_snapshot(scenario);
    runtime::SourceAccess::set_hooks(source, &hooks);
    expect_receive_error(source.receive(), ErrorCode::IoError);
    runtime::SourceAccess::set_hooks(source, nullptr);
    EXPECT_EQ(failure._failures, 1u);
    const auto after = progress_snapshot(scenario);
    EXPECT_EQ(after._source, original._source);
    EXPECT_EQ(after._queue, original._queue);
    EXPECT_EQ(after._receipts, original._receipts);
    EXPECT_EQ(support::journal_snapshot(_directory), before);
    expect_gap(source.receive(), scenario._queue, 1, 2);
  }

  TEST_F(AllowedLossTest, UncertainCommittedLossCanHideTheGapButCannotResurrectMessages)
  {
    auto scenario = create();
    auto writer = require_value(scenario._created._creator.open_writer(scenario._queue));
    send_values(scenario, writer, 1, 4);
    auto consumer = require_value(scenario._created._session.attach_task(scenario._consumer));
    LossFailure failure(mapped::JournalCheckpoint::RedoWord, true);
    runtime::SourceOperationHooks hooks;
    hooks._journal = &failure;
    auto source = require_value(consumer.open_source_set(scenario._source));
    runtime::SourceAccess::set_hooks(source, &hooks);
    expect_receive_error(source.receive(), ErrorCode::OutcomeUnknown);
    runtime::SourceAccess::set_hooks(source, nullptr);
    EXPECT_GE(failure._failures, 2u);
    // Recovery completes the committed loss. It cannot reproduce a result that
    // the interrupted caller never observed, nor can it offer sequences 1..2.
    const auto current = receive_delivery(source);
    EXPECT_EQ(current._message._header._queueSequence, 3u);
    EXPECT_EQ(current._token._issuance, 1u);
    const auto recovered = progress_snapshot(scenario);
    EXPECT_EQ(recovered._receipts[0]._state, mapped::ReceiptState::Lost);
    EXPECT_EQ(recovered._receipts[1]._state, mapped::ReceiptState::Lost);
    require_value(source.acknowledge(current._token));
  }
} // namespace
