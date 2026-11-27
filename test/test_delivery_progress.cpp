// Private progress examples below the public endpoint API. The shared fixture
// creates real Registry/history relationships, then models a captured consumer
// claim without fabricating an OS lease. Public callers must authenticate their
// Task owner before invoking these gate-bound primitives in later steps.

#include "delivery_progress.h"
#include "delivery_progress_fixture.h"
#include "delivery_words.h"
#include "native_fixture.h"
#include "result_assertions.h"

#include <gtest/gtest.h>

#include <array>
#include <bit>
#include <chrono>
#include <cstdint>
#include <limits>
#include <span>
#include <vector>

namespace
{
  using namespace angaria;
  namespace runtime = angaria::detail;
  namespace mapped = angaria::detail::mapped;
  using angaria::test_support::DeliveryProgressFixture;
  using angaria::test_support::expect_error;
  using angaria::test_support::require_value;
  using angaria::test_support::set_atomic_word;

  UtcTimestamp timestamp(std::int64_t value)
  {
    return UtcTimestamp{std::chrono::nanoseconds{value}};
  }

  runtime::DeliveryProgress issue(DeliveryProgressFixture& fixture, std::int64_t time = 0)
  {
    runtime::RuntimeAccess access(fixture._state);
    return require_value(runtime::runtime_issue_delivery(access, fixture.consumer_claim(),
        fixture._source, fixture._queue, timestamp(time)));
  }

  void acknowledge(DeliveryProgressFixture& fixture, const DeliveryToken& token, std::int64_t time)
  {
    runtime::RuntimeAccess access(fixture._state);
    require_value(runtime::runtime_acknowledge_delivery(access, token, timestamp(time)));
  }

  std::vector<std::uint64_t> state_words(DeliveryProgressFixture& fixture, std::uint64_t offset)
  {
    runtime::RuntimeAccess access(fixture._state);
    return access.read_words(offset, 12);
  }

  TEST(DeliveryProgressTest, IssuesOldestRetainedDeliveryAfterItsRingSlotWasOverwritten)
  {
    // Capacity two caches sequences 2 and 3; acceptance history still owns 1.
    // Issuance changes only reserved receipt/Source words, never log allocation.
    DeliveryProgressFixture fixture(3);
    fixture.mark_consumer_attached();
    const auto before = angaria::test_support::journal_snapshot(fixture._directory);
    const auto progress = issue(fixture);
    EXPECT_EQ(progress._token, (DeliveryToken{fixture.consumer_claim(), fixture._source,
        fixture._queue, 1, 1}));
    EXPECT_EQ(progress._publicationReference, fixture._publications[0]._reference);
    EXPECT_EQ(progress._receiptReference, fixture._publications[0]._receipt);
    EXPECT_EQ(progress._deliveryLink, fixture._publications[0]._link);
    EXPECT_EQ(progress._dequeuedAt, timestamp(0));
    const auto receipt = fixture.receipt();
    EXPECT_EQ(receipt._state, mapped::ReceiptState::Issued);
    EXPECT_EQ(receipt._presence, 1u);
    EXPECT_EQ(receipt._dequeuedAt, timestamp(0));
    EXPECT_EQ(receipt._consumerTask, fixture._consumer);
    const auto after = angaria::test_support::journal_snapshot(fixture._directory);
    EXPECT_EQ(after._tail, before._tail);
    EXPECT_EQ(after._fileBytes, before._fileBytes);
    EXPECT_EQ(after._generation, before._generation + 1);
    fixture.validate();
  }

  TEST(DeliveryProgressTest, RepeatedIssuanceReturnsTheSamePinBeforeFreshSelectionOrCounterChecks)
  {
    DeliveryProgressFixture fixture;
    fixture.mark_consumer_attached();
    // Start at the final counter value. Re-reading the newly issued pin must
    // still work, even though no further fresh issuance could be represented.
    fixture.write_word(fixture._sourceState, std::numeric_limits<std::uint64_t>::max() - 1);
    fixture.write_word(fixture._sourceState + 8, 1);
    const auto first = issue(fixture, -7);
    EXPECT_EQ(first._token._issuance, std::numeric_limits<std::uint64_t>::max());
    const auto before = fixture.image();
    {
      runtime::RuntimeAccess access(fixture._state);
      const auto repeated = require_value(runtime::runtime_issue_delivery(access, fixture.consumer_claim(),
          fixture._source, QueueId::Invalid, timestamp(99)));
      EXPECT_EQ(repeated, first);
    }
    EXPECT_EQ(fixture.image(), before);
  }

  // Receive already validated this topology under its current access. The
  // private overload may reuse it until mutation; a later access validates the
  // resulting pin afresh before either issuance entry point repeats that pin.
  TEST(DeliveryProgressTest, ValidatedTopologyIssuesAndRepeatsTheSameExactPin)
  {
    DeliveryProgressFixture fixture;
    fixture.mark_consumer_attached();
    fixture.write_word(fixture._sourceState, std::numeric_limits<std::uint64_t>::max() - 1);
    fixture.write_word(fixture._sourceState + 8, 1);
    runtime::DeliveryProgress first;
    {
      runtime::RuntimeAccess access(fixture._state);
      const auto topology = runtime::runtime_topology(access);
      const auto before = access._snapshot;
      first = require_value(runtime::runtime_issue_delivery(access, topology, fixture.consumer_claim(),
          fixture._source, fixture._queue, timestamp(-7)));
      EXPECT_EQ(first._token, (DeliveryToken{fixture.consumer_claim(), fixture._source,
          fixture._queue, 1, std::numeric_limits<std::uint64_t>::max()}));
      EXPECT_EQ(first._publicationReference, fixture._publications.front()._reference);
      EXPECT_EQ(first._receiptReference, fixture._publications.front()._receipt);
      EXPECT_EQ(first._deliveryLink, fixture._publications.front()._link);
      EXPECT_EQ(first._dequeuedAt, timestamp(-7));
      EXPECT_EQ(access._snapshot._generation, before._generation + 1);
      EXPECT_EQ(access._snapshot._tail, before._tail);
    }
    const auto beforeRepeat = fixture.image();
    {
      runtime::RuntimeAccess access(fixture._state);
      const auto topology = runtime::runtime_topology(access);
      const auto generation = access._snapshot._generation;
      const auto repeated = require_value(runtime::runtime_issue_delivery(access, topology,
          fixture.consumer_claim(), fixture._source, QueueId::Invalid, timestamp(99)));
      EXPECT_EQ(repeated, first);
      EXPECT_EQ(require_value(runtime::runtime_issue_delivery(access, fixture.consumer_claim(),
          fixture._source, QueueId::Invalid, timestamp(100))), first);
      EXPECT_EQ(access._snapshot._generation, generation);
    }
    EXPECT_EQ(fixture.image(), beforeRepeat);
    fixture.validate();
  }

  // Reusing whole-history validation does not authorize the caller's claim or
  // selection. These rejected inputs leave the same validated state untouched.
  TEST(DeliveryProgressTest, ValidatedTopologyStillChecksConsumerAndSelectedParticipants)
  {
    DeliveryProgressFixture fixture;
    fixture.mark_consumer_attached();
    const auto before = fixture.image();
    auto wrongSession = fixture.consumer_claim();
    ++wrongSession._session._low;
    auto futureClaim = fixture.consumer_claim();
    ++futureClaim._incarnation;
    auto emptyClaim = fixture.consumer_claim();
    emptyClaim._incarnation = 0;
    {
      runtime::RuntimeAccess access(fixture._state);
      const auto topology = runtime::runtime_topology(access);
      expect_error(runtime::runtime_issue_delivery(access, topology, wrongSession,
          fixture._source, fixture._queue, timestamp(0)), ErrorCode::WrongOwner);
      expect_error(runtime::runtime_issue_delivery(access, topology, futureClaim,
          fixture._source, fixture._queue, timestamp(0)), ErrorCode::WrongOwner);
      expect_error(runtime::runtime_issue_delivery(access, topology, emptyClaim,
          fixture._source, fixture._queue, timestamp(0)), ErrorCode::WrongOwner);
      expect_error(runtime::runtime_issue_delivery(access, topology, fixture.consumer_claim(),
          fixture._otherSource, fixture._queue, timestamp(0)), ErrorCode::WrongOwner);
      expect_error(runtime::runtime_issue_delivery(access, topology, fixture.consumer_claim(),
          fixture._source, QueueId::Invalid, timestamp(0)), ErrorCode::NotFound);
    }
    EXPECT_EQ(fixture.image(), before);
  }

  // After issuance changes state, cleanup and another issuance use the generic
  // fresh path. The deliberate receipt corruption qualifies that path; passing
  // the earlier topology after this mutation would violate the private contract.
  TEST(DeliveryProgressTest, GenericIssuanceRevalidatesAfterValidatedIssuanceAndCleanup)
  {
    DeliveryProgressFixture fixture;
    fixture.mark_consumer_attached();
    {
      runtime::RuntimeAccess access(fixture._state);
      const auto topology = runtime::runtime_topology(access);
      const auto first = require_value(runtime::runtime_issue_delivery(access, topology,
          fixture.consumer_claim(), fixture._source, fixture._queue, timestamp(3)));
      require_value(runtime::runtime_release_delivery(access, first._token));
      const auto generation = access._snapshot._generation;
      const auto beforeCorruption = fixture.image();

      // This is an atomic, gate-protected fault seam, not a journaled update.
      const auto reserved = first._receiptReference + 15 * sizeof(std::uint64_t);
      set_atomic_word(fixture._directory, reserved, 1);
      expect_error(runtime::runtime_issue_delivery(access, fixture.consumer_claim(),
          fixture._source, fixture._queue, timestamp(4)), ErrorCode::IoError);
      EXPECT_EQ(access.read_words(120, 1)[0], generation);
      set_atomic_word(fixture._directory, reserved, 0);
      EXPECT_EQ(fixture.image(), beforeCorruption);

      const auto second = require_value(runtime::runtime_issue_delivery(access, fixture.consumer_claim(),
          fixture._source, fixture._queue, timestamp(17)));
      EXPECT_EQ(second._token._sequence, first._token._sequence);
      EXPECT_EQ(second._token._issuance, first._token._issuance + 1);
      EXPECT_EQ(second._dequeuedAt, timestamp(17));
      EXPECT_EQ(access._snapshot._generation, generation + 1);
    }
    fixture.validate();
  }

  TEST(DeliveryProgressTest, ReleaseRetainsHistoryAndRedeliveryGetsAFreshExactToken)
  {
    DeliveryProgressFixture fixture;
    fixture.mark_consumer_attached();
    const auto first = issue(fixture, -9);
    const auto issued = fixture.receipt();
    {
      runtime::RuntimeAccess access(fixture._state);
      require_value(runtime::runtime_release_delivery(access, first._token));
    }
    auto expected = issued;
    expected._state = mapped::ReceiptState::Pending;
    EXPECT_EQ(fixture.receipt(), expected);
    const auto idle = state_words(fixture, fixture._sourceState);
    EXPECT_EQ(idle[0], 1u);
    EXPECT_EQ(idle[1], 1u);
    EXPECT_TRUE(runtime::all_zero(std::span{idle}.subspan(2)));

    const auto second = issue(fixture, 17);
    EXPECT_EQ(second._token._sequence, first._token._sequence);
    EXPECT_EQ(second._token._issuance, first._token._issuance + 1);
    EXPECT_EQ(second._dequeuedAt, timestamp(17));
    const auto before = fixture.image();
    {
      runtime::RuntimeAccess access(fixture._state);
      expect_error(runtime::runtime_release_delivery(access, first._token), ErrorCode::StaleDelivery);
      expect_error(runtime::runtime_acknowledge_delivery(access, first._token, timestamp(2)), ErrorCode::StaleDelivery);
    }
    EXPECT_EQ(fixture.image(), before);
    fixture.validate();
  }

  TEST(DeliveryProgressTest, AcknowledgmentAdvancesQueueOrderWithZeroAndBackwardUtcTimes)
  {
    DeliveryProgressFixture fixture(3);
    fixture.mark_consumer_attached();
    const std::array<std::int64_t, 3> times{0, -17, -19};
    for (std::size_t index = 0; index != times.size(); ++index)
    {
      const auto delivery = issue(fixture, static_cast<std::int64_t>(index));
      acknowledge(fixture, delivery._token, times[index]);
      const auto receipt = fixture.receipt(index);
      EXPECT_EQ(receipt._state, mapped::ReceiptState::Acknowledged);
      EXPECT_EQ(receipt._presence, 7u);
      EXPECT_EQ(receipt._acknowledgedAt, timestamp(times[index]));
      EXPECT_EQ(receipt._terminalAt, receipt._acknowledgedAt);
      const auto queue = state_words(fixture, fixture._queueState);
      EXPECT_EQ(queue[8], index + 1);
      EXPECT_EQ(queue[9], index + 1 == times.size() ? 0 : fixture._publications[index + 1]._link);
      EXPECT_EQ(queue[10], std::bit_cast<std::uint64_t>(times[index]));
      EXPECT_EQ(queue[11], 1u);
      fixture.validate();
    }
    const auto before = fixture.image();
    {
      runtime::RuntimeAccess access(fixture._state);
      expect_error(runtime::runtime_issue_delivery(access, fixture.consumer_claim(),
          fixture._source, fixture._queue, timestamp(5)), ErrorCode::NotFound);
    }
    EXPECT_EQ(fixture.image(), before);
  }

  TEST(DeliveryProgressTest, SourceCleanupPreservesIssuanceContextAndIsAnIdleNoOp)
  {
    DeliveryProgressFixture fixture;
    fixture.mark_consumer_attached();
    const auto delivery = issue(fixture, 4);
    {
      runtime::RuntimeAccess access(fixture._state);
      require_value(runtime::runtime_release_source(access, fixture.consumer_claim(), fixture._source));
    }
    const auto receipt = fixture.receipt();
    EXPECT_EQ(receipt._state, mapped::ReceiptState::Pending);
    EXPECT_EQ(receipt._issuance, delivery._token._issuance);
    EXPECT_EQ(receipt._dequeuedAt, timestamp(4));
    const auto before = fixture.image();
    {
      runtime::RuntimeAccess access(fixture._state);
      require_value(runtime::runtime_release_source(access, fixture.consumer_claim(), fixture._source));
      expect_error(runtime::runtime_acknowledge_delivery(access, delivery._token, timestamp(5)), ErrorCode::StaleDelivery);
    }
    EXPECT_EQ(fixture.image(), before);
    EXPECT_EQ(issue(fixture)._token._issuance, delivery._token._issuance + 1);
  }

  TEST(DeliveryProgressTest, SameGuardCanReleaseAnIssuanceAfterLocalPreparationFails)
  {
    // A future receiver may fail to map/prepare its return value after Issued
    // commits. Immediate exact cleanup must use the completed generation while
    // retaining this same mutation guard; it cannot submit a stale base plan.
    DeliveryProgressFixture fixture;
    fixture.mark_consumer_attached();
    runtime::RuntimeAccess access(fixture._state);
    const auto base = access._snapshot;
    const auto delivery = require_value(runtime::runtime_issue_delivery(access, fixture.consumer_claim(),
        fixture._source, fixture._queue, timestamp(5)));
    EXPECT_EQ(access._snapshot._generation, base._generation + 1);
    require_value(runtime::runtime_release_delivery(access, delivery._token));
    EXPECT_EQ(access._snapshot._generation, base._generation + 2);
    EXPECT_EQ(access._snapshot._tail, base._tail);
    (void)runtime::runtime_topology(access);
  }

  TEST(DeliveryProgressTest, WrongClaimsAndSourceOwnershipRejectWithoutProgress)
  {
    DeliveryProgressFixture fixture;
    fixture.mark_consumer_attached();
    const auto before = fixture.image();
    auto wrongSession = fixture.consumer_claim();
    ++wrongSession._session._low;
    auto futureClaim = fixture.consumer_claim();
    ++futureClaim._incarnation;
    {
      runtime::RuntimeAccess access(fixture._state);
      expect_error(runtime::runtime_issue_delivery(access, wrongSession, fixture._source,
          fixture._queue, timestamp(0)), ErrorCode::WrongOwner);
      expect_error(runtime::runtime_issue_delivery(access, futureClaim, fixture._source,
          fixture._queue, timestamp(0)), ErrorCode::WrongOwner);
      expect_error(runtime::runtime_issue_delivery(access, fixture.consumer_claim(), fixture._otherSource,
          fixture._queue, timestamp(0)), ErrorCode::WrongOwner);
    }
    EXPECT_EQ(fixture.image(), before);
  }

  TEST(DeliveryProgressTest, FreshIssuanceRejectsExhaustionButANewerClaimResetsItsContext)
  {
    DeliveryProgressFixture fixture;
    fixture.mark_consumer_attached();
    fixture.write_word(fixture._sourceState, std::numeric_limits<std::uint64_t>::max());
    fixture.write_word(fixture._sourceState + 8, 1);
    const auto before = fixture.image();
    {
      runtime::RuntimeAccess access(fixture._state);
      expect_error(runtime::runtime_issue_delivery(access, fixture.consumer_claim(),
          fixture._source, fixture._queue, timestamp(0)), ErrorCode::CapacityExceeded);
    }
    EXPECT_EQ(fixture.image(), before);

    // Deliberate private successor diagnostics isolate the Source counter's
    // incarnation fence. Native claim replacement is tested by its own suite.
    fixture.write_word(fixture._consumerState, 2);
    fixture.write_word(fixture._consumerState + 16, 1);
    auto successor = fixture.consumer_claim();
    successor._incarnation = 2;
    {
      runtime::RuntimeAccess access(fixture._state);
      const auto delivery = require_value(runtime::runtime_issue_delivery(access, successor,
          fixture._source, fixture._queue, timestamp(8)));
      EXPECT_EQ(delivery._token._issuance, 1u);
      EXPECT_EQ(delivery._token._consumer, successor);
    }
    fixture.validate();
  }

  TEST(DeliveryProgressTest, LocalWordCodecBridgePreservesAllReceiptWordsAndReservedChecks)
  {
    mapped::Receipt receipt;
    receipt._state = mapped::ReceiptState::Pending;
    auto words = require_value(mapped::encode_receipt_words(receipt));
    EXPECT_EQ(words[0], 1u);
    EXPECT_EQ(require_value(mapped::decode_receipt_words(words)), receipt);
    words[15] = 1;
    expect_error(mapped::decode_receipt_words(words), ErrorCode::IoError);
    expect_error(mapped::decode_receipt_words(std::span{words}.first(7)), ErrorCode::InvalidArgument);
  }

  TEST(DeliveryProgressTest, EveryProgressOperationUsesReservedStateAtTheExactLogCeiling)
  {
    DeliveryProgressFixture fixture(3);
    fixture.mark_consumer_attached();
    fixture.fill_log();
    const auto full = angaria::test_support::journal_snapshot(fixture._directory);
    ASSERT_EQ(full._tail, fixture._configuration._maxLogBytes);
    ASSERT_EQ(full._fileBytes, full._tail);

    const auto first = issue(fixture, -1);
    {
      runtime::RuntimeAccess access(fixture._state);
      require_value(runtime::runtime_release_delivery(access, first._token));
    }
    const auto second = issue(fixture, -2);
    {
      runtime::RuntimeAccess access(fixture._state);
      require_value(runtime::runtime_release_source(access, fixture.consumer_claim(), fixture._source));
    }
    EXPECT_EQ(fixture.receipt()._state, mapped::ReceiptState::Pending);
    EXPECT_EQ(fixture.receipt()._issuance, second._token._issuance);

    // Stale tokens cannot release or acknowledge the freshly reissued pin,
    // including when there is no append space left for an alternative path.
    const auto third = issue(fixture, -3);
    const auto beforeStale = fixture.image();
    {
      runtime::RuntimeAccess access(fixture._state);
      expect_error(runtime::runtime_release_delivery(access, first._token), ErrorCode::StaleDelivery);
      expect_error(runtime::runtime_acknowledge_delivery(access, second._token, timestamp(0)), ErrorCode::StaleDelivery);
    }
    EXPECT_EQ(fixture.image(), beforeStale);
    acknowledge(fixture, third._token, 0);
    for (std::uint64_t sequence = 2; sequence <= 3; ++sequence)
    {
      const auto delivery = issue(fixture, static_cast<std::int64_t>(sequence));
      EXPECT_EQ(delivery._token._sequence, sequence);
      acknowledge(fixture, delivery._token, -static_cast<std::int64_t>(sequence));
    }
    fixture.validate();
    const auto after = angaria::test_support::journal_snapshot(fixture._directory);
    EXPECT_EQ(after._tail, full._tail);
    EXPECT_EQ(after._accounted, full._accounted);
    EXPECT_EQ(after._fileBytes, full._fileBytes);
    EXPECT_EQ(after._manifest, full._manifest);
    EXPECT_EQ(after._revision, full._revision);
    EXPECT_EQ(state_words(fixture, fixture._queueState)[8], 3u);
  }

  TEST(DeliveryProgressTest, ValidLegacySessionRejectsProgressWithoutChangingItsBytes)
  {
    // Only the fixture's temporary parent directory is reused. A pristine
    // Session with no delivery records is relabeled while every owner is closed
    // to qualify the predecessor profile. This is private compatibility setup,
    // never a supported migration or application initialization path.
    DeliveryProgressFixture workspace(0);
    SessionOptions options;
    options._name = "Legacy progress capability boundary";
    options._directory = workspace._root / "legacy";
    options._maxLogBytes = 1024 * 1024;
    options._limits = CapacityLimits::empty().tasks(1);
    TaskDescriptor creator;
    creator._name = "Creator";
    {
      auto created = require_value(Session::create(options, creator));
    }
    {
      auto file = require_value(mapped::NativeFile::open_existing(
          options._directory / "session.ang", mapped::FileAccess::ReadWrite));
      const std::array<std::byte, 2> minor{std::byte{1}};
      const std::array<std::byte, 8> features{std::byte{3}};
      require_value(file.write_exact(10, minor));
      require_value(file.write_exact(56, features));
    }
    auto session = require_value(Session::attach(options._directory));
    auto producer = require_value(session.attach_task(TaskId{1}));
    const auto claim = producer.claim();
    auto state = angaria::test_support::open_runtime_state(options._directory);
    EXPECT_EQ(state->_controller->configuration()._formatMinor, 1u);
    EXPECT_EQ(state->_controller->configuration()._requiredFeatures, 3u);
    runtime::RuntimeAccess access(state);
    auto file = require_value(mapped::NativeFile::open_existing(
        options._directory / "session.ang", mapped::FileAccess::ReadOnly));
    std::vector<std::byte> before(static_cast<std::size_t>(require_value(file.size())));
    require_value(file.read_exact(0, before));
    const DeliveryToken token{claim, SourceQueueSetId{1}, QueueId{1}, 1, 1};
    expect_error(runtime::runtime_issue_delivery(access, claim, token._sourceSet,
        token._queue, timestamp(0)), ErrorCode::UnsupportedPolicy);
    expect_error(runtime::runtime_acknowledge_delivery(access, token, timestamp(0)), ErrorCode::UnsupportedPolicy);
    expect_error(runtime::runtime_release_delivery(access, token), ErrorCode::UnsupportedPolicy);
    expect_error(runtime::runtime_release_source(access, claim, token._sourceSet), ErrorCode::UnsupportedPolicy);
    std::vector<std::byte> after(before.size());
    require_value(file.read_exact(0, after));
    EXPECT_EQ(after, before);
  }
} // namespace
