// Private delivery progress and compact-cleanup qualification. Public Registry
// setup is combined with deliberately installed core Publications; this does
// not demonstrate application setup. Public send/receive and combined owner
// failures are covered by the Source and transport suites. The replacement
// test below exercises a real Task claim after this private setup.

#include "delivery_progress.h"
#include "angaria/detail/byte_operations.h"
#include "delivery_progress_fixture.h"
#include "journal_fixture.h"
#include "result_assertions.h"

#include <gtest/gtest.h>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <span>
#include <utility>
#include <vector>

namespace
{
  using namespace angaria;
  namespace runtime = angaria::detail;
  namespace mapped = angaria::detail::mapped;
  namespace support = angaria::test_support;

  runtime::DeliveryProgress issue(support::DeliveryProgressFixture& fixture)
  {
    runtime::RuntimeAccess access(fixture._state);
    return support::require_value(runtime::runtime_issue_delivery(
        access, fixture.consumer_claim(), fixture._source, fixture._queue,
        UtcTimestamp{std::chrono::nanoseconds{-7}}));
  }

  mapped::TransactionPlan release_plan(runtime::RuntimeAccess& access,
                                         std::uint64_t state, std::uint64_t incarnation = 1)
  {
    mapped::TransactionPlan plan;
    plan._base = access._snapshot;
    plan._newRevision = access._snapshot._revision;
    auto command = support::write_word(state, incarnation);
    command._opcode = mapped::RedoOpcode::ReleaseClaim;
    command._payload.resize(16);
    support::put_integer(command._payload, 8, 0); // Epoch zero is a present detach time.
    plan._commands.push_back(std::move(command));
    return plan;
  }

  TEST(DeliveryClaimCleanupTest, FullLogReleasePreservesHistoryCountersAndQueueProgress)
  {
    support::DeliveryProgressFixture fixture;
    fixture.mark_consumer_attached();
    const auto delivery = issue(fixture);
    const auto issued = fixture.receipt();
    fixture.fill_log();
    mapped::JournalSnapshot base;
    std::vector<std::uint64_t> queue;
    {
      runtime::RuntimeAccess access(fixture._state);
      base = access._snapshot;
      queue = access.read_words(fixture._queueState, 32);
      support::require_value(access._journal.execute(access._guard, release_plan(access, fixture._consumerState)));
    }
    auto pending = issued;
    pending._state = mapped::ReceiptState::Pending;
    EXPECT_EQ(fixture.receipt(), pending);
    {
      runtime::RuntimeAccess access(fixture._state);
      const auto source = access.read_words(fixture._sourceState, 8);
      EXPECT_EQ(source[0], delivery._token._issuance);
      EXPECT_EQ(source[1], delivery._token._consumer._incarnation);
      EXPECT_TRUE(runtime::all_zero(std::span{source}.subspan(2)));
      EXPECT_EQ(access.read_words(fixture._queueState, 32), queue);
      EXPECT_EQ(access._snapshot._tail, base._tail);
      EXPECT_EQ(access._snapshot._accounted, base._accounted);
      EXPECT_EQ(access._snapshot._fileBytes, fixture._configuration._maxLogBytes);
      const auto task = access.read_words(fixture._consumerState, 10);
      EXPECT_EQ(task[6], 6u); // Attachment and epoch-zero detachment are both present.
      EXPECT_EQ(task[9], 0u);
      // Repeating the same captured cleanup preserves canonical progress even
      // after its receipt is already Pending and the Source slot is cleared.
      support::require_value(access._journal.execute(access._guard, release_plan(access, fixture._consumerState)));
    }
    EXPECT_EQ(fixture.receipt(), pending);
    fixture.validate();
  }

  TEST(DeliveryClaimCleanupTest, TerminalReceiptsAndAcknowledgmentMetadataRemainTerminal)
  {
    support::DeliveryProgressFixture fixture;
    fixture.mark_consumer_attached();
    const auto first = issue(fixture);
    {
      runtime::RuntimeAccess access(fixture._state);
      support::require_value(runtime::runtime_acknowledge_delivery(
          access, first._token, UtcTimestamp{std::chrono::nanoseconds{0}}));
    }
    const auto acknowledged = fixture.receipt();
    const auto second = issue(fixture);
    {
      runtime::RuntimeAccess access(fixture._state);
      const auto queue = access.read_words(fixture._queueState + 64, 4);
      support::require_value(access._journal.execute(access._guard, release_plan(access, fixture._consumerState)));
      EXPECT_EQ(access.read_words(fixture._queueState + 64, 4), queue);
    }
    EXPECT_EQ(fixture.receipt(), acknowledged);
    EXPECT_EQ(fixture.receipt(1)._state, mapped::ReceiptState::Pending);
    EXPECT_EQ(fixture.receipt(1)._issuance, second._token._issuance);
    fixture.validate();
  }

  TEST(DeliveryClaimCleanupTest, ParentReleaseCleansCapturedThreadPinsAndPreservesParentTag)
  {
    support::DeliveryProgressFixture fixture(2, TaskKind::Thread);
    fixture.mark_consumer_attached();
    issue(fixture);
    {
      runtime::RuntimeAccess access(fixture._state);
      support::require_value(access._journal.execute(access._guard, release_plan(access, fixture._parentState)));
      const auto child = access.read_words(fixture._consumerState, 10);
      EXPECT_EQ(child[6] & 1, 0u);
      EXPECT_EQ(child[6] & 4, 4u);
      EXPECT_EQ(child[7], 1u);
      EXPECT_EQ(access.read_words(fixture._sourceState, 2), (std::vector<std::uint64_t>{1, 1}));
    }
    EXPECT_EQ(fixture.receipt()._state, mapped::ReceiptState::Pending);
    fixture.validate();
  }

  TEST(DeliveryClaimCleanupTest, PublicReplacementReleasesTheOldIssuanceAtLogFull)
  {
    support::DeliveryProgressFixture fixture;
    fixture.mark_consumer_attached();
    const auto previous = issue(fixture);
    fixture.fill_log();
    auto session = support::require_value(Session::attach(fixture._directory));
    auto replacement = support::require_value(session.attach_task(fixture._consumer));
    EXPECT_EQ(replacement.claim()._incarnation, 2u);
    EXPECT_EQ(fixture.receipt()._state, mapped::ReceiptState::Pending);
    EXPECT_EQ(fixture.receipt()._consumerIncarnation, previous._token._consumer._incarnation);
    {
      runtime::RuntimeAccess access(fixture._state);
      EXPECT_EQ(access._snapshot._fileBytes, fixture._configuration._maxLogBytes);
      EXPECT_EQ(access.read_words(fixture._sourceState, 2), (std::vector<std::uint64_t>{1, 1}));
    }
    fixture.validate();
  }

  TEST(DeliveryClaimCleanupTest, MixedReceiptWritesCannotEraseReplayDiscovery)
  {
    support::DeliveryProgressFixture fixture;
    fixture.mark_consumer_attached();
    issue(fixture);
    const auto before = fixture.image();
    {
      runtime::RuntimeAccess access(fixture._state);
      auto plan = release_plan(access, fixture._consumerState);
      plan._commands.push_back(support::write_word(fixture._publications.front()._receipt + 8, 0));
      support::expect_error(access._journal.execute(access._guard, plan), ErrorCode::InvalidArgument);
    }
    EXPECT_EQ(fixture.image(), before);
  }

  TEST(DeliveryClaimCleanupTest, OldCleanupCannotClearANewerSourceContext)
  {
    support::DeliveryProgressFixture fixture;
    fixture.mark_consumer_attached();
    issue(fixture);
    // Model an invalid stale compact intent at the immediate successor boundary.
    // Recovery must reject it before touching the newer Source counter or pin.
    fixture.write_word(fixture._consumerState, 2);
    fixture.write_word(fixture._sourceState + 8, 2);
    const auto before = fixture.image();
    {
      runtime::RuntimeAccess access(fixture._state);
      support::expect_error(access._journal.execute(access._guard,
          release_plan(access, fixture._consumerState)), ErrorCode::InvalidArgument);
    }
    EXPECT_EQ(fixture.image(), before);
  }

  TEST(DeliveryClaimCleanupTest, ParentCleanupCannotReplaceChildIdentityWithinItsReplayStream)
  {
    support::DeliveryProgressFixture fixture(2, TaskKind::Thread);
    fixture.mark_consumer_attached();
    issue(fixture);
    const auto before = fixture.image();
    {
      runtime::RuntimeAccess access(fixture._state);
      auto plan = release_plan(access, fixture._parentState);
      // The compact parent intent captures the parent incarnation. Rewriting a
      // child's incarnation would erase the old receipt identity on replay.
      plan._commands.push_back(support::write_word(fixture._consumerState, 2));
      support::expect_error(access._journal.execute(access._guard, plan), ErrorCode::InvalidArgument);
    }
    EXPECT_EQ(fixture.image(), before);
  }
} // namespace
