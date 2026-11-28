// Private fixed-topology qualification. Each peer executes the production
// endpoint in a separate process. A readiness pipe identifies the exact stop
// boundary; SIGKILL then releases ownership without running endpoint cleanup.
#include "recorded_queue_fixture.h"
#include "native_process.h"
#include "result_assertions.h"

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <variant>

#ifndef ANGARIA_RECORDED_QUEUE_WORKER
#error ANGARIA_RECORDED_QUEUE_WORKER must name the independent recorded Queue worker
#endif

namespace
{
  using namespace angaria;
  namespace recorded = angaria::detail::recorded;
  namespace mapped = angaria::detail::mapped;
  namespace support = angaria::test_support;
  using support::expect_error;
  using support::require_value;
  using support::require_recorded_send;

  struct ProducerStop
  {
    const char* _mode;
    const char* _name;
    bool _committed;
  };

  class RecordedQueueProducerDeath : public testing::TestWithParam<ProducerStop>
  {
  };
}

TEST_P(RecordedQueueProducerDeath, OnlyCommittedMessagesSurviveAndReplacementContinuesTheQueue)
{
  const auto stop = GetParam();
  support::ProducerLogDirectory directory;
  const auto options = support::recorded_queue_options(directory.path());
  const auto codec = support::recorded_queue_codec(options);
  require_value(recorded::create_recorded_queue(options));
  auto receiver = require_value(recorded::RecordedConsumer::open(options));
  support::NativeProcess worker({ANGARIA_RECORDED_QUEUE_WORKER, directory.path().string(), stop._mode});
  worker.expect('O');
  const TaskClaim abandoned{options._log._header._session, options._log._header._producer,
                            worker.receive<std::uint64_t>()};
  const PublicationId interrupted{abandoned, 1};
  worker.expect('R');

  // A paused writer still owns its actual log inode. Catch-up is a reader
  // operation; neither receiving nor inspecting its log takes ownership away.
  expect_error(recorded::RecordedProducer::open(options), ErrorCode::OwnershipBusy);
  auto replay = require_value(recorded::ProducerLogReader::open(options._producerPaths._log, options._log));
  auto frozen = require_value(replay.capture());
  ASSERT_EQ(frozen.record_count(), stop._committed ? 1u : 0u);
  const auto recordedFirst = require_value(replay.next(frozen));
  ASSERT_EQ(bool(recordedFirst), stop._committed);
  const auto liveFirst = require_value(receiver.try_receive());
  ASSERT_EQ(bool(liveFirst), stop._committed);
  std::optional<Message> copied;
  MessageHeader observedHeader;
  if (liveFirst)
  {
    EXPECT_EQ(liveFirst->_header._publication, interrupted);
    EXPECT_EQ(liveFirst->_header._queueSequence, 1u);
    EXPECT_EQ(liveFirst->_header._publishedAt, recordedFirst->_header._publishedAt);
    observedHeader = liveFirst->_header;
    copied = require_value(liveFirst->copy_message(8));
  }
  EXPECT_EQ(support::recorded_queue_credit(options), stop._committed ? 1u : 0u);
  EXPECT_FALSE(require_value(receiver.try_receive()));
  worker.terminate();

  {
    auto replacement = require_value(recorded::RecordedProducer::open(options));
    EXPECT_GT(require_value(replacement.claim())._incarnation, abandoned._incarnation);
    const auto outcome = require_value(replacement.lookup(interrupted));
    ASSERT_EQ(bool(outcome), stop._committed);
    const auto firstBytes = support::recorded_queue_payload(codec, 42);
    const auto repeated = replacement.try_send(interrupted, support::recorded_queue_message(options, firstBytes));
    if (stop._committed)
    {
      const auto accepted = require_recorded_send(repeated);
      EXPECT_TRUE(accepted._duplicate);
      EXPECT_EQ(accepted._reference, recordedFirst->_reference);
    }
    else
    {
      const auto* error = std::get_if<Error>(&repeated);
      ASSERT_NE(error, nullptr);
      EXPECT_EQ(error->_code, ErrorCode::InvalidArgument);
    }

    // An uncommitted suffix may be overwritten; an accepted message keeps its
    // original identity and sequence. A new claim continues after that prefix.
    const auto nextId = require_value(replacement.next_publication_id());
    const auto nextBytes = support::recorded_queue_payload(codec, 84);
    const auto next = require_recorded_send(replacement.try_send(
        nextId, support::recorded_queue_message(options, nextBytes)));
    EXPECT_EQ(next._queueSequence, stop._committed ? 2u : 1u);
    const auto received = require_value(receiver.try_receive());
    ASSERT_TRUE(received);
    EXPECT_EQ(received->_header._publication, nextId);
    EXPECT_EQ(received->_header._queueSequence, next._queueSequence);
    EXPECT_EQ(std::get<std::uint64_t>(require_value(codec.decode(received->_payload)).at(0)), 84u);
    EXPECT_FALSE(require_value(receiver.try_receive()));
  }
  receiver = recorded::RecordedConsumer{};

  // A previously captured cut stays exhausted after another claim appends.
  // Explicitly capture again to replay all accepted messages after peer exit.
  EXPECT_FALSE(require_value(replay.next(frozen)));
  auto complete = require_value(replay.capture());
  EXPECT_EQ(complete.record_count(), stop._committed ? 2u : 1u);
  for (std::uint64_t sequence = 1; sequence <= complete.record_count(); ++sequence)
  {
    const auto record = require_value(replay.next(complete));
    ASSERT_TRUE(record);
    EXPECT_EQ(require_value(record->destination_at(0))._queueSequence, sequence);
    const auto value = std::get<std::uint64_t>(require_value(codec.decode(record->_payload)).at(0));
    EXPECT_EQ(value, stop._committed && sequence == 1 ? 42u : 84u);
  }
  EXPECT_FALSE(require_value(replay.next(complete)));
  replay = recorded::ProducerLogReader{};
  if (copied)
  {
    // No producer, consumer or replay mapping remains. The owning Message
    // still has its complete observed delivery header and exact schema.
    EXPECT_EQ(copied->_header, observedHeader);
    EXPECT_EQ(copied->view()._header, observedHeader);
    EXPECT_EQ(std::get<std::uint64_t>(require_value(copied->decode(codec)).at(0)), 42u);
    EXPECT_EQ(require_value(copied->view().copy_message(8))._header, observedHeader);
  }
}

INSTANTIATE_TEST_SUITE_P(RealProcessStops, RecordedQueueProducerDeath,
    testing::Values(
        ProducerStop{"partial", "PartialMaterialization", false},
        ProducerStop{"before-commit", "BeforeCommit", false},
        ProducerStop{"after-log-commit", "AfterLogCommit", true},
        ProducerStop{"commit", "BeforeDescriptor", true},
        ProducerStop{"stamp", "BeforeStamp", true},
        ProducerStop{"published", "AfterStamp", true}),
    [](const testing::TestParamInfo<ProducerStop>& value)
    {
      return std::string{value.param._name};
    });

namespace
{
  struct ConsumerStop
  {
    const char* _mode;
    const char* _name;
    std::uint64_t _credit;
  };

  class RecordedQueueConsumerDeath : public testing::TestWithParam<ConsumerStop>
  {
  };
}

TEST_P(RecordedQueueConsumerDeath, ReplacementReplaysHistoryWithoutReturningCapacityTwice)
{
  const auto stop = GetParam();
  support::ProducerLogDirectory directory;
  const auto options = support::recorded_queue_options(directory.path());
  const auto codec = support::recorded_queue_codec(options);
  const auto bytes = support::recorded_queue_payload(codec, 42);
  const auto outgoing = support::recorded_queue_message(options, bytes);
  require_value(recorded::create_recorded_queue(options));
  auto sender = require_value(recorded::RecordedProducer::open(options));
  const auto firstId = require_value(sender.next_publication_id());
  require_recorded_send(sender.try_send(firstId, outgoing));
  require_recorded_send(sender.try_send(require_value(sender.next_publication_id()), outgoing));
  const auto waitingId = require_value(sender.next_publication_id());
  ASSERT_TRUE(std::holds_alternative<recorded::QueueCreditFull>(sender.try_send(waitingId, outgoing)));

  support::NativeProcess worker({ANGARIA_RECORDED_QUEUE_WORKER, directory.path().string(), stop._mode});
  worker.expect('O');
  const auto originalIncarnation = worker.receive<std::uint64_t>();
  worker.proceed();
  worker.expect('R');
  EXPECT_EQ(support::recorded_queue_credit(options), stop._credit);
  expect_error(recorded::RecordedConsumer::open(options), ErrorCode::OwnershipBusy);
  worker.terminate();

  // The process may have returned credit before its receive call returned to
  // the application. No processing acknowledgment exists in this mode: a
  // replacement intentionally replays from sequence 1 in both outcomes.
  if (stop._credit == 1)
  {
    EXPECT_EQ(require_recorded_send(sender.try_send(waitingId, outgoing))._queueSequence, 3u);
    // Sequence 3 reuses sequence 1's descriptor. Replacement must obtain the
    // credited historical message from the immutable log, bypassing that slot.
  }
  auto replacement = require_value(recorded::RecordedConsumer::open(options));
  EXPECT_GT(require_value(replacement.claim())._incarnation, originalIncarnation);
  const auto first = require_value(replacement.try_receive());
  ASSERT_TRUE(first);
  EXPECT_EQ(first->_header._publication, firstId);
  EXPECT_EQ(first->_header._queueSequence, 1u);
  EXPECT_EQ(support::recorded_queue_credit(options), 1u);
  if (stop._credit == 0)
  {
    EXPECT_EQ(require_recorded_send(sender.try_send(waitingId, outgoing))._queueSequence, 3u);
  }
  const auto fourthId = require_value(sender.next_publication_id());
  EXPECT_TRUE(std::holds_alternative<recorded::QueueCreditFull>(sender.try_send(fourthId, outgoing)));
  for (std::uint64_t sequence = 2; sequence <= 3; ++sequence)
  {
    const auto message = require_value(replacement.try_receive());
    ASSERT_TRUE(message);
    EXPECT_EQ(message->_header._queueSequence, sequence);
    EXPECT_EQ(support::recorded_queue_credit(options), sequence);
  }
  EXPECT_FALSE(require_value(replacement.try_receive()));
  replacement = recorded::RecordedConsumer{};

  // All three old messages are now credited. Replaying them a second time
  // must not create additional capacity for the still-live producer.
  auto again = require_value(recorded::RecordedConsumer::open(options));
  EXPECT_EQ(require_recorded_send(sender.try_send(fourthId, outgoing))._queueSequence, 4u);
  require_recorded_send(sender.try_send(require_value(sender.next_publication_id()), outgoing));
  const auto blocked = require_value(sender.next_publication_id());
  for (std::uint64_t sequence = 1; sequence <= 3; ++sequence)
  {
    const auto message = require_value(again.try_receive());
    ASSERT_TRUE(message);
    EXPECT_EQ(message->_header._queueSequence, sequence);
    EXPECT_EQ(support::recorded_queue_credit(options), 3u);
    EXPECT_TRUE(std::holds_alternative<recorded::QueueCreditFull>(sender.try_send(blocked, outgoing)));
  }
  for (std::uint64_t sequence = 4; sequence <= 5; ++sequence)
  {
    const auto message = require_value(again.try_receive());
    ASSERT_TRUE(message);
    EXPECT_EQ(message->_header._queueSequence, sequence);
    EXPECT_EQ(support::recorded_queue_credit(options), sequence);
  }
  EXPECT_FALSE(require_value(again.try_receive()));

  // Keep the tiny ring running through many wraps, then kill a third consumer
  // while it is replaying an already credited historical message. Its private
  // cursor must not become transport credit or hide the retained prefix.
  for (std::uint64_t sequence = 6; sequence <= 124; ++sequence)
  {
    const auto accepted = require_recorded_send(sender.try_send(require_value(sender.next_publication_id()), outgoing));
    EXPECT_EQ(accepted._queueSequence, sequence);
    const auto message = require_value(again.try_receive());
    ASSERT_TRUE(message);
    EXPECT_EQ(message->_header._queueSequence, sequence);
  }
  again = recorded::RecordedConsumer{};
  support::NativeProcess replayWorker({ANGARIA_RECORDED_QUEUE_WORKER, directory.path().string(), stop._mode});
  replayWorker.expect('O');
  const auto replayIncarnation = replayWorker.receive<std::uint64_t>();
  replayWorker.proceed();
  replayWorker.expect('R');
  EXPECT_EQ(support::recorded_queue_credit(options), 124u);
  replayWorker.terminate();

  require_recorded_send(sender.try_send(require_value(sender.next_publication_id()), outgoing));
  require_recorded_send(sender.try_send(require_value(sender.next_publication_id()), outgoing));
  const auto stillWaiting = require_value(sender.next_publication_id());
  auto afterReplayDeath = require_value(recorded::RecordedConsumer::open(options));
  EXPECT_GT(require_value(afterReplayDeath.claim())._incarnation, replayIncarnation);
  for (std::uint64_t sequence = 1; sequence <= 124; ++sequence)
  {
    const auto message = require_value(afterReplayDeath.try_receive());
    ASSERT_TRUE(message);
    EXPECT_EQ(message->_header._queueSequence, sequence);
    EXPECT_EQ(support::recorded_queue_credit(options), 124u);
    EXPECT_TRUE(std::holds_alternative<recorded::QueueCreditFull>(sender.try_send(stillWaiting, outgoing)));
  }
  const auto uncredited = require_value(afterReplayDeath.try_receive());
  ASSERT_TRUE(uncredited);
  EXPECT_EQ(uncredited->_header._queueSequence, 125u);
  EXPECT_EQ(support::recorded_queue_credit(options), 125u);
  EXPECT_EQ(require_recorded_send(sender.try_send(stillWaiting, outgoing))._queueSequence, 127u);
  for (std::uint64_t sequence = 126; sequence <= 127; ++sequence)
  {
    const auto message = require_value(afterReplayDeath.try_receive());
    ASSERT_TRUE(message);
    EXPECT_EQ(message->_header._queueSequence, sequence);
    EXPECT_EQ(support::recorded_queue_credit(options), sequence);
  }
  EXPECT_FALSE(require_value(afterReplayDeath.try_receive()));
}

INSTANTIATE_TEST_SUITE_P(RealProcessStops, RecordedQueueConsumerDeath,
    testing::Values(
        ConsumerStop{"consumer-before-credit", "BeforeCredit", 0},
        ConsumerStop{"consumer-after-credit", "AfterCredit", 1}),
    [](const testing::TestParamInfo<ConsumerStop>& value)
    {
      return std::string{value.param._name};
    });

TEST(RecordedQueueRestart, StaleStampedDescriptorFailsClosedWithoutReturningCredit)
{
  support::ProducerLogDirectory directory;
  const auto options = support::recorded_queue_options(directory.path());
  const auto codec = support::recorded_queue_codec(options);
  const auto bytes = support::recorded_queue_payload(codec, 42);
  require_value(recorded::create_recorded_queue(options));
  auto sender = require_value(recorded::RecordedProducer::open(options));
  auto receiver = require_value(recorded::RecordedConsumer::open(options));
  const auto id = require_value(sender.next_publication_id());
  require_recorded_send(sender.try_send(id, support::recorded_queue_message(options, bytes)));
  auto file = require_value(mapped::NativeFile::open_existing(options._connectionPath, mapped::FileAccess::ReadWrite));
  auto mapping = require_value(file.map_read_write(4096, 64));
  auto slot = require_value(mapping.writable_bytes());
  auto generation = require_value(mapped::AtomicWord::bind(slot, 24));
  generation.store_release(options._generation + 1);

  // Deliberate protocol corruption: a matching Queue-sequence stamp does not
  // make a descriptor from another connection generation usable. The accepted
  // log record is intact, and repairing bytes cannot revive the failed handle.
  expect_error(receiver.try_receive(), ErrorCode::IoError);
  EXPECT_EQ(support::recorded_queue_credit(options), 0u);
  generation.store_release(options._generation);
  expect_error(receiver.try_receive(), ErrorCode::IoError);
  EXPECT_EQ(support::recorded_queue_credit(options), 0u);
  receiver = recorded::RecordedConsumer{};
  auto replacement = require_value(recorded::RecordedConsumer::open(options));
  const auto recovered = require_value(replacement.try_receive());
  ASSERT_TRUE(recovered);
  EXPECT_EQ(recovered->_header._publication, id);
  EXPECT_EQ(support::recorded_queue_credit(options), 1u);
}
