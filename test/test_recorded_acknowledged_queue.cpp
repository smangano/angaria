// Private fixed-topology acknowledged endpoints. These tests are executable
// contract examples, not public profile admission or application configuration.
// Their exec peers interrupt the actual mapped progress implementation.
#include "angaria/runtime.h"
#include "angaria/detail/byte_operations.h"
#include "recorded_acknowledged_fixture.h"
#include "recorded_queue_independence_fixture.h"
#include "native_process.h"
#include "result_assertions.h"
#include "session_controller.h"

#include <gtest/gtest.h>

#include <array>
#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <variant>
#include <vector>

#ifndef ANGARIA_RECORDED_ACKNOWLEDGED_QUEUE_WORKER
#error ANGARIA_RECORDED_ACKNOWLEDGED_QUEUE_WORKER must name the independent acknowledged worker
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
  using support::recorded_test_time;

  Delivery require_delivery(Result<std::optional<Delivery>> result)
  {
    auto delivery = require_value(std::move(result));
    if (!delivery)
    {
      throw std::runtime_error("Expected an acknowledged Queue delivery");
    }
    return *delivery;
  }

  class ArmedProgressError final : public recorded::ProgressObserver
  {
  public:
    explicit ArmedProgressError(recorded::ProgressCheckpoint point) : _point(point)
    {
    }

    OperationResult checkpoint(recorded::ProgressCheckpoint point) override
    {
      if (_armed && point == _point)
      {
        _armed = false;
        return Error{ErrorCode::IoError, "Injected progress boundary failure", "acknowledged_queue_test"};
      }
      return std::monostate{};
    }

    bool _armed{};

  private:
    recorded::ProgressCheckpoint _point;
  };

  class RecordedAcknowledgedQueueTest : public testing::Test
  {
  protected:
    void create(std::uint64_t publications = 128)
    {
      _options = support::recorded_acknowledged_options(_directory.path(), publications);
      require_value(recorded::create_recorded_queue(_options));
    }

    PublicationId send(recorded::RecordedProducer& sender, std::uint64_t value = 42)
    {
      const auto bytes = support::recorded_queue_payload(_codec, value);
      const auto id = require_value(sender.next_publication_id());
      require_recorded_send(sender.try_send(id, support::recorded_queue_message(_options, bytes)));
      return id;
    }

    std::uint64_t decoded(const MessageView& view)
    {
      return std::get<std::uint64_t>(require_value(_codec.decode(view._payload)).at(0));
    }

    std::vector<std::string> worker(const char* operation, const char* point) const
    {
      return {ANGARIA_RECORDED_ACKNOWLEDGED_QUEUE_WORKER, _directory.path().string(), operation, point};
    }

    support::ProducerLogDirectory _directory;
    recorded::RecordedQueueOptions _options{support::recorded_acknowledged_options(_directory.path())};
    BinaryPayloadCodec _codec{support::recorded_queue_codec(_options)};
  };

  void seed_progress_generation(const recorded::RecordedQueueOptions& options, std::uint64_t generation)
  {
    // Deliberate exhaustion fixture with both consumer handles closed. Preserve
    // all original identity/state bytes, copy the selected image to the target
    // parity slot, and raise only its generation and the two control words.
    auto file = require_value(mapped::NativeFile::open_existing(
        support::recorded_progress_path(options), mapped::FileAccess::ReadWrite));
    auto mapping = require_value(file.map_read_write(0, require_value(file.size())));
    auto bytes = require_value(mapping.writable_bytes());
    auto selector = require_value(mapped::AtomicWord::bind(bytes, recorded::progress_selector_offset));
    auto applied = require_value(mapped::AtomicWord::bind(bytes, recorded::progress_applied_offset));
    const auto previous = selector.load_acquire();
    std::array<std::byte, recorded::progress_image_bytes> image{};
    const auto selectedOffset = recorded::progress_image_offset + (previous & 1) * recorded::progress_image_bytes;
    std::copy_n(bytes.begin() + selectedOffset, image.size(), image.begin());
    angaria::detail::write_unsigned_le<std::uint64_t>(image, 0, generation);
    const auto target = recorded::progress_image_offset + (generation & 1) * recorded::progress_image_bytes;
    std::copy(image.begin(), image.end(), bytes.begin() + target);
    selector.publish_control(generation);
    applied.publish_control(generation);
  }
}

TEST_F(RecordedAcknowledgedQueueTest, APendingReceiptCannotDescribeAnActivePinOrReviveItsFailedHandle)
{
  create();
  auto sender = require_value(recorded::RecordedProducer::open(_options));
  auto receiver = require_value(recorded::RecordedAcknowledgedConsumer::open(_options));
  const auto id = send(sender);
  const auto delivery = require_delivery(receiver.try_receive(recorded_test_time(0)));
  const auto copied = require_value(delivery._message.copy_message(8));
  auto file = require_value(mapped::NativeFile::open_existing(
      support::recorded_progress_path(_options), mapped::FileAccess::ReadWrite));
  auto mapping = require_value(file.map_read_write(recorded::progress_receipts_offset, recorded::progress_receipt_bytes));
  auto bytes = require_value(mapping.writable_bytes());

  // Deliberate stored-state corruption, not a supported external writer: all
  // issuance fields remain, but Pending contradicts the Source's active pin.
  // Inspection must reject it before an acknowledgment can normalize the row.
  angaria::detail::write_unsigned_le<std::uint64_t>(bytes, 0,
      static_cast<std::uint64_t>(recorded::ProgressReceiptState::Pending));
  expect_error(receiver.receipt(1), ErrorCode::IoError);
  angaria::detail::write_unsigned_le<std::uint64_t>(bytes, 0,
      static_cast<std::uint64_t>(recorded::ProgressReceiptState::Issued));
  expect_error(receiver.try_receive(), ErrorCode::IoError);
  EXPECT_EQ(copied._header._publication, id);
  EXPECT_EQ(std::get<std::uint64_t>(require_value(copied.decode(_codec)).at(0)), 42u);

  receiver = recorded::RecordedAcknowledgedConsumer{};
  auto replacement = require_value(recorded::RecordedAcknowledgedConsumer::open(_options));
  const auto recovered = require_delivery(replacement.try_receive());
  EXPECT_EQ(recovered._message._header._publication, id);
  EXPECT_NE(recovered._token._consumer, delivery._token._consumer);
  require_value(replacement.acknowledge(recovered._token));
}

TEST_F(RecordedAcknowledgedQueueTest, PinnedReceiveKeepsItsTokenAndEpochZeroTimeWithoutAnotherCommit)
{
  create();
  auto sender = require_value(recorded::RecordedProducer::open(_options));
  auto receiver = require_value(recorded::RecordedAcknowledgedConsumer::open(_options));
  EXPECT_FALSE(require_value(receiver.try_receive()));
  const auto id = send(sender);
  const auto first = require_delivery(receiver.try_receive(recorded_test_time(0)));
  EXPECT_EQ(first._message._header._publication, id);
  EXPECT_EQ(first._token._consumer, require_value(receiver.claim()));
  EXPECT_EQ(first._token._sourceSet, _options._source);
  EXPECT_EQ(first._token._queue, _options._queue);
  EXPECT_EQ(first._token._sequence, 1u);
  EXPECT_EQ(first._message._header._dequeuedAt, recorded_test_time(0));
  const auto committed = require_value(receiver.progress());
  for (unsigned retry = 0; retry != 5; ++retry)
  {
    const auto repeated = require_delivery(receiver.try_receive(recorded_test_time(1234)));
    EXPECT_EQ(repeated._token, first._token);
    EXPECT_EQ(repeated._message._header, first._message._header);
    EXPECT_EQ(repeated._message._payload.data(), first._message._payload.data());
    EXPECT_EQ(require_value(receiver.progress())._generation, committed._generation);
  }
  // A successful receive frees transport credit, not a processing obligation.
  EXPECT_EQ(support::recorded_queue_credit(_options), 1u);
  EXPECT_EQ(require_value(sender.acknowledged_sequence()), 0u);
  const auto issued = require_value(receiver.receipt(1));
  EXPECT_EQ(issued._state, recorded::ProgressReceiptState::Issued);
  EXPECT_EQ(issued._dequeuedAt, recorded_test_time(0));
  EXPECT_FALSE(issued._acknowledgedAt);
  require_value(receiver.acknowledge(first._token, recorded_test_time(-7)));
  EXPECT_EQ(require_value(sender.acknowledged_sequence()), 1u);
  const auto acknowledged = require_value(receiver.receipt(1));
  EXPECT_EQ(acknowledged._state, recorded::ProgressReceiptState::Acknowledged);
  EXPECT_EQ(acknowledged._dequeuedAt, recorded_test_time(0));
  EXPECT_EQ(acknowledged._acknowledgedAt, recorded_test_time(-7));
  EXPECT_FALSE(require_value(receiver.try_receive()));
}

TEST_F(RecordedAcknowledgedQueueTest, ReleaseRetainsHistoryAndStaleTokensCannotFinishItsRedelivery)
{
  create();
  auto sender = require_value(recorded::RecordedProducer::open(_options));
  auto receiver = require_value(recorded::RecordedAcknowledgedConsumer::open(_options));
  send(sender);
  const auto first = require_delivery(receiver.try_receive(recorded_test_time(-99)));
  require_value(receiver.release(first._token));
  const auto pending = require_value(receiver.receipt(1));
  EXPECT_EQ(pending._state, recorded::ProgressReceiptState::Pending);
  EXPECT_EQ(pending._consumerIncarnation, first._token._consumer._incarnation);
  EXPECT_EQ(pending._issuance, first._token._issuance);
  EXPECT_EQ(pending._dequeuedAt, recorded_test_time(-99));
  EXPECT_FALSE(pending._acknowledgedAt);
  const auto second = require_delivery(receiver.try_receive(recorded_test_time(0)));
  EXPECT_EQ(second._token._sequence, 1u);
  EXPECT_GT(second._token._issuance, first._token._issuance);
  const auto before = require_value(receiver.progress());
  expect_error(receiver.acknowledge(first._token), ErrorCode::StaleDelivery);
  expect_error(receiver.release(first._token), ErrorCode::StaleDelivery);
  EXPECT_EQ(require_value(receiver.progress())._generation, before._generation);
  EXPECT_EQ(decoded(second._message), 42u); // Known rejection preserves this borrow.
  EXPECT_EQ(require_delivery(receiver.try_receive())._token, second._token);
  require_value(receiver.acknowledge(second._token, recorded_test_time(0)));
  EXPECT_EQ(require_value(receiver.receipt(1))._acknowledgedAt, recorded_test_time(0));
  EXPECT_EQ(support::recorded_queue_credit(_options), 1u);
}

TEST_F(RecordedAcknowledgedQueueTest, EveryTokenFieldMustMatchTheCurrentPin)
{
  create();
  auto sender = require_value(recorded::RecordedProducer::open(_options));
  auto receiver = require_value(recorded::RecordedAcknowledgedConsumer::open(_options));
  send(sender);
  const auto delivery = require_delivery(receiver.try_receive());
  const auto generation = require_value(receiver.progress())._generation;
  for (unsigned field = 0; field != 8; ++field)
  {
    auto stale = delivery._token;
    switch (field)
    {
    case 0: ++stale._consumer._session._high; break;
    case 1: ++stale._consumer._session._low; break;
    case 2: stale._consumer._task = TaskId{7}; break;
    case 3: ++stale._consumer._incarnation; break;
    case 4: stale._sourceSet = SourceQueueSetId{3}; break;
    case 5: stale._queue = QueueId{5}; break;
    case 6: ++stale._sequence; break;
    case 7: ++stale._issuance; break;
    }
    expect_error(receiver.acknowledge(stale), ErrorCode::StaleDelivery);
    expect_error(receiver.release(stale), ErrorCode::StaleDelivery);
    EXPECT_EQ(require_value(receiver.progress())._generation, generation);
    EXPECT_EQ(decoded(delivery._message), 42u);
  }
  require_value(receiver.acknowledge(delivery._token));
  expect_error(receiver.acknowledge(delivery._token), ErrorCode::StaleDelivery);
  expect_error(receiver.release(delivery._token), ErrorCode::StaleDelivery);
}

TEST_F(RecordedAcknowledgedQueueTest, CloseReleasesItsPinAndReplacementPreservesTheAcknowledgedPrefix)
{
  create();
  auto sender = require_value(recorded::RecordedProducer::open(_options));
  const auto firstId = send(sender, 11);
  const auto secondId = send(sender, 22);
  auto receiver = require_value(recorded::RecordedAcknowledgedConsumer::open(_options));
  const auto first = require_delivery(receiver.try_receive(recorded_test_time(-11)));
  Message copy = require_value(first._message.copy_message(8));
  require_value(receiver.acknowledge(first._token, recorded_test_time(0)));
  const auto second = require_delivery(receiver.try_receive(recorded_test_time(-22)));
  const auto predecessor = second._token;
  require_value(receiver.close());
  expect_error(receiver.try_receive(), ErrorCode::InvalidState);
  receiver = {};
  auto replacement = require_value(recorded::RecordedAcknowledgedConsumer::open(_options));
  EXPECT_EQ(require_value(replacement.progress())._source._acknowledged, 1u);
  EXPECT_EQ(require_value(replacement.receipt(1))._acknowledgedAt, recorded_test_time(0));
  const auto redelivered = require_delivery(replacement.try_receive(recorded_test_time(-33)));
  EXPECT_EQ(redelivered._message._header._publication, secondId);
  EXPECT_EQ(redelivered._token._sequence, 2u);
  EXPECT_GT(redelivered._token._consumer._incarnation, predecessor._consumer._incarnation);
  expect_error(replacement.acknowledge(predecessor), ErrorCode::StaleDelivery);
  require_value(replacement.acknowledge(redelivered._token));
  EXPECT_FALSE(require_value(replacement.try_receive()));
  replacement = {};
  sender = {};
  EXPECT_EQ(copy._header._publication, firstId);
  EXPECT_EQ(copy._header._dequeuedAt, recorded_test_time(-11));
  EXPECT_EQ(copy.view()._header, copy._header);
  EXPECT_EQ(std::get<std::uint64_t>(require_value(copy.decode(_codec)).at(0)), 11u);
}

TEST_F(RecordedAcknowledgedQueueTest, ReleasingAnOldCreditedPinDoesNotReadItsRecycledRingSlot)
{
  create();
  auto sender = require_value(recorded::RecordedProducer::open(_options));
  auto receiver = require_value(recorded::RecordedAcknowledgedConsumer::open(_options));
  const auto firstId = send(sender, 11);
  send(sender, 22);
  const auto first = require_delivery(receiver.try_receive());
  send(sender, 33); // Queue sequence 3 recycles sequence 1's descriptor.
  require_value(receiver.release(first._token));
  const auto again = require_delivery(receiver.try_receive());
  EXPECT_EQ(again._message._header._publication, firstId);
  EXPECT_EQ(decoded(again._message), 11u);
  EXPECT_EQ(support::recorded_queue_credit(_options), 1u);
  require_value(receiver.acknowledge(again._token));
  for (std::uint64_t sequence = 2; sequence <= 3; ++sequence)
  {
    const auto next = require_delivery(receiver.try_receive());
    EXPECT_EQ(next._token._sequence, sequence);
    EXPECT_EQ(decoded(next._message), sequence * 11);
    require_value(receiver.acknowledge(next._token));
  }
  EXPECT_FALSE(require_value(receiver.try_receive()));
}

TEST_F(RecordedAcknowledgedQueueTest, FullMessageLogStillPermitsReleaseAcknowledgmentAndReplacement)
{
  create(2);
  auto sender = require_value(recorded::RecordedProducer::open(_options));
  send(sender, 11);
  send(sender, 22);
  auto receiver = require_value(recorded::RecordedAcknowledgedConsumer::open(_options));
  const auto first = require_delivery(receiver.try_receive());
  require_value(receiver.acknowledge(first._token));
  const auto second = require_delivery(receiver.try_receive());
  require_value(receiver.release(second._token));
  const auto bytes = support::recorded_queue_payload(_codec, 33);
  const auto denied = sender.try_send(require_value(sender.next_publication_id()),
      support::recorded_queue_message(_options, bytes));
  const auto* error = std::get_if<Error>(&denied);
  ASSERT_NE(error, nullptr);
  EXPECT_EQ(error->_code, ErrorCode::LogFull);
  const auto repeated = require_delivery(receiver.try_receive());
  require_value(receiver.acknowledge(repeated._token, recorded_test_time(0)));
  require_value(receiver.close());
  receiver = {};
  auto replacement = require_value(recorded::RecordedAcknowledgedConsumer::open(_options));
  EXPECT_EQ(require_value(replacement.progress())._source._acknowledged, 2u);
  EXPECT_FALSE(require_value(replacement.try_receive()));
  // Processing completion has not reclaimed or overwritten the two messages.
  auto log = require_value(recorded::ProducerLogReader::open(_options._producerPaths._log, _options._log));
  auto cut = require_value(log.capture());
  EXPECT_EQ(cut.record_count(), 2u);
  for (std::uint64_t expected : {11u, 22u})
  {
    const auto record = require_value(log.next(cut));
    ASSERT_TRUE(record);
    EXPECT_EQ(std::get<std::uint64_t>(require_value(_codec.decode(record->_payload)).at(0)), expected);
  }
}

TEST_F(RecordedAcknowledgedQueueTest, APrecommitProgressErrorPreservesItsBorrowAndCommittedPin)
{
  create();
  auto sender = require_value(recorded::RecordedProducer::open(_options));
  send(sender);
  ArmedProgressError observer{recorded::ProgressCheckpoint::BeforeSelector};
  auto receiver = require_value(recorded::RecordedAcknowledgedConsumer::open(_options, nullptr, &observer));
  const auto delivery = require_delivery(receiver.try_receive(recorded_test_time(-23)));
  const auto before = require_value(receiver.progress());
  observer._armed = true;
  expect_error(receiver.acknowledge(delivery._token, recorded_test_time(-17)), ErrorCode::IoError);
  EXPECT_EQ(require_value(receiver.progress())._generation, before._generation);
  EXPECT_EQ(decoded(delivery._message), 42u);
  const auto same = require_delivery(receiver.try_receive());
  EXPECT_EQ(same._token, delivery._token);
  EXPECT_EQ(same._message._header._dequeuedAt, recorded_test_time(-23));
  require_value(receiver.acknowledge(same._token));
}

TEST_F(RecordedAcknowledgedQueueTest, AnUncertainIssueSettlesToItsOriginalTokenAndCapturedTime)
{
  create();
  auto sender = require_value(recorded::RecordedProducer::open(_options));
  send(sender);
  ArmedProgressError observer{recorded::ProgressCheckpoint::SelectorPublished};
  auto receiver = require_value(recorded::RecordedAcknowledgedConsumer::open(_options, nullptr, &observer));
  observer._armed = true;
  expect_error(receiver.try_receive(recorded_test_time(-23)), ErrorCode::OutcomeUnknown);
  const auto settled = require_delivery(receiver.try_receive(recorded_test_time(777)));
  EXPECT_EQ(settled._message._header._dequeuedAt, recorded_test_time(-23));
  EXPECT_EQ(settled._token._issuance, 1u);
  EXPECT_EQ(support::recorded_queue_credit(_options), 1u);
  require_value(receiver.acknowledge(settled._token));
}

TEST_F(RecordedAcknowledgedQueueTest, AnUncertainAcknowledgmentSettlesWithoutIssuingTheTerminalMessageAgain)
{
  create();
  auto sender = require_value(recorded::RecordedProducer::open(_options));
  send(sender, 11);
  const auto secondId = send(sender, 22);
  ArmedProgressError observer{recorded::ProgressCheckpoint::SelectorPublished};
  auto receiver = require_value(recorded::RecordedAcknowledgedConsumer::open(_options, nullptr, &observer));
  const auto delivery = require_delivery(receiver.try_receive(recorded_test_time(-23)));
  const auto copied = require_value(delivery._message.copy_message(8));
  observer._armed = true;
  expect_error(receiver.acknowledge(delivery._token, recorded_test_time(-17)), ErrorCode::OutcomeUnknown);
  // The previous borrow ends on uncertainty. Use the owning copy if bytes are
  // needed while settlement recovers the selected operation.
  EXPECT_EQ(std::get<std::uint64_t>(require_value(copied.decode(_codec)).at(0)), 11u);
  const auto next = require_delivery(receiver.try_receive());
  EXPECT_EQ(next._message._header._publication, secondId);
  EXPECT_EQ(next._token._sequence, 2u);
  EXPECT_EQ(require_value(receiver.receipt(1))._acknowledgedAt, recorded_test_time(-17));
  require_value(receiver.acknowledge(next._token));
}

TEST_F(RecordedAcknowledgedQueueTest, AForeignThreadCannotOperateThePinnedSource)
{
  create();
  auto sender = require_value(recorded::RecordedProducer::open(_options));
  send(sender);
  auto receiver = require_value(recorded::RecordedAcknowledgedConsumer::open(_options));
  const auto delivery = require_delivery(receiver.try_receive());
  bool receiveWrong{}, acknowledgeWrong{}, releaseWrong{};
  std::thread foreign([&]
  {
    receiveWrong = support::has_error(receiver.try_receive(), ErrorCode::WrongOwner);
    acknowledgeWrong = support::has_error(receiver.acknowledge(delivery._token), ErrorCode::WrongOwner);
    releaseWrong = support::has_error(receiver.release(delivery._token), ErrorCode::WrongOwner);
  });
  foreign.join();
  EXPECT_TRUE(receiveWrong);
  EXPECT_TRUE(acknowledgeWrong);
  EXPECT_TRUE(releaseWrong);
  EXPECT_EQ(decoded(delivery._message), 42u);
  EXPECT_EQ(require_delivery(receiver.try_receive())._token, delivery._token);
  require_value(receiver.acknowledge(delivery._token));
}

TEST_F(RecordedAcknowledgedQueueTest, AcknowledgmentModeIsBoundToItsStoredConnection)
{
  create();
  EXPECT_TRUE(std::filesystem::exists(support::recorded_progress_path(_options)));
  expect_error(recorded::RecordedConsumer::open(_options), ErrorCode::UnsupportedPolicy);
  auto unacknowledged = _options;
  unacknowledged._acknowledged = false;
  EXPECT_TRUE(std::holds_alternative<Error>(recorded::RecordedProducer::open(unacknowledged)));
  auto receiver = require_value(recorded::RecordedAcknowledgedConsumer::open(_options));
  expect_error(recorded::RecordedAcknowledgedConsumer::open(_options), ErrorCode::OwnershipBusy);
}

TEST_F(RecordedAcknowledgedQueueTest, FreshIssuanceReservesTheLastGenerationForItsTerminalResolution)
{
  create();
  auto sender = require_value(recorded::RecordedProducer::open(_options));
  send(sender, 11);
  send(sender, 22);
  {
    auto initial = require_value(recorded::RecordedAcknowledgedConsumer::open(_options));
    require_value(initial.close());
  }
  constexpr auto maximum = std::numeric_limits<std::uint64_t>::max();
  seed_progress_generation(_options, maximum - 3);
  auto receiver = require_value(recorded::RecordedAcknowledgedConsumer::open(_options));
  EXPECT_EQ(require_value(receiver.progress())._generation, maximum - 2);
  const auto last = require_delivery(receiver.try_receive(recorded_test_time(0)));
  EXPECT_EQ(require_value(receiver.progress())._generation, maximum - 1);
  EXPECT_EQ(require_delivery(receiver.try_receive())._token, last._token);
  require_value(receiver.acknowledge(last._token, recorded_test_time(-7)));
  EXPECT_EQ(require_value(receiver.progress())._generation, maximum);
  EXPECT_EQ(require_value(receiver.receipt(1))._acknowledgedAt, recorded_test_time(-7));
  expect_error(receiver.try_receive(), ErrorCode::CapacityExceeded);
  EXPECT_EQ(require_value(receiver.progress())._source._pinSequence, 0u);
  EXPECT_EQ(require_value(receiver.progress())._source._acknowledged, 1u);
  require_value(receiver.close());
}

TEST_F(RecordedAcknowledgedQueueTest, ASourceWithOnlyOneGenerationRemainingRejectsFreshIssuanceWithoutAPin)
{
  create();
  auto sender = require_value(recorded::RecordedProducer::open(_options));
  send(sender);
  {
    auto initial = require_value(recorded::RecordedAcknowledgedConsumer::open(_options));
    require_value(initial.close());
  }
  constexpr auto maximum = std::numeric_limits<std::uint64_t>::max();
  seed_progress_generation(_options, maximum - 2);
  auto receiver = require_value(recorded::RecordedAcknowledgedConsumer::open(_options));
  EXPECT_EQ(require_value(receiver.progress())._generation, maximum - 1);
  expect_error(receiver.try_receive(), ErrorCode::CapacityExceeded);
  EXPECT_EQ(require_value(receiver.progress())._source._pinSequence, 0u);
  EXPECT_EQ(require_value(receiver.progress())._source._issuance, 0u);
  EXPECT_EQ(support::recorded_queue_credit(_options), 0u);
}

TEST_F(RecordedAcknowledgedQueueTest, ExhaustedIssuanceStillRepeatsAndResolvesItsExistingPin)
{
  create();
  auto sender = require_value(recorded::RecordedProducer::open(_options));
  send(sender, 11);
  send(sender, 22);
  ArmedProgressError observer{recorded::ProgressCheckpoint::SelectorPublished};
  auto receiver = require_value(recorded::RecordedAcknowledgedConsumer::open(_options, nullptr, &observer));
  const auto first = require_delivery(receiver.try_receive(recorded_test_time(-23)));
  observer._armed = true;
  expect_error(receiver.release(first._token), ErrorCode::OutcomeUnknown);
  {
    // Deliberate bounded-counter fixture, after the owner has returned an
    // uncertain release and before its selected image is installed. Seeding
    // its coherent absolute after-values avoids billions of real issuances;
    // recovery still follows the production selector/install protocol.
    auto file = require_value(mapped::NativeFile::open_existing(
        support::recorded_progress_path(_options), mapped::FileAccess::ReadWrite));
    auto mapping = require_value(file.map_read_write(0, require_value(file.size())));
    auto bytes = require_value(mapping.writable_bytes());
    const auto selected = require_value(mapped::AtomicWord::bind(bytes, recorded::progress_selector_offset)).load_acquire();
    const auto applied = require_value(mapped::AtomicWord::bind(bytes, recorded::progress_applied_offset)).load_acquire();
    ASSERT_EQ(selected, applied + 1);
    const auto image = recorded::progress_image_offset + (selected & 1) * recorded::progress_image_bytes;
    constexpr auto previous = std::numeric_limits<std::uint64_t>::max() - 1;
    // Source starts at image+48; receipt starts at image+128. Their latest
    // issuance counters are independently explicit in the fixture bytes.
    angaria::detail::write_unsigned_le<std::uint64_t>(bytes, image + 56, previous);
    angaria::detail::write_unsigned_le<std::uint64_t>(bytes, image + 144, previous);
  }
  const auto last = require_delivery(receiver.try_receive(recorded_test_time(0)));
  EXPECT_EQ(last._token._issuance, std::numeric_limits<std::uint64_t>::max());
  const auto generation = require_value(receiver.progress())._generation;
  const auto repeated = require_delivery(receiver.try_receive(recorded_test_time(999)));
  EXPECT_EQ(repeated._token, last._token);
  EXPECT_EQ(repeated._message._header, last._message._header);
  EXPECT_EQ(require_value(receiver.progress())._generation, generation);
  require_value(receiver.acknowledge(last._token, recorded_test_time(-7)));
  const auto settled = require_value(receiver.progress());
  expect_error(receiver.try_receive(), ErrorCode::CapacityExceeded);
  expect_error(receiver.try_receive(), ErrorCode::CapacityExceeded);
  EXPECT_EQ(require_value(receiver.progress())._generation, settled._generation);
  EXPECT_EQ(require_value(receiver.progress())._source._pinSequence, 0u);
  EXPECT_EQ(support::recorded_queue_credit(_options), 1u);
}

TEST_F(RecordedAcknowledgedQueueTest, EmptyPollingStillReturnsEmptyWhenProgressGenerationsAreExhausted)
{
  create();
  auto sender = require_value(recorded::RecordedProducer::open(_options));
  {
    auto initial = require_value(recorded::RecordedAcknowledgedConsumer::open(_options));
    require_value(initial.close());
  }
  constexpr auto maximum = std::numeric_limits<std::uint64_t>::max();
  seed_progress_generation(_options, maximum - 1);
  auto receiver = require_value(recorded::RecordedAcknowledgedConsumer::open(_options));
  EXPECT_EQ(require_value(receiver.progress())._generation, maximum);
  EXPECT_FALSE(require_value(receiver.try_receive()));
  EXPECT_FALSE(require_value(receiver.try_receive()));
  send(sender);
  expect_error(receiver.try_receive(), ErrorCode::CapacityExceeded);
  EXPECT_EQ(support::recorded_queue_credit(_options), 0u);
  EXPECT_EQ(require_value(receiver.progress())._source._pinSequence, 0u);
  require_value(receiver.close());
}

namespace
{
  struct ProgressStop
  {
    const char* _operation;
    const char* _point;
    std::string _name;
    bool _committed;
  };

  std::vector<ProgressStop> progress_stops()
  {
    struct Boundary
    {
      const char* _point;
      const char* _name;
      bool _committed;
    };
    const std::array boundaries{
        Boundary{"partial-image", "PartialImage", false},
        Boundary{"image", "ImageWritten", false},
        Boundary{"before-selector", "BeforeSelector", false},
        Boundary{"selector", "SelectorPublished", true},
        Boundary{"partial-receipt", "PartialReceipt", true},
        Boundary{"receipt", "ReceiptInstalled", true},
        Boundary{"partial-source", "PartialSource", true},
        Boundary{"source", "SourceInstalled", true},
        Boundary{"before-applied", "BeforeApplied", true},
        Boundary{"applied", "AppliedPublished", true}};
    std::vector<ProgressStop> values;
    for (const auto* operation : {"issue", "ack", "release", "close"})
    {
      for (const auto boundary : boundaries)
      {
        // Issuance/acknowledgment qualify every byte/control boundary. Release
        // and close share that primitive and add representative precommit,
        // committed and partially installed operation cases.
        if ((std::string{operation} == "release" || std::string{operation} == "close") &&
            std::string{boundary._point} != "before-selector" && std::string{boundary._point} != "selector" &&
            std::string{boundary._point} != "partial-source")
        {
          continue;
        }
        values.push_back({operation, boundary._point, std::string{operation} + "_" + boundary._name, boundary._committed});
      }
    }
    return values;
  }

  struct InspectedProgress
  {
    recorded::ProgressSnapshot _snapshot;
    recorded::ProgressReceipt _receipt;
  };

  InspectedProgress inspect_owned_progress(const recorded::RecordedQueueOptions& options, std::uint64_t sequence = 1)
  {
    // This private inspection obeys the same native connection ownership as
    // the endpoint. Inspect BEFORE replacement attachment releases old pins,
    // otherwise that cleanup could mask an incorrect commit boundary.
    auto claim = require_value(mapped::NativeClaim::acquire_existing(options._connectionPath));
    auto store = require_value(recorded::ProgressStore::open(support::recorded_progress_options(options)));
    return {require_value(store.snapshot()), require_value(store.receipt(sequence))};
  }

  class RecordedAcknowledgedProgressDeath : public testing::TestWithParam<ProgressStop>
  {
  };
}

TEST_P(RecordedAcknowledgedProgressDeath, OnlyTheSelectedOperationSurvivesAndReplacementPreservesItsTime)
{
  const auto stop = GetParam();
  support::ProducerLogDirectory directory;
  const auto options = support::recorded_acknowledged_options(directory.path());
  const auto codec = support::recorded_queue_codec(options);
  const auto firstBytes = support::recorded_queue_payload(codec, 42);
  const auto secondBytes = support::recorded_queue_payload(codec, 84);
  require_value(recorded::create_recorded_queue(options));
  auto sender = require_value(recorded::RecordedProducer::open(options));
  const auto firstId = require_value(sender.next_publication_id());
  require_recorded_send(sender.try_send(firstId, support::recorded_queue_message(options, firstBytes)));
  const auto secondId = require_value(sender.next_publication_id());
  require_recorded_send(sender.try_send(secondId, support::recorded_queue_message(options, secondBytes)));
  support::NativeProcess child({ANGARIA_RECORDED_ACKNOWLEDGED_QUEUE_WORKER,
      directory.path().string(), stop._operation, stop._point});
  child.expect('O');
  const auto predecessorIncarnation = child.receive<std::uint64_t>();
  std::optional<DeliveryToken> predecessor;
  if (std::string{stop._operation} != "issue")
  {
    child.expect('D');
    predecessor = child.receive<DeliveryToken>();
  }
  child.expect('P');
  const auto previousGeneration = child.receive<std::uint64_t>();
  child.proceed();
  child.expect('R');
  expect_error(recorded::RecordedAcknowledgedConsumer::open(options), ErrorCode::OwnershipBusy);
  child.terminate();

  const auto stored = inspect_owned_progress(options);
  EXPECT_EQ(stored._snapshot._generation, previousGeneration + (stop._committed ? 1u : 0u));
  const bool issuedBefore = std::string{stop._operation} != "issue";
  const bool terminal = std::string{stop._operation} == "ack" && stop._committed;
  const bool pin = stop._committed ? std::string{stop._operation} == "issue" : issuedBefore;
  EXPECT_EQ(stored._snapshot._source._pinSequence, pin ? 1u : 0u);
  EXPECT_EQ(stored._snapshot._source._acknowledged, terminal ? 1u : 0u);
  EXPECT_EQ(stored._receipt._state, terminal ? recorded::ProgressReceiptState::Acknowledged :
      pin ? recorded::ProgressReceiptState::Issued : recorded::ProgressReceiptState::Pending);
  if (issuedBefore || stop._committed)
  {
    EXPECT_EQ(stored._receipt._dequeuedAt, recorded_test_time(-23));
    EXPECT_EQ(stored._receipt._consumerIncarnation, predecessorIncarnation);
  }
  else
  {
    EXPECT_FALSE(stored._receipt._dequeuedAt);
  }
  EXPECT_EQ(stored._receipt._acknowledgedAt,
            terminal ? std::optional{recorded_test_time(-17)} : std::nullopt);

  auto replacement = require_value(recorded::RecordedAcknowledgedConsumer::open(options));
  EXPECT_GT(require_value(replacement.claim())._incarnation, predecessorIncarnation);
  EXPECT_EQ(require_value(sender.acknowledged_sequence()), terminal ? 1u : 0u);
  // Selected issuance returns derived credit on recovery even if the dead
  // owner never reached its credit store. Old replay cannot return it twice.
  EXPECT_EQ(support::recorded_queue_credit(options), issuedBefore || stop._committed ? 1u : 0u);
  const auto next = require_delivery(replacement.try_receive(recorded_test_time(-31)));
  EXPECT_EQ(next._token._sequence, terminal ? 2u : 1u);
  EXPECT_EQ(next._message._header._publication, terminal ? secondId : firstId);
  EXPECT_EQ(std::get<std::uint64_t>(require_value(codec.decode(next._message._payload)).at(0)), terminal ? 84u : 42u);
  if (predecessor)
  {
    expect_error(replacement.acknowledge(*predecessor), ErrorCode::StaleDelivery);
    expect_error(replacement.release(*predecessor), ErrorCode::StaleDelivery);
  }
  require_value(replacement.acknowledge(next._token));
  if (!terminal)
  {
    const auto last = require_delivery(replacement.try_receive());
    EXPECT_EQ(last._token._sequence, 2u);
    require_value(replacement.acknowledge(last._token));
  }
  else
  {
    EXPECT_EQ(require_value(replacement.receipt(1))._acknowledgedAt, recorded_test_time(-17));
  }
  EXPECT_EQ(require_value(sender.acknowledged_sequence()), 2u);
  EXPECT_FALSE(require_value(replacement.try_receive()));
  EXPECT_TRUE(require_value(sender.lookup(firstId)));
}

INSTANTIATE_TEST_SUITE_P(RealProgressStops, RecordedAcknowledgedProgressDeath,
    testing::ValuesIn(progress_stops()), [](const testing::TestParamInfo<ProgressStop>& value)
    {
      return value.param._name;
    });

TEST_F(RecordedAcknowledgedQueueTest, RecoveryCanBeKilledAgainWithoutLosingTheOriginalAcknowledgmentTime)
{
  create();
  auto sender = require_value(recorded::RecordedProducer::open(_options));
  send(sender, 42);
  const auto secondId = send(sender, 84);
  support::NativeProcess committing{worker("ack", "selector")};
  committing.expect('O');
  const auto predecessor = committing.receive<std::uint64_t>();
  committing.expect('D');
  const auto oldToken = committing.receive<DeliveryToken>();
  committing.expect('P');
  (void)committing.receive<std::uint64_t>();
  committing.proceed();
  committing.expect('R');
  committing.terminate();
  // Reapplying absolute after-values must remain safe when each repair itself
  // dies. No repair samples a replacement timestamp or promotes an inactive
  // image; only the original selected acknowledgment is authoritative.
  for (const auto* point : {"partial-receipt", "partial-source", "before-applied"})
  {
    support::NativeProcess repair{worker("recover", point)};
    repair.expect('R');
    repair.terminate();
  }
  auto replacement = require_value(recorded::RecordedAcknowledgedConsumer::open(_options));
  EXPECT_GT(require_value(replacement.claim())._incarnation, predecessor);
  EXPECT_EQ(require_value(replacement.progress())._source._acknowledged, 1u);
  EXPECT_EQ(require_value(replacement.receipt(1))._acknowledgedAt, recorded_test_time(-17));
  EXPECT_EQ(require_value(sender.acknowledged_sequence()), 1u);
  const auto next = require_delivery(replacement.try_receive());
  EXPECT_EQ(next._message._header._publication, secondId);
  expect_error(replacement.acknowledge(oldToken), ErrorCode::StaleDelivery);
  require_value(replacement.acknowledge(next._token));
  EXPECT_FALSE(require_value(replacement.try_receive()));
}

TEST_F(RecordedAcknowledgedQueueTest, InterruptedReplacementReusesOnlyItsNeverExposedCandidateIncarnation)
{
  create();
  auto sender = require_value(recorded::RecordedProducer::open(_options));
  send(sender, 42);
  const auto pendingId = send(sender, 84);
  {
    auto first = require_value(recorded::RecordedAcknowledgedConsumer::open(_options));
    const auto delivery = require_delivery(first.try_receive(recorded_test_time(0)));
    require_value(first.acknowledge(delivery._token, recorded_test_time(-7)));
    require_value(first.close());
  }
  // Leave a committed issuance under a real ended owner, including the window
  // where its processing image is applied but its transport credit still lags.
  support::NativeProcess predecessor{worker("issue", "applied")};
  predecessor.expect('O');
  const auto predecessorIncarnation = predecessor.receive<std::uint64_t>();
  predecessor.expect('P');
  (void)predecessor.receive<std::uint64_t>();
  predecessor.proceed();
  predecessor.expect('R');
  EXPECT_EQ(support::recorded_queue_credit(_options), 1u);
  predecessor.terminate();
  const auto issued = inspect_owned_progress(_options, 2);
  ASSERT_EQ(issued._snapshot._source._pinSequence, 2u);
  const DeliveryToken oldToken{{_options._log._header._session, _options._consumer, predecessorIncarnation},
      _options._source, _options._queue, 2, issued._snapshot._source._pinIssuance};

  // Arming before open stops replacement attachment itself. Its local image
  // has committed the new actor and released the predecessor pin, but only
  // part of the Source working row has been installed and no claim was exposed.
  support::NativeProcess attachment{worker("attach", "partial-source")};
  attachment.expect('R');
  EXPECT_EQ(support::recorded_consumer_incarnation(_options), predecessorIncarnation);
  EXPECT_EQ(support::recorded_queue_credit(_options), 2u);
  EXPECT_EQ(require_value(sender.acknowledged_sequence()), 1u);
  send(sender, 126);
  send(sender, 168); // Recycles sequence 2's descriptor while attachment waits.
  const auto waiting = require_value(sender.next_publication_id());
  const auto bytes = support::recorded_queue_payload(_codec, 210);
  EXPECT_TRUE(std::holds_alternative<recorded::QueueCreditFull>(sender.try_send(
      waiting, support::recorded_queue_message(_options, bytes))));
  attachment.terminate();

  const auto repaired = inspect_owned_progress(_options, 2);
  EXPECT_EQ(repaired._snapshot._generation, issued._snapshot._generation + 1);
  EXPECT_EQ(repaired._snapshot._source._actorIncarnation, predecessorIncarnation + 1);
  EXPECT_EQ(repaired._snapshot._source._issuance, 0u);
  EXPECT_EQ(repaired._snapshot._source._pinSequence, 0u);
  EXPECT_EQ(repaired._snapshot._source._lastAcknowledgedAt, recorded_test_time(-7));
  EXPECT_EQ(repaired._receipt._state, recorded::ProgressReceiptState::Pending);
  EXPECT_EQ(repaired._receipt._consumerIncarnation, predecessorIncarnation);
  EXPECT_EQ(repaired._receipt._dequeuedAt, recorded_test_time(-23));
  EXPECT_EQ(support::recorded_consumer_incarnation(_options), predecessorIncarnation);

  auto replacement = require_value(recorded::RecordedAcknowledgedConsumer::open(_options));
  const auto current = require_value(replacement.claim());
  // This candidate was never returned by the killed open. Reusing it is safe;
  // issuing another attachment would incorrectly consume an extra generation.
  EXPECT_EQ(current._incarnation, predecessorIncarnation + 1);
  EXPECT_EQ(require_value(replacement.progress())._generation, repaired._snapshot._generation);
  EXPECT_EQ(require_value(replacement.receipt(1))._acknowledgedAt, recorded_test_time(-7));
  const auto next = require_delivery(replacement.try_receive(recorded_test_time(0)));
  EXPECT_EQ(next._message._header._publication, pendingId);
  EXPECT_EQ(next._token._consumer, current);
  EXPECT_EQ(next._token._sequence, 2u);
  EXPECT_NE(next._token, oldToken);
  expect_error(replacement.acknowledge(oldToken), ErrorCode::StaleDelivery);
  EXPECT_EQ(decoded(next._message), 84u);
  EXPECT_EQ(support::recorded_queue_credit(_options), 2u);
  EXPECT_TRUE(std::holds_alternative<recorded::QueueCreditFull>(sender.try_send(
      waiting, support::recorded_queue_message(_options, bytes))));
  require_value(replacement.acknowledge(next._token, recorded_test_time(0)));
  EXPECT_EQ(require_value(replacement.receipt(1))._acknowledgedAt, recorded_test_time(-7));
  for (std::uint64_t sequence = 3; sequence <= 4; ++sequence)
  {
    const auto delivery = require_delivery(replacement.try_receive());
    EXPECT_EQ(delivery._token._sequence, sequence);
    EXPECT_EQ(decoded(delivery._message), sequence * 42);
    require_value(replacement.acknowledge(delivery._token));
  }
  EXPECT_FALSE(require_value(replacement.try_receive()));
}

TEST_F(RecordedAcknowledgedQueueTest, CommittedIssuanceRepairsCreditAfterDeathBeforeTheHintStore)
{
  create();
  auto sender = require_value(recorded::RecordedProducer::open(_options));
  const auto firstId = send(sender);
  send(sender);
  support::NativeProcess child{worker("credit", "unused")};
  child.expect('O');
  (void)child.receive<std::uint64_t>();
  child.expect('P');
  (void)child.receive<std::uint64_t>();
  child.proceed();
  child.expect('R');
  EXPECT_EQ(support::recorded_queue_credit(_options), 0u);
  child.terminate();
  auto replacement = require_value(recorded::RecordedAcknowledgedConsumer::open(_options));
  EXPECT_EQ(support::recorded_queue_credit(_options), 1u);
  send(sender); // New acceptance can recycle the still-pending old descriptor.
  const auto again = require_delivery(replacement.try_receive());
  EXPECT_EQ(again._message._header._publication, firstId);
  EXPECT_EQ(support::recorded_queue_credit(_options), 1u);
  require_value(replacement.acknowledge(again._token));
}

TEST_F(RecordedAcknowledgedQueueTest, ACommittedAcknowledgmentCanHaveALaggingProducerHintUntilRecovery)
{
  create();
  auto sender = require_value(recorded::RecordedProducer::open(_options));
  send(sender);
  support::NativeProcess child{worker("ack-hint", "unused")};
  child.expect('O');
  (void)child.receive<std::uint64_t>();
  child.expect('D');
  (void)child.receive<DeliveryToken>();
  child.expect('P');
  (void)child.receive<std::uint64_t>();
  child.proceed();
  child.expect('R');
  EXPECT_EQ(require_value(sender.acknowledged_sequence()), 0u);
  child.terminate();
  auto replacement = require_value(recorded::RecordedAcknowledgedConsumer::open(_options));
  EXPECT_EQ(require_value(sender.acknowledged_sequence()), 1u);
  EXPECT_EQ(require_value(replacement.receipt(1))._acknowledgedAt, recorded_test_time(-17));
  EXPECT_FALSE(require_value(replacement.try_receive()));
}

namespace
{
  class RecordedAcknowledgedIndependenceTest : public testing::Test
  {
  protected:
    void SetUp() override
    {
      SessionOptions options;
      options._name = "AcknowledgedIndependence";
      options._directory = _directory.path() / "session";
      options._maxLogBytes = 1048576;
      options._limits = CapacityLimits::empty().tasks(8);
      TaskDescriptor creator;
      creator._name = "Administrator";
      auto created = require_value(Session::create(options, creator));
      _path = options._directory;
      _session = created._session.identity();
      _controller = require_value(mapped::SessionController::open_existing(_path));
    }

    recorded::RecordedQueueOptions options(unsigned connection) const
    {
      auto value = support::recorded_independence_options(_path, _session, connection);
      value._acknowledged = true;
      return value;
    }

    std::vector<std::string> worker(const char* mode, unsigned connection = 1) const
    {
      return {ANGARIA_RECORDED_ACKNOWLEDGED_QUEUE_WORKER, _path.string(), mode, std::to_string(connection)};
    }

    static void expect_complete(support::NativeProcess& child)
    {
      child.expect('A');
      EXPECT_EQ(child.receive<std::uint64_t>(), support::independence_message_count);
      child.wait_success();
    }

    support::ProducerLogDirectory _directory;
    std::filesystem::path _path;
    SessionId _session{};
    std::shared_ptr<mapped::SessionController> _controller;
  };
}

TEST_F(RecordedAcknowledgedIndependenceTest, ACommittedPausedPeerAcknowledgmentAndItsRecoveryDoNotGateOtherTraffic)
{
  const auto first = options(0);
  require_value(recorded::create_recorded_queue(first));
  require_value(recorded::create_recorded_queue(options(1)));
  auto source = require_value(recorded::RecordedProducer::open(first));
  const auto codec = support::recorded_queue_codec(first);
  const auto bytes = support::recorded_queue_payload(codec, 42);
  require_recorded_send(source.try_send(require_value(source.next_publication_id()),
      support::recorded_queue_message(first, bytes)));
  support::NativeProcess paused{worker("session-pause-ack", 0)};
  paused.expect('O');
  support::NativeProcess producer{worker("session-produce")};
  support::NativeProcess consumer{worker("session-consume")};
  producer.expect('O');
  consumer.expect('O');
  support::NativeProcess administrator{worker("session-gate")};
  administrator.expect('G');
  paused.proceed();
  paused.expect('R');
  expect_error(recorded::RecordedAcknowledgedConsumer::open(first), ErrorCode::OwnershipBusy);
  producer.proceed();
  consumer.proceed();
  expect_complete(producer);
  expect_complete(consumer);
  expect_error(_controller->try_acquire_mutation(), ErrorCode::OwnershipBusy);
  // Even repair of the unrelated partially installed local acknowledgment
  // does not need the still-held administrative gate.
  paused.terminate();
  auto replacement = require_value(recorded::RecordedAcknowledgedConsumer::open(first));
  EXPECT_EQ(require_value(source.acknowledged_sequence()), 1u);
  EXPECT_EQ(require_value(replacement.receipt(1))._acknowledgedAt, recorded_test_time(-17));
  EXPECT_FALSE(require_value(replacement.try_receive()));
  expect_error(_controller->try_acquire_mutation(), ErrorCode::OwnershipBusy);
  administrator.proceed();
  administrator.wait_success();
}
