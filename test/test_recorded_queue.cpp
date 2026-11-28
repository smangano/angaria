// These private endpoints exercise the new fixed-topology transport. Their
// catalog has real schema definitions, but no public Session is constructed.
#include "recorded_queue_fixture.h"
#include "native_file.h"
#include "native_process.h"
#include "result_assertions.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <thread>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

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

  constexpr std::uint64_t credit_offset = 256;

  std::vector<std::byte> file_bytes(const std::filesystem::path& path)
  {
    auto file = require_value(mapped::NativeFile::open_existing(path, mapped::FileAccess::ReadOnly));
    std::vector<std::byte> bytes(static_cast<std::size_t>(require_value(file.size())));
    require_value(file.read_exact(0, bytes));
    return bytes;
  }

  std::uint64_t control_word(const std::filesystem::path& path, std::uint64_t offset)
  {
    auto file = require_value(mapped::NativeFile::open_existing(path, mapped::FileAccess::ReadOnly));
    auto region = require_value(file.map_read_only(offset, 8));
    return require_value(mapped::AtomicReadWord::bind(require_value(region.bytes()), 0)).load_acquire();
  }

  void expect_send_error(const recorded::RecordedSendResult& result, ErrorCode code)
  {
    const auto* error = std::get_if<Error>(&result);
    ASSERT_NE(error, nullptr);
    EXPECT_EQ(error->_code, code);
  }

  class ErrorAtBoundary final : public recorded::RecordedQueueObserver
  {
  public:
    explicit ErrorAtBoundary(recorded::ProducerLogCheckpoint point) : _point(point)
    {
    }

    explicit ErrorAtBoundary(recorded::RecordedQueueCheckpoint point) : _point(point)
    {
    }

    OperationResult checkpoint(recorded::ProducerLogCheckpoint point) override
    {
      return interrupt(point);
    }

    OperationResult checkpoint(recorded::RecordedQueueCheckpoint point) override
    {
      return interrupt(point);
    }

    bool fired() const noexcept
    {
      return _fired;
    }

  private:
    template <typename Point>
    OperationResult interrupt(Point point)
    {
      const auto* selected = std::get_if<Point>(&_point);
      if (!_fired && selected && *selected == point)
      {
        _fired = true;
        return Error{ErrorCode::IoError, "Injected recorded Queue interruption", "recorded_queue.test"};
      }
      return std::monostate{};
    }

    std::variant<recorded::ProducerLogCheckpoint, recorded::RecordedQueueCheckpoint> _point;
    bool _fired{};
  };

  class RecordedQueueTest : public testing::Test
  {
  protected:
    void create()
    {
      require_value(recorded::create_recorded_queue(_options));
    }

    recorded::RecordedProducer producer()
    {
      return require_value(recorded::RecordedProducer::open(_options));
    }

    recorded::RecordedConsumer consumer()
    {
      return require_value(recorded::RecordedConsumer::open(_options));
    }

    MessageView outgoing(std::span<const std::byte> bytes)
    {
      return support::recorded_queue_message(_options, bytes);
    }

    std::uint64_t received_value(const MessageView& message)
    {
      return std::get<std::uint64_t>(require_value(_codec.decode(message._payload)).at(0));
    }

    support::ProducerLogDirectory _directory;
    recorded::RecordedQueueOptions _options{support::recorded_queue_options(_directory.path())};
    BinaryPayloadCodec _codec{support::recorded_queue_codec(_options)};
  };

  static_assert(!std::is_copy_constructible_v<recorded::RecordedProducer>);
  static_assert(!std::is_copy_constructible_v<recorded::RecordedConsumer>);
  static_assert(std::is_nothrow_move_constructible_v<recorded::RecordedProducer>);
  static_assert(std::is_nothrow_move_constructible_v<recorded::RecordedConsumer>);
}

TEST_F(RecordedQueueTest, OrderedRoundTripsKeepQueueSequenceAcrossSmallRingWraps)
{
  create();
  auto sender = producer();
  auto receiver = consumer();
  EXPECT_FALSE(require_value(receiver.try_receive()));
  for (std::uint64_t value = 1; value <= 12; ++value)
  {
    const auto bytes = support::recorded_queue_payload(_codec, value);
    const auto id = require_value(sender.next_publication_id());
    const auto accepted = require_recorded_send(sender.try_send(id, outgoing(bytes)));
    EXPECT_EQ(accepted._queueSequence, value);
    const auto message = require_value(receiver.try_receive());
    ASSERT_TRUE(message);
    EXPECT_EQ(received_value(*message), value);
    EXPECT_EQ(message->_header._publication, id);
    EXPECT_EQ(message->_header._queue, _options._queue);
    EXPECT_EQ(message->_header._queueSequence, value);
    EXPECT_EQ(message->_header._consumer, require_value(receiver.claim()));
    EXPECT_TRUE(message->_header._enqueuedAt);
    EXPECT_TRUE(message->_header._dequeuedAt);
    EXPECT_EQ(message->_header._flags, 0xa55a33ccu);
    EXPECT_FALSE(require_value(receiver.try_receive()));
  }
  EXPECT_EQ(control_word(_options._connectionPath, credit_offset), 12u);
}

TEST_F(RecordedQueueTest, FullCreditPreservesTheUnacceptedIdAndStillResolvesAcceptedRetries)
{
  create();
  auto sender = producer();
  auto receiver = consumer();
  const auto bytes = support::recorded_queue_payload(_codec, 7);
  const auto message = outgoing(bytes);
  const auto firstId = require_value(sender.next_publication_id());
  const auto first = require_recorded_send(sender.try_send(firstId, message));
  const auto secondId = require_value(sender.next_publication_id());
  require_recorded_send(sender.try_send(secondId, message));
  const auto waitingId = require_value(sender.next_publication_id());
  const auto logBefore = file_bytes(_options._producerPaths._log);
  const auto queueBefore = file_bytes(_options._connectionPath);
  EXPECT_TRUE(std::holds_alternative<recorded::QueueCreditFull>(sender.try_send(waitingId, message)));
  EXPECT_FALSE(require_value(sender.lookup(waitingId)));
  EXPECT_EQ(file_bytes(_options._producerPaths._log), logBefore);
  EXPECT_EQ(file_bytes(_options._connectionPath), queueBefore);

  // Acceptance lookup precedes fresh credit admission. Retrying an accepted
  // ID never consumes a second slot, even while the logical Queue is full.
  const auto duplicate = require_recorded_send(sender.try_send(firstId, message));
  EXPECT_TRUE(duplicate._duplicate);
  EXPECT_EQ(duplicate._reference, first._reference);
  const auto changed = support::recorded_queue_payload(_codec, 8);
  expect_send_error(sender.try_send(firstId, outgoing(changed)), ErrorCode::Conflict);
  EXPECT_EQ(file_bytes(_options._producerPaths._log), logBefore);
  EXPECT_EQ(file_bytes(_options._connectionPath), queueBefore);

  ASSERT_TRUE(require_value(receiver.try_receive()));
  const auto accepted = require_recorded_send(sender.try_send(waitingId, message));
  EXPECT_EQ(accepted._queueSequence, 3u);
  const auto second = require_value(receiver.try_receive());
  ASSERT_TRUE(second);
  EXPECT_EQ(second->_header._publication, secondId);
  const auto third = require_value(receiver.try_receive());
  ASSERT_TRUE(third);
  EXPECT_EQ(third->_header._publication, waitingId);
  EXPECT_FALSE(require_value(receiver.try_receive()));
}

TEST_F(RecordedQueueTest, RetiredPublicationIsRejectedInsteadOfWaitingForQueueCredit)
{
  create();
  auto sender = producer();
  auto receiver = consumer();
  const auto bytes = support::recorded_queue_payload(_codec, 7);
  const auto message = outgoing(bytes);
  const auto retiredId = require_value(sender.next_publication_id());
  // Accepting a later ID retires this skipped ID. Additional ring credit
  // cannot make it eligible for a first acceptance again.
  const auto firstId = require_value(sender.next_publication_id());
  const auto first = require_recorded_send(sender.try_send(firstId, message));
  const auto secondId = require_value(sender.next_publication_id());
  require_recorded_send(sender.try_send(secondId, message));
  const auto waitingId = require_value(sender.next_publication_id());
  const auto logBefore = file_bytes(_options._producerPaths._log);
  const auto ownerBefore = file_bytes(_options._producerPaths._owner);
  const auto queueBefore = file_bytes(_options._connectionPath);
  expect_send_error(sender.try_send(retiredId, message), ErrorCode::InvalidArgument);
  EXPECT_FALSE(require_value(sender.lookup(retiredId)));
  EXPECT_TRUE(std::holds_alternative<recorded::QueueCreditFull>(sender.try_send(waitingId, message)));
  const auto duplicate = require_recorded_send(sender.try_send(firstId, message));
  EXPECT_TRUE(duplicate._duplicate);
  EXPECT_EQ(duplicate._reference, first._reference);
  EXPECT_EQ(file_bytes(_options._producerPaths._log), logBefore);
  EXPECT_EQ(file_bytes(_options._producerPaths._owner), ownerBefore);
  EXPECT_EQ(file_bytes(_options._connectionPath), queueBefore);

  ASSERT_TRUE(require_value(receiver.try_receive()));
  const auto queueWithCredit = file_bytes(_options._connectionPath);
  expect_send_error(sender.try_send(retiredId, message), ErrorCode::InvalidArgument);
  EXPECT_EQ(file_bytes(_options._producerPaths._log), logBefore);
  EXPECT_EQ(file_bytes(_options._producerPaths._owner), ownerBefore);
  EXPECT_EQ(file_bytes(_options._connectionPath), queueWithCredit);
  const auto accepted = require_recorded_send(sender.try_send(waitingId, message));
  EXPECT_FALSE(accepted._duplicate);
  EXPECT_EQ(accepted._queueSequence, 3u);
  const auto second = require_value(receiver.try_receive());
  ASSERT_TRUE(second);
  EXPECT_EQ(second->_header._publication, secondId);
  const auto third = require_value(receiver.try_receive());
  ASSERT_TRUE(third);
  EXPECT_EQ(third->_header._publication, waitingId);
  EXPECT_FALSE(require_value(receiver.try_receive()));
}

TEST_F(RecordedQueueTest, PreviousClaimUnacceptedPublicationIsRejectedEvenWhenQueueIsFull)
{
  create();
  auto receiver = consumer();
  const auto bytes = support::recorded_queue_payload(_codec, 7);
  const auto message = outgoing(bytes);
  PublicationId firstId;
  PublicationId abandonedId;
  recorded::ProducerAppendResult first;
  {
    auto original = producer();
    firstId = require_value(original.next_publication_id());
    first = require_recorded_send(original.try_send(firstId, message));
    abandonedId = require_value(original.next_publication_id());
  }
  // A replacement claim can resolve the old claim's accepted IDs, but cannot
  // accept an ID that the old claim merely issued before it closed.
  auto sender = producer();
  const auto secondId = require_value(sender.next_publication_id());
  ASSERT_NE(secondId._producer._incarnation, abandonedId._producer._incarnation);
  require_recorded_send(sender.try_send(secondId, message));
  const auto waitingId = require_value(sender.next_publication_id());
  const auto logBefore = file_bytes(_options._producerPaths._log);
  const auto ownerBefore = file_bytes(_options._producerPaths._owner);
  const auto queueBefore = file_bytes(_options._connectionPath);
  expect_send_error(sender.try_send(abandonedId, message), ErrorCode::InvalidArgument);
  EXPECT_FALSE(require_value(sender.lookup(abandonedId)));
  EXPECT_TRUE(std::holds_alternative<recorded::QueueCreditFull>(sender.try_send(waitingId, message)));
  const auto duplicate = require_recorded_send(sender.try_send(firstId, message));
  EXPECT_TRUE(duplicate._duplicate);
  EXPECT_EQ(duplicate._reference, first._reference);
  EXPECT_EQ(file_bytes(_options._producerPaths._log), logBefore);
  EXPECT_EQ(file_bytes(_options._producerPaths._owner), ownerBefore);
  EXPECT_EQ(file_bytes(_options._connectionPath), queueBefore);

  ASSERT_TRUE(require_value(receiver.try_receive()));
  const auto queueWithCredit = file_bytes(_options._connectionPath);
  expect_send_error(sender.try_send(abandonedId, message), ErrorCode::InvalidArgument);
  EXPECT_EQ(file_bytes(_options._producerPaths._log), logBefore);
  EXPECT_EQ(file_bytes(_options._producerPaths._owner), ownerBefore);
  EXPECT_EQ(file_bytes(_options._connectionPath), queueWithCredit);
  const auto accepted = require_recorded_send(sender.try_send(waitingId, message));
  EXPECT_FALSE(accepted._duplicate);
  EXPECT_EQ(accepted._queueSequence, 3u);
  const auto second = require_value(receiver.try_receive());
  ASSERT_TRUE(second);
  EXPECT_EQ(second->_header._publication, secondId);
  const auto third = require_value(receiver.try_receive());
  ASSERT_TRUE(third);
  EXPECT_EQ(third->_header._publication, waitingId);
  EXPECT_FALSE(require_value(receiver.try_receive()));
}

TEST_F(RecordedQueueTest, EmptyPollsDoNotChangeTheLogOrConnection)
{
  create();
  auto receiver = consumer();
  const auto logBefore = file_bytes(_options._producerPaths._log);
  const auto queueBefore = file_bytes(_options._connectionPath);
  for (unsigned poll = 0; poll != 64; ++poll)
  {
    EXPECT_FALSE(require_value(receiver.try_receive()));
  }
  EXPECT_EQ(file_bytes(_options._producerPaths._log), logBefore);
  EXPECT_EQ(file_bytes(_options._connectionPath), queueBefore);
  EXPECT_EQ(control_word(_options._connectionPath, credit_offset), 0u);
}

TEST_F(RecordedQueueTest, ExactSchemaAndBinaryPayloadAdmissionRejectBeforeAnyAcceptance)
{
  create();
  auto sender = producer();
  const auto bytes = support::recorded_queue_payload(_codec, 17);
  const auto id = require_value(sender.next_publication_id());
  const auto logBefore = file_bytes(_options._producerPaths._log);
  const auto queueBefore = file_bytes(_options._connectionPath);
  for (unsigned invalid = 0; invalid != 4; ++invalid)
  {
    SCOPED_TRACE(invalid);
    auto message = outgoing(bytes);
    switch (invalid)
    {
    case 0:
      ++message._header._schema._version;
      break;
    case 1:
      message._header._format = PayloadFormat::Text;
      break;
    case 2:
      --message._header._payloadBytes;
      break;
    default:
      message._payload = message._payload.first(7);
      message._header._payloadBytes = 7;
      break;
    }
    EXPECT_TRUE(std::holds_alternative<Error>(sender.try_send(id, message)));
    EXPECT_FALSE(require_value(sender.lookup(id)));
    EXPECT_EQ(file_bytes(_options._producerPaths._log), logBefore);
    EXPECT_EQ(file_bytes(_options._connectionPath), queueBefore);
  }
  // Rejection does not consume the publication identity; corrected input may
  // still use it for its first acceptance.
  EXPECT_FALSE(require_recorded_send(sender.try_send(id, outgoing(bytes)))._duplicate);
}

TEST_F(RecordedQueueTest, LogExhaustionDoesNotHidePreviouslyAcceptedOutcomes)
{
  _options._log._header._publicationCapacity = 2;
  create();
  auto sender = producer();
  auto receiver = consumer();
  const auto bytes = support::recorded_queue_payload(_codec, 99);
  const auto firstId = require_value(sender.next_publication_id());
  const auto first = require_recorded_send(sender.try_send(firstId, outgoing(bytes)));
  ASSERT_TRUE(require_value(receiver.try_receive()));
  require_recorded_send(sender.try_send(require_value(sender.next_publication_id()), outgoing(bytes)));
  ASSERT_TRUE(require_value(receiver.try_receive()));
  const auto before = file_bytes(_options._producerPaths._log);
  const auto thirdId = require_value(sender.next_publication_id());
  expect_send_error(sender.try_send(thirdId, outgoing(bytes)), ErrorCode::LogFull);
  EXPECT_FALSE(require_value(sender.lookup(thirdId)));
  const auto retried = require_recorded_send(sender.try_send(firstId, outgoing(bytes)));
  EXPECT_TRUE(retried._duplicate);
  EXPECT_EQ(retried._reference, first._reference);
  EXPECT_EQ(file_bytes(_options._producerPaths._log), before);
}

TEST_F(RecordedQueueTest, OwningCopiesRetainTheExactHeaderAndCodecAfterWrapAndEndpointDestruction)
{
  create();
  Message copied;
  MessageHeader originalHeader;
  {
    auto sender = producer();
    auto receiver = consumer();
    const auto bytes = support::recorded_queue_payload(_codec, 123456789);
    const auto id = require_value(sender.next_publication_id());
    require_recorded_send(sender.try_send(id, outgoing(bytes)));
    const auto message = require_value(receiver.try_receive());
    ASSERT_TRUE(message);
    originalHeader = message->_header;
    copied = require_value(message->copy_message(8));
    EXPECT_EQ(copied._header, originalHeader);
    EXPECT_EQ(copied.schema_session(), _options._log._header._session);
    EXPECT_EQ(std::get<std::uint64_t>(require_value(copied.decode(_codec)).at(0)), 123456789u);
    for (std::uint64_t value = 0; value != 8; ++value)
    {
      const auto nextBytes = support::recorded_queue_payload(_codec, value);
      require_recorded_send(sender.try_send(require_value(sender.next_publication_id()), outgoing(nextBytes)));
      ASSERT_TRUE(require_value(receiver.try_receive()));
    }
  }
  EXPECT_EQ(copied._header, originalHeader);
  EXPECT_EQ(copied.view()._header, originalHeader);
  EXPECT_EQ(std::get<std::uint64_t>(require_value(copied.decode(_codec)).at(0)), 123456789u);
  EXPECT_EQ(require_value(copied.view().copy_message(8))._header, originalHeader);
}

TEST_F(RecordedQueueTest, ReplacingAConsumerReplaysCreditedHistoryWithoutReturningCreditTwice)
{
  create();
  auto sender = producer();
  const auto bytes = support::recorded_queue_payload(_codec, 5);
  require_recorded_send(sender.try_send(require_value(sender.next_publication_id()), outgoing(bytes)));
  require_recorded_send(sender.try_send(require_value(sender.next_publication_id()), outgoing(bytes)));
  TaskClaim priorClaim;
  {
    auto original = consumer();
    priorClaim = require_value(original.claim());
    ASSERT_TRUE(require_value(original.try_receive()));
    ASSERT_TRUE(require_value(original.try_receive()));
  }
  EXPECT_EQ(control_word(_options._connectionPath, credit_offset), 2u);
  // Freed ring slots may now contain newer descriptors. A replacement starts
  // at Queue sequence 1 and recovers older deliveries from the immutable log.
  require_recorded_send(sender.try_send(require_value(sender.next_publication_id()), outgoing(bytes)));
  require_recorded_send(sender.try_send(require_value(sender.next_publication_id()), outgoing(bytes)));
  auto replacement = consumer();
  const auto current = require_value(replacement.claim());
  EXPECT_GT(current._incarnation, priorClaim._incarnation);
  for (std::uint64_t sequence = 1; sequence <= 4; ++sequence)
  {
    const auto message = require_value(replacement.try_receive());
    ASSERT_TRUE(message);
    EXPECT_EQ(message->_header._queueSequence, sequence);
    EXPECT_EQ(message->_header._consumer, current);
    EXPECT_EQ(control_word(_options._connectionPath, credit_offset), std::max<std::uint64_t>(2, sequence));
  }
  EXPECT_FALSE(require_value(replacement.try_receive()));
}

TEST_F(RecordedQueueTest, MismatchedFrozenContextRejectsBeforeAnyOwnershipOrDataWrite)
{
  create();
  const auto data = file_bytes(_options._producerPaths._log);
  const auto owner = file_bytes(_options._producerPaths._owner);
  const auto connection = file_bytes(_options._connectionPath);
  for (unsigned mismatch = 0; mismatch != 6; ++mismatch)
  {
    SCOPED_TRACE(mismatch);
    auto options = _options;
    switch (mismatch)
    {
    case 0:
      ++options._generation;
      break;
    case 1:
      options._catalog._queues.front()._descriptor._producer = options._consumer;
      break;
    case 2:
      --options._log._header._catalogRevision;
      break;
    case 3:
      ++options._schemaReference;
      break;
    case 4:
      options._catalog._sources.front()._state = EntityState::Draft;
      break;
    default:
      options._log._queues.push_back(QueueId{5});
      break;
    }
    EXPECT_TRUE(std::holds_alternative<Error>(recorded::RecordedProducer::open(options)));
    EXPECT_TRUE(std::holds_alternative<Error>(recorded::RecordedConsumer::open(options)));
    EXPECT_EQ(file_bytes(_options._producerPaths._log), data);
    EXPECT_EQ(file_bytes(_options._producerPaths._owner), owner);
    EXPECT_EQ(file_bytes(_options._connectionPath), connection);
  }
}

TEST_F(RecordedQueueTest, DuplicateProducerAndConsumerClaimsAreBusyAcrossIndependentOpens)
{
  create();
  auto sender = producer();
  {
    auto receiver = consumer();
    expect_error(recorded::RecordedProducer::open(_options), ErrorCode::OwnershipBusy);
    expect_error(recorded::RecordedConsumer::open(_options), ErrorCode::OwnershipBusy);
  }
  support::NativeProcess worker({ANGARIA_RECORDED_QUEUE_WORKER, _directory.path().string(), "hold-consumer"});
  worker.expect('O');
  EXPECT_NE(worker.receive<std::uint64_t>(), 0u);
  expect_error(recorded::RecordedConsumer::open(_options), ErrorCode::OwnershipBusy);
  worker.proceed();
  worker.wait_success();
  auto replacement = consumer();
  EXPECT_NE(require_value(replacement.claim())._incarnation, 0u);
}

TEST_F(RecordedQueueTest, ForeignThreadCallsRejectBeforePublicationOrCreditChanges)
{
  create();
  auto sender = producer();
  auto receiver = consumer();
  const auto bytes = support::recorded_queue_payload(_codec, 1);
  const auto id = require_value(sender.next_publication_id());
  const auto data = file_bytes(_options._producerPaths._log);
  const auto connection = file_bytes(_options._connectionPath);
  bool wrongProducerClaim{};
  bool wrongPublication{};
  bool wrongSend{};
  bool wrongLookup{};
  bool wrongConsumerClaim{};
  bool wrongReceive{};
  std::thread foreign([&]
  {
    wrongProducerClaim = support::has_error(sender.claim(), ErrorCode::WrongOwner);
    wrongPublication = support::has_error(sender.next_publication_id(), ErrorCode::WrongOwner);
    const auto sent = sender.try_send(id, outgoing(bytes));
    const auto* error = std::get_if<Error>(&sent);
    wrongSend = error && error->_code == ErrorCode::WrongOwner;
    wrongLookup = support::has_error(sender.lookup(id), ErrorCode::WrongOwner);
    wrongConsumerClaim = support::has_error(receiver.claim(), ErrorCode::WrongOwner);
    wrongReceive = support::has_error(receiver.try_receive(), ErrorCode::WrongOwner);
  });
  foreign.join();
  EXPECT_TRUE(wrongProducerClaim);
  EXPECT_TRUE(wrongPublication);
  EXPECT_TRUE(wrongSend);
  EXPECT_TRUE(wrongLookup);
  EXPECT_TRUE(wrongConsumerClaim);
  EXPECT_TRUE(wrongReceive);
  EXPECT_EQ(file_bytes(_options._producerPaths._log), data);
  EXPECT_EQ(file_bytes(_options._connectionPath), connection);
  require_recorded_send(sender.try_send(id, outgoing(bytes)));
  ASSERT_TRUE(require_value(receiver.try_receive()));
}

TEST_F(RecordedQueueTest, InheritedForkHandlesRejectWithoutReleasingParentClaims)
{
  create();
  auto sender = producer();
  auto receiver = consumer();
  const auto child = ::fork();
  ASSERT_GE(child, 0);
  if (child == 0)
  {
    ::alarm(3);
    const bool producerRejected = support::has_error(sender.next_publication_id(), ErrorCode::WrongOwner);
    const bool consumerRejected = support::has_error(receiver.try_receive(), ErrorCode::WrongOwner);
    sender = recorded::RecordedProducer{};
    receiver = recorded::RecordedConsumer{};
    ::_exit((producerRejected ? 1 : 0) | (consumerRejected ? 2 : 0));
  }
  const auto status = support::wait_for_exit(child, "Cannot reap recorded Queue child", "Recorded Queue fork deadline");
  EXPECT_EQ(support::exit_code(status), 3);
  expect_error(recorded::RecordedProducer::open(_options), ErrorCode::OwnershipBusy);
  expect_error(recorded::RecordedConsumer::open(_options), ErrorCode::OwnershipBusy);
  const auto bytes = support::recorded_queue_payload(_codec, 42);
  require_recorded_send(sender.try_send(require_value(sender.next_publication_id()), outgoing(bytes)));
  ASSERT_TRUE(require_value(receiver.try_receive()));
}

TEST_F(RecordedQueueTest, IndependentExecConsumerReceivesTheActualMappedMessage)
{
  create();
  support::NativeProcess worker({ANGARIA_RECORDED_QUEUE_WORKER, _directory.path().string(), "consume"});
  worker.expect('O');
  EXPECT_NE(worker.receive<std::uint64_t>(), 0u);
  auto sender = producer();
  const auto bytes = support::recorded_queue_payload(_codec, 8675309);
  require_recorded_send(sender.try_send(require_value(sender.next_publication_id()), outgoing(bytes)));
  worker.proceed();
  worker.expect('A');
  EXPECT_EQ(worker.receive<std::uint64_t>(), 8675309u);
  EXPECT_EQ(worker.receive<std::uint64_t>(), 1u);
  worker.wait_success();
  EXPECT_EQ(control_word(_options._connectionPath, credit_offset), 1u);
}

TEST_F(RecordedQueueTest, AConsumerFindsCommittedDataBeforeTheLiveProducerPublishesItsHint)
{
  // A descriptor is an acceleration hint. Every checkpoint has already passed
  // the log commit, so a paused writer cannot hide acceptance from its consumer.
  for (const char* mode : {"commit", "stamp", "published"})
  {
    SCOPED_TRACE(mode);
    support::ProducerLogDirectory directory;
    const auto options = support::recorded_queue_options(directory.path());
    require_value(recorded::create_recorded_queue(options));
    auto receiver = require_value(recorded::RecordedConsumer::open(options));
    support::NativeProcess worker({ANGARIA_RECORDED_QUEUE_WORKER, directory.path().string(), mode});
    worker.expect('O');
    const auto incarnation = worker.receive<std::uint64_t>();
    worker.expect('R');
    const auto first = require_value(receiver.try_receive());
    ASSERT_TRUE(first);
    EXPECT_EQ(received_value(*first), 42u);
    EXPECT_EQ(first->_header._publication._producer._incarnation, incarnation);
    EXPECT_EQ(first->_header._queueSequence, 1u);
    EXPECT_EQ(control_word(options._connectionPath, credit_offset), 1u);
    // Returning credit before a late descriptor arrives must neither redeliver
    // that old hint nor prevent the same live producer's next message.
    worker.proceed();
    worker.expect('A');
    EXPECT_EQ(worker.receive<std::uint64_t>(), 1u);
    EXPECT_FALSE(require_value(receiver.try_receive()));
    worker.proceed();
    worker.expect('A');
    EXPECT_EQ(worker.receive<std::uint64_t>(), 2u);
    const auto second = require_value(receiver.try_receive());
    ASSERT_TRUE(second);
    EXPECT_EQ(received_value(*second), 84u);
    EXPECT_EQ(second->_header._queueSequence, 2u);
    EXPECT_EQ(control_word(options._connectionPath, credit_offset), 2u);
    EXPECT_FALSE(require_value(receiver.try_receive()));
    worker.wait_success();
  }
}

TEST_F(RecordedQueueTest, ACopiedConnectionCannotCreateAnIndependentCreditAndConsumerHistory)
{
  create();
  auto copied = _options;
  copied._connectionPath = _directory.path() / "copied.connection";
  std::filesystem::copy_file(_options._connectionPath, copied._connectionPath);
  const auto data = file_bytes(_options._producerPaths._log);
  const auto owner = file_bytes(_options._producerPaths._owner);
  const auto connection = file_bytes(_options._connectionPath);
  const auto alias = file_bytes(copied._connectionPath);
  // These native resources are bound to their physical files. Copying an
  // individual connection cannot fork its credit or ownership control plane.
  expect_error(recorded::RecordedProducer::open(copied), ErrorCode::ConfigurationMismatch);
  expect_error(recorded::RecordedConsumer::open(copied), ErrorCode::ConfigurationMismatch);
  EXPECT_EQ(file_bytes(_options._producerPaths._log), data);
  EXPECT_EQ(file_bytes(_options._producerPaths._owner), owner);
  EXPECT_EQ(file_bytes(_options._connectionPath), connection);
  EXPECT_EQ(file_bytes(copied._connectionPath), alias);
}

TEST_F(RecordedQueueTest, AnUncertainCommittedSendResolvesAndTheSameProducerContinues)
{
  create();
  ErrorAtBoundary interruption{recorded::ProducerLogCheckpoint::AfterCommit};
  auto sender = require_value(recorded::RecordedProducer::open(_options, &interruption));
  auto receiver = consumer();
  const auto bytes = support::recorded_queue_payload(_codec, 42);
  const auto firstId = require_value(sender.next_publication_id());
  expect_send_error(sender.try_send(firstId, outgoing(bytes)), ErrorCode::OutcomeUnknown);
  ASSERT_TRUE(interruption.fired());
  const auto outcome = require_value(sender.lookup(firstId));
  ASSERT_TRUE(outcome);
  EXPECT_EQ(outcome->_queueSequence, 1u);
  const auto first = require_value(receiver.try_receive());
  ASSERT_TRUE(first);
  EXPECT_EQ(first->_header._publication, firstId);
  EXPECT_EQ(first->_header._queueSequence, 1u);
  EXPECT_EQ(control_word(_options._connectionPath, credit_offset), 1u);

  // The endpoint must reconcile accepted progress even when append's normal
  // return was interrupted. Otherwise this next credit check uses stale state.
  const auto secondId = require_value(sender.next_publication_id());
  const auto second = require_recorded_send(sender.try_send(secondId, outgoing(bytes)));
  EXPECT_EQ(second._queueSequence, 2u);
  const auto message = require_value(receiver.try_receive());
  ASSERT_TRUE(message);
  EXPECT_EQ(message->_header._publication, secondId);
  EXPECT_EQ(message->_header._queueSequence, 2u);
  EXPECT_FALSE(require_value(receiver.try_receive()));
}

TEST_F(RecordedQueueTest, InterruptedDeliveryClosesTheHandleAndReplacementNeverReturnsCreditTwice)
{
  for (const auto point : {recorded::RecordedQueueCheckpoint::DeliveryBeforeCredit,
                           recorded::RecordedQueueCheckpoint::DeliveryAfterCredit})
  {
    SCOPED_TRACE(static_cast<int>(point));
    support::ProducerLogDirectory directory;
    const auto options = support::recorded_queue_options(directory.path());
    require_value(recorded::create_recorded_queue(options));
    auto sender = require_value(recorded::RecordedProducer::open(options));
    const auto bytes = support::recorded_queue_payload(_codec, 42);
    const auto message = support::recorded_queue_message(options, bytes);
    const auto firstId = require_value(sender.next_publication_id());
    require_recorded_send(sender.try_send(firstId, message));
    require_recorded_send(sender.try_send(require_value(sender.next_publication_id()), message));
    const auto waiting = require_value(sender.next_publication_id());
    EXPECT_TRUE(std::holds_alternative<recorded::QueueCreditFull>(sender.try_send(waiting, message)));
    const auto data = file_bytes(options._producerPaths._log);
    ErrorAtBoundary interruption{point};
    auto original = require_value(recorded::RecordedConsumer::open(options, &interruption));
    const auto priorClaim = require_value(original.claim());
    expect_error(original.try_receive(), ErrorCode::IoError);
    ASSERT_TRUE(interruption.fired());
    const std::uint64_t returned = point == recorded::RecordedQueueCheckpoint::DeliveryAfterCredit ? 1 : 0;
    EXPECT_EQ(control_word(options._connectionPath, credit_offset), returned);
    expect_error(original.try_receive(), ErrorCode::IoError);
    expect_error(original.claim(), ErrorCode::IoError);
    EXPECT_EQ(control_word(options._connectionPath, credit_offset), returned);
    EXPECT_EQ(file_bytes(options._producerPaths._log), data);
    original = recorded::RecordedConsumer{};

    // Unacknowledged credit is transport capacity, not a processing receipt.
    // A replacement deliberately replays from sequence 1 in either outcome.
    auto replacement = require_value(recorded::RecordedConsumer::open(options));
    EXPECT_GT(require_value(replacement.claim())._incarnation, priorClaim._incarnation);
    const auto first = require_value(replacement.try_receive());
    ASSERT_TRUE(first);
    EXPECT_EQ(first->_header._publication, firstId);
    EXPECT_EQ(first->_header._queueSequence, 1u);
    EXPECT_EQ(control_word(options._connectionPath, credit_offset), 1u);
    EXPECT_EQ(require_recorded_send(sender.try_send(waiting, message))._queueSequence, 3u);
    for (std::uint64_t sequence = 2; sequence <= 3; ++sequence)
    {
      const auto next = require_value(replacement.try_receive());
      ASSERT_TRUE(next);
      EXPECT_EQ(next->_header._queueSequence, sequence);
      EXPECT_EQ(control_word(options._connectionPath, credit_offset), sequence);
    }
    EXPECT_FALSE(require_value(replacement.try_receive()));
  }
}
