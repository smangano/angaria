// Private endpoint qualification against an actual public Session controller.
// New recorded resources share that Session's identity/directory, but this is
// not public profile admission or integrated Task lifetime ownership.
#include "angaria/runtime.h"
#include "recorded_queue_independence_fixture.h"
#include "native_file.h"
#include "native_process.h"
#include "result_assertions.h"
#include "session_controller.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#ifndef ANGARIA_RECORDED_QUEUE_INDEPENDENCE_WORKER
#error ANGARIA_RECORDED_QUEUE_INDEPENDENCE_WORKER must name the independent qualification peer
#endif

namespace
{
  using namespace angaria;
  namespace mapped = angaria::detail::mapped;
  namespace recorded = angaria::detail::recorded;
  namespace support = angaria::test_support;
  using support::require_value;
  using support::expect_error;

  class RecordedQueueIndependenceTest : public testing::Test
  {
  protected:
    void SetUp() override
    {
      SessionOptions options;
      options._name = "ConnectionIndependence";
      options._directory = _directory.path() / "session";
      options._maxLogBytes = 1048576;
      options._limits = CapacityLimits::empty().tasks(8);
      TaskDescriptor creator;
      creator._name = "Administrator";
      auto created = require_value(Session::create(options, creator));
      _session = created._session.identity();
      _path = options._directory;
      _controller = require_value(mapped::SessionController::open_existing(_path));
      // The public creator may close: neither private recorded peer needs its
      // claim or a Session gate once the offline resources have been established.
    }

    void create(unsigned connection, std::uint64_t publications = 128)
    {
      require_value(recorded::create_recorded_queue(support::recorded_independence_options(
          _path, _session, connection, publications)));
    }

    std::vector<std::string> arguments(const char* mode, unsigned connection = 1,
                                       std::uint64_t publications = 128) const
    {
      return {ANGARIA_RECORDED_QUEUE_INDEPENDENCE_WORKER, _path.string(), mode,
              std::to_string(connection), std::to_string(publications)};
    }

    static void expect_established(support::NativeProcess& peer)
    {
      peer.expect('O');
      EXPECT_NE(peer.receive<std::uint64_t>(), 0u);
    }

    static void expect_completed(support::NativeProcess& peer)
    {
      peer.expect('A');
      EXPECT_EQ(peer.receive<std::uint64_t>(), support::independence_message_count);
      EXPECT_EQ(peer.receive<std::uint64_t>(),
                support::independence_message_count * (support::independence_message_count + 1) / 2);
      peer.wait_success();
    }

    std::vector<std::byte> session_bytes() const
    {
      auto file = require_value(mapped::NativeFile::open_existing(_path / "session.ang", mapped::FileAccess::ReadOnly));
      std::vector<std::byte> bytes(static_cast<std::size_t>(require_value(file.size())));
      require_value(file.read_exact(0, bytes));
      return bytes;
    }

    support::ProducerLogDirectory _directory;
    std::filesystem::path _path;
    SessionId _session{};
    std::shared_ptr<mapped::SessionController> _controller;
  };
}

TEST_F(RecordedQueueIndependenceTest, EstablishedExecPeersFinishWhileTheActualSessionGateRemainsHeld)
{
  create(1);
  support::NativeProcess producer{arguments("produce")};
  support::NativeProcess consumer{arguments("consume")};
  expect_established(producer);
  expect_established(consumer);
  const auto sessionBefore = session_bytes();
  support::NativeProcess administrator{arguments("gate")};
  administrator.expect('G');
  // Positive control: an ordinary administrative acquisition sees the exact
  // gate held by the independent child, before AND after message completion.
  expect_error(_controller->try_acquire_mutation(), ErrorCode::OwnershipBusy);
  producer.proceed();
  consumer.proceed();
  expect_completed(producer);
  expect_completed(consumer);
  expect_error(_controller->try_acquire_mutation(), ErrorCode::OwnershipBusy);
  EXPECT_EQ(session_bytes(), sessionBefore);
  administrator.proceed();
  administrator.wait_success();
  auto guard = require_value(_controller->try_acquire_mutation());
  require_value(guard.check_owner());
}

TEST_F(RecordedQueueIndependenceTest, ACommittedPausedProducerAndItsBusyReplacementDoNotGateAnotherConnection)
{
  create(0);
  create(1);
  support::NativeProcess producer{arguments("produce")};
  support::NativeProcess consumer{arguments("consume")};
  expect_established(producer);
  expect_established(consumer);
  support::NativeProcess paused{arguments("paused", 0)};
  expect_established(paused);
  paused.expect('R');
  // This owner has committed a real message and still holds its actual log
  // claim before descriptor exposure. A replacement must fail without waiting.
  support::NativeProcess replacement{arguments("probe-producer", 0)};
  replacement.expect('B');
  replacement.wait_success();
  producer.proceed();
  consumer.proceed();
  expect_completed(producer);
  expect_completed(consumer);
  paused.proceed();
  paused.expect('A');
  paused.wait_success();
}

TEST_F(RecordedQueueIndependenceTest, AnOfflineFullConnectionDoesNotPreventAnotherConnectionFromWrappingItsRing)
{
  create(0);
  create(1);
  support::NativeProcess producer{arguments("produce")};
  support::NativeProcess consumer{arguments("consume")};
  expect_established(producer);
  expect_established(consumer);
  support::NativeProcess full{arguments("full", 0)};
  expect_established(full);
  full.expect('F');
  // No consumer ever opens the first Queue. The second connection must issue,
  // send, resolve and consume many more messages than either ring can hold.
  producer.proceed();
  consumer.proceed();
  expect_completed(producer);
  expect_completed(consumer);
  full.proceed();
  full.wait_success();
}

TEST_F(RecordedQueueIndependenceTest, ExhaustedRecordingCapacityDoesNotBecomeASessionWideStop)
{
  create(0, 2);
  create(1);
  support::NativeProcess producer{arguments("produce")};
  support::NativeProcess consumer{arguments("consume")};
  expect_established(producer);
  expect_established(consumer);
  support::NativeProcess full{arguments("logfull", 0, 2)};
  expect_established(full);
  full.expect('L');
  // The exhausted connection returns all ring credit first, proving that this
  // is producer-log exhaustion rather than QueueCreditFull backpressure.
  producer.proceed();
  consumer.proceed();
  expect_completed(producer);
  expect_completed(consumer);
  full.proceed();
  full.wait_success();
}

TEST_F(RecordedQueueIndependenceTest, KillingPausedRecoveryLeavesOtherTrafficAndLaterReplacementIndependent)
{
  create(0);
  create(1);
  const auto options = support::recorded_independence_options(_path, _session, 0);
  const auto codec = support::recorded_queue_codec(options);
  const auto bytes = support::recorded_queue_payload(codec, 42);
  const auto outgoing = support::recorded_queue_message(options, bytes);
  PublicationId acceptedId;
  recorded::ProducerAppendResult original;
  {
    auto producer = require_value(recorded::RecordedProducer::open(options));
    acceptedId = require_value(producer.next_publication_id());
    original = support::require_recorded_send(producer.try_send(acceptedId, outgoing));
  }
  support::NativeProcess producer{arguments("produce")};
  support::NativeProcess consumer{arguments("consume")};
  expect_established(producer);
  expect_established(consumer);
  support::NativeProcess administrator{arguments("gate")};
  administrator.expect('G');
  support::NativeProcess recovery{arguments("recover", 0)};
  recovery.expect('R');
  // Recovery owns the affected log before prefix validation. Neither that
  // native claim nor the held administrative gate may stop another connection.
  support::NativeProcess competingRecovery{arguments("probe-producer", 0)};
  competingRecovery.expect('B');
  competingRecovery.wait_success();
  expect_error(_controller->try_acquire_mutation(), ErrorCode::OwnershipBusy);
  producer.proceed();
  consumer.proceed();
  expect_completed(producer);
  expect_completed(consumer);
  expect_error(_controller->try_acquire_mutation(), ErrorCode::OwnershipBusy);

  // A real SIGKILL releases only the affected claim. The next replacement
  // rebuilds acceptance and continues its Queue while the Session gate is
  // still held; recovery did not create a shared recovery gate or counter.
  recovery.terminate();
  auto replacement = require_value(recorded::RecordedProducer::open(options));
  EXPECT_GT(require_value(replacement.claim())._incarnation, acceptedId._producer._incarnation);
  const auto known = require_value(replacement.lookup(acceptedId));
  ASSERT_TRUE(known);
  EXPECT_EQ(known->_reference, original._reference);
  EXPECT_EQ(known->_queueSequence, 1u);
  auto receiver = require_value(recorded::RecordedConsumer::open(options));
  const auto replayed = require_value(receiver.try_receive());
  ASSERT_TRUE(replayed);
  EXPECT_EQ(replayed->_header._publication, acceptedId);
  EXPECT_EQ(std::get<std::uint64_t>(require_value(codec.decode(replayed->_payload)).at(0)), 42u);
  const auto next = support::require_recorded_send(replacement.try_send(
      require_value(replacement.next_publication_id()), outgoing));
  EXPECT_EQ(next._queueSequence, 2u);
  ASSERT_TRUE(require_value(receiver.try_receive()));
  EXPECT_FALSE(require_value(receiver.try_receive()));
  expect_error(_controller->try_acquire_mutation(), ErrorCode::OwnershipBusy);
  administrator.proceed();
  administrator.wait_success();
}
