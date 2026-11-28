// Public messaging examples: configure the complete graph, attach its Tasks,
// open endpoints, and handle explicit publication/processing results. Private
// infrastructure is used only to hold the actual administrative gate below.
#include "public_runtime_fixture.h"
#include "result_assertions.h"
#include "native_fixture.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <exception>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

#include <poll.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

namespace
{
  using namespace angaria;
  using angaria::test_support::expect_error;
  using angaria::test_support::has_error;
  using angaria::test_support::PublicRuntimeFixture;
  using angaria::test_support::public_message;
  using angaria::test_support::require_value;

  void expect_receive_error(const ReceiveResult& result, ErrorCode code)
  {
    ASSERT_TRUE(std::holds_alternative<Error>(result));
    EXPECT_EQ(std::get<Error>(result)._code, code) << std::get<Error>(result)._message;
  }

  void expect_accepted(const PublicationOutcome& result, PublicationId publication, QueueId queue)
  {
    EXPECT_EQ(result._publication, publication);
    EXPECT_EQ(result._status, AcceptanceStatus::Accepted);
    ASSERT_EQ(result._destinations.size(), 1u);
    EXPECT_EQ(result._destinations[0]._queue, queue);
    EXPECT_EQ(result._destinations[0]._status, DestinationStatus::Accepted);
    EXPECT_EQ(result._destinations[0]._reason, ErrorCode::None);
    ASSERT_TRUE(result._destinations[0]._queueSequence);
    EXPECT_GT(*result._destinations[0]._queueSequence, 0u);
  }

  TEST(PublicRuntimeAdmission, InvalidAndWrongOwnerOpeningsDoNotSealOrMutateSetup)
  {
    PublicRuntimeFixture fixture;
    auto& producer = fixture.created()._creator;
    auto consumer = require_value(fixture.created()._session.attach_task(fixture.consumer_ids()[0]));
    const auto before = angaria::test_support::recovered_image(fixture.directory());
    expect_error(producer.open_writer(QueueId::Invalid), ErrorCode::InvalidArgument);
    expect_error(producer.open_writer(QueueId{60000}), ErrorCode::NotFound);
    expect_error(producer.open_writer("MissingQueue"), ErrorCode::NotFound);
    expect_error(consumer.open_writer(fixture.queues()[0]), ErrorCode::WrongOwner);
    expect_error(consumer.open_writer("Queue0"), ErrorCode::WrongOwner);
    expect_error(consumer.open_source_set(SourceQueueSetId::Invalid), ErrorCode::InvalidArgument);
    expect_error(consumer.open_source_set(SourceQueueSetId{60000}), ErrorCode::NotFound);
    expect_error(consumer.open_source_set("MissingInput"), ErrorCode::NotFound);
    expect_error(producer.open_source_set(fixture.sources()[0]), ErrorCode::WrongOwner);
    expect_error(producer.open_source_set("Input0"), ErrorCode::WrongOwner);
    EXPECT_EQ(angaria::test_support::recovered_image(fixture.directory()), before);
    EXPECT_FALSE(std::filesystem::exists(fixture.directory() / "producer-1.messages.ang"));
    // Failed openings are validation errors, not an implicit transition from
    // setup to messaging. A legitimate later registration must still succeed.
    const auto added = require_value(producer.registry().register_task(TaskDescriptor{"StillConfiguring"}));
    EXPECT_NE(added, TaskId::Invalid);
    EXPECT_EQ(require_value(producer.registry().find_task("StillConfiguring")), added);
  }

  TEST(PublicRuntimeAdmission, PublicationStatusDuringSetupDoesNotSealOrIssueIds)
  {
    PublicRuntimeFixture fixture;
    auto& producer = fixture.created()._creator;
    const PublicationId notIssued{producer.claim(), 1};
    const auto before = angaria::test_support::recovered_image(fixture.directory());
    expect_error(producer.publication_status(notIssued), ErrorCode::InvalidState);
    std::array<DestinationOutcome, 1> destination;
    EXPECT_EQ(producer.publication_status(notIssued, destination)._error, ErrorCode::InvalidState);
    EXPECT_EQ(angaria::test_support::recovered_image(fixture.directory()), before);
    EXPECT_FALSE(std::filesystem::exists(fixture.directory() / "producer-1.messages.ang"));
    require_value(producer.registry().register_task(TaskDescriptor{"LaterSetup"}));
    // A read-only status request must not create an issuer, consume ID 1, or
    // cache partial admission state that breaks the eventual valid opening.
    auto writer = require_value(producer.open_writer(fixture.queues()[0]));
    const auto first = require_value(producer.next_publication_id());
    EXPECT_EQ(first, notIssued);
    const auto bytes = fixture.payload(3);
    expect_accepted(require_value(writer.send(first, fixture.message(bytes))), first, fixture.queues()[0]);
    EXPECT_EQ(require_value(producer.publication_status(first))._status, AcceptanceStatus::Accepted);
  }

  TEST(PublicRuntimeAdmission, DefaultAcknowledgmentKeepsAnExactPinUntilAcknowledgeOrRelease)
  {
    PublicRuntimeFixture fixture;
    auto& producer = fixture.created()._creator;
    auto writer = require_value(producer.open_writer("Queue0"));
    auto consumer = require_value(fixture.created()._session.attach_task("Consumer0"));
    auto source = require_value(consumer.open_source_set("Input0"));
    const auto bytes = fixture.payload(42);
    const auto publication = require_value(producer.next_publication_id());
    expect_accepted(require_value(writer.send(publication, fixture.message(bytes))), publication, fixture.queues()[0]);

    auto first = source.receive();
    ASSERT_TRUE(std::holds_alternative<Delivery>(first));
    const auto delivery = std::get<Delivery>(first);
    EXPECT_EQ(delivery._token._consumer, consumer.claim());
    EXPECT_EQ(delivery._message._header._schema, fixture.schemas()[0]);
    EXPECT_EQ(delivery._message._header._publication, publication);
    EXPECT_EQ(delivery._message._header._consumer, consumer.claim());
    EXPECT_EQ(delivery._message._header._queue, fixture.queues()[0]);
    EXPECT_EQ(delivery._message._header._queueSequence, 1u);
    EXPECT_EQ(delivery._message._header._flags, 0x80000001u);
    EXPECT_TRUE(delivery._message._header._enqueuedAt);
    EXPECT_TRUE(delivery._message._header._dequeuedAt);
    const auto repeat = source.receive();
    ASSERT_TRUE(std::holds_alternative<Delivery>(repeat));
    EXPECT_EQ(std::get<Delivery>(repeat)._token, delivery._token);
    EXPECT_EQ(std::get<Delivery>(repeat)._message._header, delivery._message._header);

    // Releasing is a request to process the same publication again, not an ACK.
    require_value(source.release(delivery._token));
    expect_error(source.acknowledge(delivery._token), ErrorCode::StaleDelivery);
    const auto redelivered = source.receive();
    ASSERT_TRUE(std::holds_alternative<Delivery>(redelivered));
    const auto next = std::get<Delivery>(redelivered);
    EXPECT_EQ(next._message._header._publication, publication);
    EXPECT_GT(next._token._issuance, delivery._token._issuance);
    require_value(source.acknowledge(next._token));
    expect_error(source.release(next._token), ErrorCode::StaleDelivery);
    EXPECT_TRUE(std::holds_alternative<ReceiveTimeout>(source.receive()));
  }

  TEST(PublicRuntimeAdmission, BothReceiveModesPreserveEveryObservedHeaderFieldInOwningCopies)
  {
    for (const auto mode : {AcknowledgmentMode::Required, AcknowledgmentMode::None})
    {
      SCOPED_TRACE(static_cast<unsigned>(mode));
      Message saved;
      MessageHeader observed;
      std::optional<BinaryPayloadCodec> retainedCodec;
      {
        PublicRuntimeFixture fixture({._mode = mode, ._ringCapacity = 1});
        auto& producer = fixture.created()._creator;
        auto writer = require_value(producer.open_writer(fixture.queues()[0]));
        auto consumer = require_value(fixture.created()._session.attach_task(fixture.consumer_ids()[0]));
        auto source = require_value(consumer.open_source_set(fixture.sources()[0]));
        const auto bytes = fixture.payload(12345);
        const auto publication = require_value(producer.next_publication_id());
        require_value(writer.send(publication, fixture.message(bytes)));
        const auto received = source.receive();
        EXPECT_EQ(std::holds_alternative<Delivery>(received), mode == AcknowledgmentMode::Required);
        EXPECT_EQ(std::holds_alternative<MessageView>(received), mode == AcknowledgmentMode::None);
        const auto view = public_message(received);
        observed = view._header;
        expect_error(view.copy_message(7), ErrorCode::CapacityExceeded);
        saved = require_value(view.copy_message(64));
        retainedCodec.emplace(fixture.codecs()[0]);
        if (const auto* delivery = std::get_if<Delivery>(&received))
        {
          require_value(source.acknowledge(delivery->_token));
        }
        // More accepted messages and credit reuse must not rewrite saved metadata.
        for (std::int64_t value = 1; value <= 16; ++value)
        {
          const auto nextBytes = fixture.payload(value);
          const auto nextId = require_value(producer.next_publication_id());
          require_value(writer.send(nextId, fixture.message(nextBytes)));
          const auto next = source.receive();
          ASSERT_FALSE(std::holds_alternative<Error>(next));
          if (const auto* delivery = std::get_if<Delivery>(&next))
          {
            require_value(source.acknowledge(delivery->_token));
          }
        }
        require_value(source.close());
      }
      // The Session, Tasks, mappings and files have all gone out of scope.
      EXPECT_EQ(saved._header, observed);
      EXPECT_EQ(saved.view()._header, observed);
      EXPECT_EQ(require_value(saved.decode(*retainedCodec)), (std::vector<PayloadValue>{std::int64_t{12345}}));
    }
  }

  TEST(PublicRuntimeAdmission, UnacknowledgedReceiveReturnsCreditWithoutCreatingATokenObligation)
  {
    PublicRuntimeFixture fixture({._mode = AcknowledgmentMode::None, ._ringCapacity = 1});
    auto& producer = fixture.created()._creator;
    auto writer = require_value(producer.open_writer(fixture.queues()[0]));
    auto consumer = require_value(fixture.created()._session.attach_task(fixture.consumer_ids()[0]));
    auto source = require_value(consumer.open_source_set(fixture.sources()[0]));
    const auto bytes = fixture.payload(7);
    for (std::uint64_t sequence = 1; sequence <= 3; ++sequence)
    {
      const auto id = require_value(producer.next_publication_id());
      expect_accepted(require_value(writer.send(id, fixture.message(bytes))), id, fixture.queues()[0]);
      const auto result = source.receive();
      ASSERT_TRUE(std::holds_alternative<MessageView>(result));
      EXPECT_EQ(std::get<MessageView>(result)._header._queueSequence, sequence);
    }
    const DeliveryToken invented{consumer.claim(), fixture.sources()[0], fixture.queues()[0], 1, 1};
    expect_error(source.acknowledge(invented), ErrorCode::UnsupportedPolicy);
    expect_error(source.release(invented), ErrorCode::UnsupportedPolicy);
    EXPECT_TRUE(std::holds_alternative<ReceiveTimeout>(source.receive()));
  }

  TEST(PublicRuntimeAdmission, OneTaskSharesItsIssuerAcrossWritersAndSelectsEveryExactSchema)
  {
    PublicRuntimeFixture fixture({._outputs = 2});
    auto& producer = fixture.created()._creator;
    auto firstWriter = require_value(producer.open_writer(fixture.queues()[0]));
    auto duplicateWriter = require_value(producer.open_writer(fixture.queues()[0]));
    auto secondWriter = require_value(producer.open_writer(fixture.queues()[1]));
    auto firstConsumer = require_value(fixture.created()._session.attach_task(fixture.consumer_ids()[0]));
    auto secondConsumer = require_value(fixture.created()._session.attach_task(fixture.consumer_ids()[1]));
    auto firstSource = require_value(firstConsumer.open_source_set(fixture.sources()[0]));
    auto secondSource = require_value(secondConsumer.open_source_set(fixture.sources()[1]));
    const auto bytes1 = fixture.payload(11);
    const auto bytes2 = fixture.payload(22, 1);
    const auto first = require_value(producer.next_publication_id());
    const auto second = require_value(producer.next_publication_id());
    const auto third = require_value(producer.next_publication_id());
    EXPECT_EQ(second._sequence, first._sequence + 1);
    EXPECT_EQ(third._sequence, second._sequence + 1);
    require_value(firstWriter.send(first, fixture.message(bytes1)));
    require_value(secondWriter.send(second, fixture.message(bytes2, 1)));
    require_value(duplicateWriter.send(third, fixture.message(bytes2, 1)));
    expect_accepted(require_value(producer.publication_status(second)), second, fixture.queues()[1]);

    const auto firstDelivery = std::get<Delivery>(firstSource.receive());
    EXPECT_EQ(firstDelivery._message._header._publication, first);
    EXPECT_EQ(firstDelivery._message._header._queueSequence, 1u);
    require_value(firstSource.acknowledge(firstDelivery._token));
    const auto thirdDelivery = std::get<Delivery>(firstSource.receive());
    EXPECT_EQ(thirdDelivery._message._header._publication, third);
    EXPECT_EQ(thirdDelivery._message._header._schema, fixture.schemas()[1]);
    EXPECT_EQ(thirdDelivery._message._header._payloadBytes, 16u);
    EXPECT_EQ(thirdDelivery._message._header._queueSequence, 2u);
    EXPECT_EQ(require_value(require_value(thirdDelivery._message.copy_message(64)).decode(fixture.codecs()[1])),
              (std::vector<PayloadValue>{std::int64_t{22}, std::int64_t{23}}));
    require_value(firstSource.acknowledge(thirdDelivery._token));
    const auto secondDelivery = std::get<Delivery>(secondSource.receive());
    EXPECT_EQ(secondDelivery._message._header._publication, second);
    EXPECT_EQ(secondDelivery._message._header._queueSequence, 1u);
    require_value(secondSource.acknowledge(secondDelivery._token));
  }

  TEST(PublicRuntimeAdmission, SharedLogDiscoveryAdvancesPastOtherQueuesInBoundedBatches)
  {
    PublicRuntimeFixture fixture({._mode = AcknowledgmentMode::None, ._outputs = 2, ._ringCapacity = 64});
    auto& producer = fixture.created()._creator;
    auto writer0 = require_value(producer.open_writer(fixture.queues()[0]));
    auto writer1 = require_value(producer.open_writer(fixture.queues()[1]));
    auto consumer = require_value(fixture.created()._session.attach_task(fixture.consumer_ids()[1]));
    auto source = require_value(consumer.open_source_set(fixture.sources()[1]));
    const auto bytes = fixture.payload(17);
    for (unsigned index = 0; index < 40; ++index)
    {
      require_value(writer0.send(require_value(producer.next_publication_id()), fixture.message(bytes)));
    }
    const auto wanted = require_value(producer.next_publication_id());
    require_value(writer1.send(wanted, fixture.message(bytes)));
    // A bounded nonblocking poll may first scan records for Queue0. A timed
    // receive continues that cursor until it reaches this Queue's message.
    const auto result = source.receive(std::chrono::seconds{1});
    ASSERT_TRUE(std::holds_alternative<MessageView>(result));
    EXPECT_EQ(std::get<MessageView>(result)._header._publication, wanted);
    EXPECT_EQ(std::get<MessageView>(result)._header._queueSequence, 1u);
  }

  TEST(PublicRuntimeAdmission, FullQueueIsKnownUnacceptedAndAnAcceptedRetryIsIdempotent)
  {
    PublicRuntimeFixture fixture({._mode = AcknowledgmentMode::None, ._ringCapacity = 1});
    auto& producer = fixture.created()._creator;
    auto writer = require_value(producer.open_writer(fixture.queues()[0]));
    auto consumer = require_value(fixture.created()._session.attach_task(fixture.consumer_ids()[0]));
    auto source = require_value(consumer.open_source_set(fixture.sources()[0]));
    const auto bytes = fixture.payload(8);
    const auto first = require_value(producer.next_publication_id());
    require_value(writer.send(first, fixture.message(bytes)));
    const auto pending = require_value(producer.next_publication_id());
    const auto rejected = require_value(writer.send(pending, fixture.message(bytes)));
    ASSERT_EQ(rejected._destinations.size(), 1u);
    EXPECT_EQ(rejected._status, AcceptanceStatus::Rejected);
    EXPECT_EQ(rejected._destinations[0]._reason, ErrorCode::QueueFull);
    EXPECT_FALSE(rejected._destinations[0]._queueSequence);
    const auto absent = require_value(producer.publication_status(pending));
    EXPECT_EQ(absent._status, AcceptanceStatus::Rejected);
    EXPECT_TRUE(absent._destinations.empty());
    expect_accepted(require_value(writer.send(first, fixture.message(bytes))), first, fixture.queues()[0]);
    const auto changed = fixture.payload(9);
    expect_error(writer.send(first, fixture.message(changed)), ErrorCode::Conflict);
    EXPECT_EQ(public_message(source.receive())._header._publication, first);
    // No later ID was accepted, so this still-issued, unaccepted ID can retry.
    expect_accepted(require_value(writer.send(pending, fixture.message(bytes))), pending, fixture.queues()[0]);
    EXPECT_EQ(public_message(source.receive())._header._publication, pending);
    EXPECT_TRUE(std::holds_alternative<ReceiveTimeout>(source.receive()));
  }

  TEST(PublicRuntimeAdmission, LaterTaskAcceptanceRetiresAnEarlierUnacceptedIdAcrossQueues)
  {
    PublicRuntimeFixture fixture({._mode = AcknowledgmentMode::None, ._outputs = 2, ._ringCapacity = 1});
    auto& producer = fixture.created()._creator;
    auto writer0 = require_value(producer.open_writer(fixture.queues()[0]));
    auto writer1 = require_value(producer.open_writer(fixture.queues()[1]));
    auto consumer0 = require_value(fixture.created()._session.attach_task(fixture.consumer_ids()[0]));
    auto source0 = require_value(consumer0.open_source_set(fixture.sources()[0]));
    const auto bytes = fixture.payload(1);
    require_value(writer0.send(require_value(producer.next_publication_id()), fixture.message(bytes)));
    const auto blocked = require_value(producer.next_publication_id());
    EXPECT_EQ(require_value(writer0.send(blocked, fixture.message(bytes)))._status, AcceptanceStatus::Rejected);
    const auto later = require_value(producer.next_publication_id());
    expect_accepted(require_value(writer1.send(later, fixture.message(bytes))), later, fixture.queues()[1]);
    (void)public_message(source0.receive());
    expect_error(writer0.send(blocked, fixture.message(bytes)), ErrorCode::InvalidArgument);
    const auto replacement = require_value(producer.next_publication_id());
    expect_accepted(require_value(writer0.send(replacement, fixture.message(bytes))), replacement, fixture.queues()[0]);
  }

  TEST(PublicRuntimeAdmission, AcceptedSequencesAndCommittedAcknowledgmentWatermarksArePerQueue)
  {
    PublicRuntimeFixture fixture({._outputs = 2, ._ringCapacity = 1});
    auto& producer = fixture.created()._creator;
    auto writer0 = require_value(producer.open_writer(fixture.queues()[0]));
    auto writer1 = require_value(producer.open_writer(fixture.queues()[1]));
    auto consumer0 = require_value(fixture.created()._session.attach_task(fixture.consumer_ids()[0]));
    auto consumer1 = require_value(fixture.created()._session.attach_task(fixture.consumer_ids()[1]));
    auto source0 = require_value(consumer0.open_source_set(fixture.sources()[0]));
    auto source1 = require_value(consumer1.open_source_set(fixture.sources()[1]));
    const auto bytes = fixture.payload(91);
    EXPECT_EQ(require_value(writer0.acknowledged_sequence()), 0u);
    EXPECT_EQ(require_value(writer1.acknowledged_sequence()), 0u);
    const auto first = require_value(producer.next_publication_id());
    const auto accepted = require_value(writer0.send(first, fixture.message(bytes)));
    ASSERT_EQ(accepted._destinations.size(), 1u);
    EXPECT_EQ(accepted._destinations[0]._queueSequence, 1u);
    const auto status = require_value(producer.publication_status(first));
    ASSERT_EQ(status._destinations.size(), 1u);
    EXPECT_EQ(status._destinations[0]._queueSequence, accepted._destinations[0]._queueSequence);
    std::array<DestinationOutcome, 1> destination;
    EXPECT_EQ(producer.publication_status(first, destination)._status, PublicationStatus::Accepted);
    EXPECT_EQ(destination[0]._queueSequence, 1u);
    EXPECT_EQ(writer0.send(first, fixture.message(bytes), destination)._status, PublicationStatus::Accepted);
    EXPECT_EQ(destination[0]._queueSequence, 1u);

    const auto pin = std::get<Delivery>(source0.receive());
    EXPECT_EQ(pin._message._header._queueSequence, accepted._destinations[0]._queueSequence);
    EXPECT_EQ(require_value(writer0.acknowledged_sequence()), 0u);
    // Receive returned K=1 credit, allowing a second acceptance. It did not
    // certify processing of either publication or advance the ACK lower bound.
    const auto second = require_value(producer.next_publication_id());
    EXPECT_EQ(require_value(writer0.send(second, fixture.message(bytes)))._destinations[0]._queueSequence, 2u);
    EXPECT_EQ(require_value(writer0.acknowledged_sequence()), 0u);
    const auto other = require_value(producer.next_publication_id());
    const auto otherAccepted = require_value(writer1.send(other, fixture.message(bytes)));
    ASSERT_EQ(otherAccepted._destinations.size(), 1u);
    EXPECT_EQ(otherAccepted._destinations[0]._queueSequence, 1u);
    const auto otherPin = std::get<Delivery>(source1.receive());
    EXPECT_EQ(require_value(writer1.acknowledged_sequence()), 0u);

    require_value(source0.release(pin._token));
    expect_error(source0.acknowledge(pin._token), ErrorCode::StaleDelivery);
    EXPECT_EQ(require_value(writer0.acknowledged_sequence()), 0u);
    const auto repeated = std::get<Delivery>(source0.receive());
    EXPECT_EQ(repeated._message._header._publication, first);
    EXPECT_GT(repeated._token._issuance, pin._token._issuance);
    EXPECT_EQ(require_value(writer0.acknowledged_sequence()), 0u);
    // Both writers share one Task/log, while each Queue has its own processing
    // watermark. Applications choose when to observe this committed lower bound;
    // it is not a built-in producer round trip for every publication. An
    // interrupted consumer may leave a lagging hint; no interruption occurs here.
    require_value(source1.acknowledge(otherPin._token));
    EXPECT_EQ(require_value(writer1.acknowledged_sequence()), 1u);
    EXPECT_EQ(require_value(writer0.acknowledged_sequence()), 0u);
    require_value(source0.acknowledge(repeated._token));
    EXPECT_EQ(require_value(writer0.acknowledged_sequence()), 1u);
    EXPECT_EQ(require_value(writer1.acknowledged_sequence()), 1u);
    const auto secondPin = std::get<Delivery>(source0.receive());
    EXPECT_EQ(secondPin._message._header._publication, second);
    EXPECT_EQ(require_value(writer0.acknowledged_sequence()), 1u);
    require_value(source0.close());
    EXPECT_EQ(require_value(writer0.acknowledged_sequence()), 1u);
  }

  TEST(PublicRuntimeAdmission, UnacknowledgedAcceptanceHasAQueueSequenceButNoProcessingWatermark)
  {
    PublicRuntimeFixture fixture({._mode = AcknowledgmentMode::None, ._ringCapacity = 1});
    auto& producer = fixture.created()._creator;
    auto writer = require_value(producer.open_writer(fixture.queues()[0]));
    auto consumer = require_value(fixture.created()._session.attach_task(fixture.consumer_ids()[0]));
    auto source = require_value(consumer.open_source_set(fixture.sources()[0]));
    const auto bytes = fixture.payload(97);
    expect_error(writer.acknowledged_sequence(), ErrorCode::UnsupportedPolicy);
    const auto first = require_value(producer.next_publication_id());
    const auto accepted = require_value(writer.send(first, fixture.message(bytes)));
    ASSERT_EQ(accepted._destinations.size(), 1u);
    EXPECT_EQ(accepted._destinations[0]._queueSequence, 1u);
    const auto status = require_value(producer.publication_status(first));
    ASSERT_EQ(status._destinations.size(), 1u);
    EXPECT_EQ(status._destinations[0]._queueSequence, 1u);
    const auto blocked = require_value(producer.next_publication_id());
    std::array<DestinationOutcome, 1> destination;
    destination[0]._queueSequence = 999;
    const auto rejected = writer.send(blocked, fixture.message(bytes), destination);
    EXPECT_EQ(rejected._status, PublicationStatus::Rejected);
    EXPECT_EQ(rejected._error, ErrorCode::None);
    EXPECT_EQ(destination[0]._reason, ErrorCode::QueueFull);
    EXPECT_FALSE(destination[0]._queueSequence);
    EXPECT_EQ(public_message(source.receive())._header._queueSequence, 1u);
    expect_error(writer.acknowledged_sequence(), ErrorCode::UnsupportedPolicy);
    const auto next = require_value(writer.send(blocked, fixture.message(bytes)));
    ASSERT_EQ(next._destinations.size(), 1u);
    EXPECT_EQ(next._destinations[0]._queueSequence, 2u);
    EXPECT_EQ(public_message(source.receive())._header._queueSequence, 2u);
    expect_error(writer.acknowledged_sequence(), ErrorCode::UnsupportedPolicy);
  }

  TEST(PublicRuntimeAdmission, ReturningTransportCreditDoesNotAcknowledgeProcessingAndLogFullDoesNotBlockAck)
  {
    PublicRuntimeFixture fixture({._ringCapacity = 1, ._publications = 2});
    auto& producer = fixture.created()._creator;
    auto writer = require_value(producer.open_writer(fixture.queues()[0]));
    auto consumer = require_value(fixture.created()._session.attach_task(fixture.consumer_ids()[0]));
    auto source = require_value(consumer.open_source_set(fixture.sources()[0]));
    const auto bytes = fixture.payload(1);
    const auto first = require_value(producer.next_publication_id());
    require_value(writer.send(first, fixture.message(bytes)));
    const auto pin = std::get<Delivery>(source.receive());
    const auto second = require_value(producer.next_publication_id());
    require_value(writer.send(second, fixture.message(bytes)));
    EXPECT_EQ(std::get<Delivery>(source.receive())._token, pin._token);
    require_value(source.acknowledge(pin._token));
    const auto secondPin = std::get<Delivery>(source.receive());
    EXPECT_EQ(secondPin._message._header._publication, second);
    const auto extra = require_value(producer.next_publication_id());
    expect_error(writer.send(extra, fixture.message(bytes)), ErrorCode::LogFull);
    require_value(source.acknowledge(secondPin._token));
    EXPECT_TRUE(std::holds_alternative<ReceiveTimeout>(source.receive()));
    expect_accepted(require_value(producer.publication_status(first)), first, fixture.queues()[0]);
  }

  TEST(PublicRuntimeAdmission, CloseReopenRedeliversAnUnacknowledgedPinWithANewToken)
  {
    PublicRuntimeFixture fixture;
    auto& producer = fixture.created()._creator;
    auto writer = require_value(producer.open_writer(fixture.queues()[0]));
    auto consumer = require_value(fixture.created()._session.attach_task(fixture.consumer_ids()[0]));
    auto source = require_value(consumer.open_source_set(fixture.sources()[0]));
    expect_error(consumer.open_source_set(fixture.sources()[0]), ErrorCode::OwnershipBusy);
    const auto bytes = fixture.payload(51);
    const auto publication = require_value(producer.next_publication_id());
    require_value(writer.send(publication, fixture.message(bytes)));
    const auto original = std::get<Delivery>(source.receive());
    require_value(source.close());
    require_value(source.close());
    expect_receive_error(source.receive(), ErrorCode::InvalidState);
    auto reopened = require_value(consumer.open_source_set(fixture.sources()[0]));
    const auto current = std::get<Delivery>(reopened.receive());
    EXPECT_EQ(current._message._header._publication, publication);
    EXPECT_GT(current._token._issuance, original._token._issuance);
    expect_error(reopened.acknowledge(original._token), ErrorCode::StaleDelivery);
    require_value(reopened.acknowledge(current._token));
    EXPECT_TRUE(std::holds_alternative<ReceiveTimeout>(reopened.receive()));
  }

  TEST(PublicRuntimeAdmission, UnacknowledgedCloseReopenMayDeliverAgainWithoutInventingAnAck)
  {
    PublicRuntimeFixture fixture({._mode = AcknowledgmentMode::None});
    auto& producer = fixture.created()._creator;
    auto writer = require_value(producer.open_writer(fixture.queues()[0]));
    auto consumer = require_value(fixture.created()._session.attach_task(fixture.consumer_ids()[0]));
    auto source = require_value(consumer.open_source_set(fixture.sources()[0]));
    const auto bytes = fixture.payload(19);
    const auto id = require_value(producer.next_publication_id());
    require_value(writer.send(id, fixture.message(bytes)));
    EXPECT_EQ(public_message(source.receive())._header._publication, id);
    require_value(source.close());
    auto replacement = require_value(consumer.open_source_set(fixture.sources()[0]));
    EXPECT_EQ(public_message(replacement.receive())._header._publication, id);
    EXPECT_TRUE(std::holds_alternative<ReceiveTimeout>(replacement.receive()));
  }

  TEST(PublicRuntimeAdmission, EmptyAndInvalidReceiveArgumentsAreExplicit)
  {
    PublicRuntimeFixture fixture;
    auto consumer = require_value(fixture.created()._session.attach_task(fixture.consumer_ids()[0]));
    auto source = require_value(consumer.open_source_set(fixture.sources()[0]));
    EXPECT_TRUE(std::holds_alternative<ReceiveTimeout>(source.receive()));
    EXPECT_TRUE(std::holds_alternative<ReceiveTimeout>(source.receive(std::chrono::milliseconds{1})));
    expect_receive_error(source.receive(std::chrono::nanoseconds{-1}), ErrorCode::InvalidArgument);
  }

  TEST(PublicRuntimeAdmission, TooSmallOutcomeStorageCannotAccidentallyAcceptAMessage)
  {
    PublicRuntimeFixture fixture;
    auto& producer = fixture.created()._creator;
    auto writer = require_value(producer.open_writer(fixture.queues()[0]));
    const auto bytes = fixture.payload(7);
    const auto id = require_value(producer.next_publication_id());
    const auto sized = writer.send(id, fixture.message(bytes), {});
    EXPECT_EQ(sized._error, ErrorCode::OutputBufferTooSmall);
    EXPECT_EQ(sized._requiredCapacity, 1u);
    EXPECT_EQ(require_value(producer.publication_status(id))._status, AcceptanceStatus::Rejected);
    std::array<DestinationOutcome, 1> destination;
    EXPECT_EQ(writer.send(id, fixture.message(bytes), destination)._status, PublicationStatus::Accepted);
    EXPECT_EQ(producer.publication_status(id, {})._error, ErrorCode::OutputBufferTooSmall);
  }

  TEST(PublicRuntimeAdmission, EstablishedMessagingContinuesWhileTheActualAdministrativeGateIsHeld)
  {
    for (const auto mode : {AcknowledgmentMode::Required, AcknowledgmentMode::None})
    {
      SCOPED_TRACE(static_cast<unsigned>(mode));
      PublicRuntimeFixture fixture({._mode = mode});
      auto& producer = fixture.created()._creator;
      auto writer = require_value(producer.open_writer(fixture.queues()[0]));
      auto consumer = require_value(fixture.created()._session.attach_task(fixture.consumer_ids()[0]));
      auto source = require_value(consumer.open_source_set(fixture.sources()[0]));
      const auto bytes = fixture.payload(5);
      auto state = angaria::test_support::open_runtime_state(fixture.directory());
      // This is deliberately a private positive-control probe. All endpoints
      // are already established before taking the real Session mutation gate.
      angaria::detail::RuntimeAccess administrator(state);
      expect_error(state->_controller->try_acquire_mutation(), ErrorCode::OwnershipBusy);
      const auto adminWords = administrator.read_words(0, 24);
      const auto id = require_value(producer.next_publication_id());
      std::array<DestinationOutcome, 1> destination;
      EXPECT_EQ(writer.send(id, fixture.message(bytes), destination)._status, PublicationStatus::Accepted);
      EXPECT_EQ(producer.publication_status(id, destination)._status, PublicationStatus::Accepted);
      const auto received = source.receive();
      EXPECT_EQ(public_message(received)._header._publication, id);
      if (const auto* delivery = std::get_if<Delivery>(&received))
      {
        EXPECT_EQ(require_value(writer.acknowledged_sequence()), 0u);
        require_value(source.acknowledge(delivery->_token));
        EXPECT_EQ(require_value(writer.acknowledged_sequence()), 1u);
      }
      else
      {
        expect_error(writer.acknowledged_sequence(), ErrorCode::UnsupportedPolicy);
      }
      EXPECT_EQ(writer.id(), fixture.queues()[0]);
      EXPECT_EQ(source.id(), fixture.sources()[0]);
      EXPECT_EQ(administrator.read_words(0, 24), adminWords);
      expect_error(state->_controller->try_acquire_mutation(), ErrorCode::OwnershipBusy);
    }
  }

  TEST(PublicRuntimeAdmission, ProcessEndpointsSupportCoordinatedUseFromAnotherThread)
  {
    PublicRuntimeFixture fixture;
    auto& producer = fixture.created()._creator;
    auto writer = require_value(producer.open_writer(fixture.queues()[0]));
    auto consumer = require_value(fixture.created()._session.attach_task(fixture.consumer_ids()[0]));
    auto source = require_value(consumer.open_source_set(fixture.sources()[0]));
    const auto bytes = fixture.payload(24);
    std::optional<PublicationOutcome> accepted;
    std::exception_ptr failure;
    std::thread sender([&]
    {
      try
      {
        accepted.emplace(require_value(writer.send(require_value(producer.next_publication_id()), fixture.message(bytes))));
      }
      catch (...)
      {
        failure = std::current_exception();
      }
    });
    sender.join();
    if (failure)
    {
      std::rethrow_exception(failure);
    }
    ASSERT_TRUE(accepted);
    std::thread receiver([&]
    {
      try
      {
        const auto delivery = std::get<Delivery>(source.receive());
        if (delivery._message._header._publication != accepted->_publication)
        {
          throw std::runtime_error("Cross-thread delivery differs from accepted publication");
        }
        require_value(source.acknowledge(delivery._token));
      }
      catch (...)
      {
        failure = std::current_exception();
      }
    });
    receiver.join();
    if (failure)
    {
      std::rethrow_exception(failure);
    }
    EXPECT_TRUE(std::holds_alternative<ReceiveTimeout>(source.receive()));
  }

  TEST(PublicRuntimeAdmission, ThreadProducerRejectsAnotherThreadAndItsWriterRetainsTheParentClaim)
  {
    PublicRuntimeFixture fixture({._threadProducer = true});
    std::optional<Task> task(require_value(fixture.created()._session.attach_task(fixture.producer_id())));
    std::optional<QueueWriter> writer(require_value(task->open_writer(fixture.queues()[0])));
    const auto bytes = fixture.payload(31);
    const auto id = require_value(task->next_publication_id());
    std::optional<Result<PublicationId>> wrongIssuer;
    std::optional<Result<PublicationOutcome>> wrongSend;
    std::thread other([&]
    {
      wrongIssuer.emplace(task->next_publication_id());
      wrongSend.emplace(writer->send(id, fixture.message(bytes)));
    });
    other.join();
    ASSERT_TRUE(wrongIssuer);
    ASSERT_TRUE(wrongSend);
    expect_error(*wrongIssuer, ErrorCode::WrongOwner);
    expect_error(*wrongSend, ErrorCode::WrongOwner);
    expect_accepted(require_value(writer->send(id, fixture.message(bytes))), id, fixture.queues()[0]);
    task.reset();
    {
      auto closingParent = std::move(fixture.created()._creator);
    }
    expect_error(fixture.created()._session.attach_task(TaskId{1}), ErrorCode::OwnershipBusy);
    writer.reset();
    const auto parent = require_value(fixture.created()._session.attach_task(TaskId{1}));
    EXPECT_EQ(parent.claim()._incarnation, 2u);
  }

  TEST(PublicRuntimeAdmission, ThreadConsumerChecksAffinityAndClosingTheSourceReleasesItsParent)
  {
    PublicRuntimeFixture fixture({._threadConsumer = true});
    std::optional<QueueWriter> writer(require_value(fixture.created()._creator.open_writer(fixture.queues()[0])));
    std::optional<Task> consumer(require_value(fixture.created()._session.attach_task(fixture.consumer_ids()[0])));
    auto source = require_value(consumer->open_source_set(fixture.sources()[0]));
    std::optional<ReceiveResult> wrongReceive;
    std::thread other([&]
    {
      wrongReceive.emplace(source.receive());
    });
    other.join();
    ASSERT_TRUE(wrongReceive);
    expect_receive_error(*wrongReceive, ErrorCode::WrongOwner);
    const auto bytes = fixture.payload(43);
    require_value(writer->send(require_value(fixture.created()._creator.next_publication_id()), fixture.message(bytes)));
    const auto delivery = std::get<Delivery>(source.receive());
    require_value(source.acknowledge(delivery._token));
    consumer.reset();
    writer.reset();
    {
      auto closingParent = std::move(fixture.created()._creator);
    }
    expect_error(fixture.created()._session.attach_task(TaskId{1}), ErrorCode::OwnershipBusy);
    require_value(source.close());
    const auto parent = require_value(fixture.created()._session.attach_task(TaskId{1}));
    EXPECT_EQ(parent.claim()._incarnation, 2u);
  }

  TEST(PublicRuntimeAdmission, ForeignThreadSourceDestructionDoesNotLeaveAStaleLocalReservation)
  {
    PublicRuntimeFixture fixture({._threadConsumer = true});
    auto& producer = fixture.created()._creator;
    auto writer = require_value(producer.open_writer(fixture.queues()[0]));
    auto consumer = require_value(fixture.created()._session.attach_task(fixture.consumer_ids()[0]));
    auto source = require_value(consumer.open_source_set(fixture.sources()[0]));
    const auto bytes = fixture.payload(47);
    const auto publication = require_value(producer.next_publication_id());
    require_value(writer.send(publication, fixture.message(bytes)));
    const auto original = std::get<Delivery>(source.receive());

    // A Thread Source still rejects explicit use by another thread. Rejection
    // must leave the owner's exact pin unchanged and available for processing.
    std::optional<OperationResult> wrongClose;
    std::thread other([&]
    {
      wrongClose.emplace(source.close());
    });
    other.join();
    ASSERT_TRUE(wrongClose);
    expect_error(*wrongClose, ErrorCode::WrongOwner);
    EXPECT_EQ(std::get<Delivery>(source.receive())._token, original._token);

    // Last-handle cleanup may run outside the owner thread. Once that handle
    // is gone, a stale local reservation must not prevent the still-live Task
    // from opening a replacement on its proper thread. Cleanup is not an ACK.
    std::thread disposal([closing = std::optional<SourceQueueSet>{std::move(source)}]() mutable
    {
      closing.reset();
    });
    disposal.join();
    EXPECT_EQ(require_value(writer.acknowledged_sequence()), 0u);
    auto reopened = require_value(consumer.open_source_set(fixture.sources()[0]));
    expect_error(reopened.acknowledge(original._token), ErrorCode::StaleDelivery);
    const auto redelivered = std::get<Delivery>(reopened.receive());
    EXPECT_EQ(redelivered._message._header._publication, publication);
    EXPECT_NE(redelivered._token, original._token);
    require_value(reopened.acknowledge(redelivered._token));
    EXPECT_EQ(require_value(writer.acknowledged_sequence()), 1u);
  }

  TEST(PublicRuntimeAdmission, ForkChildrenCannotUseInheritedMessagingHandlesOrProcessingTokens)
  {
    PublicRuntimeFixture fixture;
    auto& producer = fixture.created()._creator;
    auto writer = require_value(producer.open_writer(fixture.queues()[0]));
    auto consumer = require_value(fixture.created()._session.attach_task(fixture.consumer_ids()[0]));
    auto source = require_value(consumer.open_source_set(fixture.sources()[0]));
    const auto bytes = fixture.payload(61);
    const auto id = require_value(producer.next_publication_id());
    require_value(writer.send(id, fixture.message(bytes)));
    const auto delivery = std::get<Delivery>(source.receive());
    const auto child = ::fork();
    ASSERT_GE(child, 0);
    if (child == 0)
    {
      // _exit skips inherited C++ destructors; the parent still owns these
      // native claims and mappings. Wrong-owner checks precede local locks.
      const auto received = source.receive();
      const auto* error = std::get_if<Error>(&received);
      const bool valid = has_error(producer.next_publication_id(), ErrorCode::WrongOwner) &&
          has_error(writer.send(id, fixture.message(bytes)), ErrorCode::WrongOwner) &&
          has_error(producer.publication_status(id), ErrorCode::WrongOwner) &&
          error && error->_code == ErrorCode::WrongOwner &&
          has_error(source.acknowledge(delivery._token), ErrorCode::WrongOwner) &&
          has_error(source.release(delivery._token), ErrorCode::WrongOwner) &&
          has_error(source.close(), ErrorCode::WrongOwner);
      ::_exit(valid ? 0 : 1);
    }
    int status{};
    ASSERT_EQ(::waitpid(child, &status, 0), child);
    ASSERT_TRUE(WIFEXITED(status));
    EXPECT_EQ(WEXITSTATUS(status), 0);
    require_value(source.acknowledge(delivery._token));
    EXPECT_TRUE(std::holds_alternative<ReceiveTimeout>(source.receive()));
  }

  TEST(PublicRuntimeAdmission, ProducerReplacementFindsEarlierAcceptanceAndStartsItsOwnIdSequence)
  {
    PublicRuntimeFixture fixture;
    std::optional<QueueWriter> writer(require_value(fixture.created()._creator.open_writer(fixture.queues()[0])));
    auto consumer = require_value(fixture.created()._session.attach_task(fixture.consumer_ids()[0]));
    auto source = require_value(consumer.open_source_set(fixture.sources()[0]));
    const auto bytes = fixture.payload(81);
    const auto oldId = require_value(fixture.created()._creator.next_publication_id());
    require_value(writer->send(oldId, fixture.message(bytes)));
    writer.reset();
    {
      auto closing = std::move(fixture.created()._creator);
    }
    auto replacement = require_value(fixture.created()._session.attach_task(TaskId{1}));
    EXPECT_GT(replacement.claim()._incarnation, oldId._producer._incarnation);
    expect_accepted(require_value(replacement.publication_status(oldId)), oldId, fixture.queues()[0]);
    const auto newId = require_value(replacement.next_publication_id());
    EXPECT_EQ(newId._sequence, 1u);
    EXPECT_NE(newId, oldId);
    auto nextWriter = require_value(replacement.open_writer(fixture.queues()[0]));
    require_value(nextWriter.send(newId, fixture.message(bytes)));
    const auto oldDelivery = std::get<Delivery>(source.receive());
    EXPECT_EQ(oldDelivery._message._header._publication, oldId);
    require_value(source.acknowledge(oldDelivery._token));
    const auto newDelivery = std::get<Delivery>(source.receive());
    EXPECT_EQ(newDelivery._message._header._publication, newId);
    EXPECT_EQ(newDelivery._message._header._queueSequence, 2u);
    require_value(source.acknowledge(newDelivery._token));
  }

  TEST(PublicRuntimeAdmission, ReplacingTheConsumerTaskChangesItsAuthorityAndPreservesPendingMessages)
  {
    PublicRuntimeFixture fixture;
    auto& producer = fixture.created()._creator;
    auto writer = require_value(producer.open_writer(fixture.queues()[0]));
    const auto bytes = fixture.payload(83);
    const auto publication = require_value(producer.next_publication_id());
    require_value(writer.send(publication, fixture.message(bytes)));
    DeliveryToken oldToken;
    {
      auto consumer = require_value(fixture.created()._session.attach_task(fixture.consumer_ids()[0]));
      auto source = require_value(consumer.open_source_set(fixture.sources()[0]));
      oldToken = std::get<Delivery>(source.receive())._token;
      // Closing without an ACK releases the pin; it does not claim processing.
      require_value(source.close());
    }
    auto replacement = require_value(fixture.created()._session.attach_task(fixture.consumer_ids()[0]));
    auto source = require_value(replacement.open_source_set(fixture.sources()[0]));
    const auto redelivery = std::get<Delivery>(source.receive());
    EXPECT_EQ(redelivery._message._header._publication, publication);
    EXPECT_GT(redelivery._token._consumer._incarnation, oldToken._consumer._incarnation);
    expect_error(source.acknowledge(oldToken), ErrorCode::StaleDelivery);
    require_value(source.acknowledge(redelivery._token));
    EXPECT_TRUE(std::holds_alternative<ReceiveTimeout>(source.receive()));
  }

  TEST(PublicRuntimeAdmission, TerminatedPublicConsumerLeavesItsUnacknowledgedMessageForReplacement)
  {
    PublicRuntimeFixture fixture;
    auto& producer = fixture.created()._creator;
    auto writer = require_value(producer.open_writer(fixture.queues()[0]));
    const auto bytes = fixture.payload(89);
    const auto publication = require_value(producer.next_publication_id());
    require_value(writer.send(publication, fixture.message(bytes)));
    int channel[2]{};
    ASSERT_EQ(::pipe(channel), 0);
    const auto directory = fixture.directory().string();
    const auto taskArgument = std::to_string(static_cast<std::uint16_t>(fixture.consumer_ids()[0]));
    const auto sourceArgument = std::to_string(static_cast<std::uint16_t>(fixture.sources()[0]));
    const auto channelArgument = std::to_string(channel[1]);
    const auto child = ::fork();
    ASSERT_GE(child, 0);
    if (child == 0)
    {
      ::close(channel[0]);
      // Exec starts with fresh process-local coordination state. Inherited
      // handles remain forbidden; the worker uses only new public handles.
      ::execl(ANGARIA_PUBLIC_RUNTIME_CONSUMER_WORKER, ANGARIA_PUBLIC_RUNTIME_CONSUMER_WORKER,
          directory.c_str(), taskArgument.c_str(), sourceArgument.c_str(), channelArgument.c_str(),
          static_cast<char*>(nullptr));
      ::_exit(127);
    }
    ::close(channel[1]);
    std::array<std::uint64_t, 8> words{};
    pollfd ready{channel[0], POLLIN, 0};
    const auto available = ::poll(&ready, 1, 10000);
    const auto received = available == 1 ? ::read(channel[0], words.data(), sizeof(words)) : -1;
    // Always stop/reap the child, including malformed-output and timeout paths.
    // SIGKILL skips Source/Task destructors and therefore their normal cleanup.
    const auto stopped = ::kill(child, SIGKILL);
    ::close(channel[0]);
    int status{};
    const auto reaped = ::waitpid(child, &status, 0);
    ASSERT_EQ(available, 1);
    ASSERT_EQ(received, static_cast<ssize_t>(sizeof(words)));
    ASSERT_EQ(stopped, 0);
    ASSERT_EQ(reaped, child);
    ASSERT_TRUE(WIFSIGNALED(status));
    EXPECT_EQ(WTERMSIG(status), SIGKILL);
    const DeliveryToken predecessor{{{words[0], words[1]}, TaskId{static_cast<std::uint16_t>(words[2])}, words[3]},
        SourceQueueSetId{static_cast<std::uint16_t>(words[4])}, QueueId{static_cast<std::uint16_t>(words[5])}, words[6], words[7]};
    auto replacement = require_value(fixture.created()._session.attach_task(fixture.consumer_ids()[0]));
    auto source = require_value(replacement.open_source_set(fixture.sources()[0]));
    const auto recovered = std::get<Delivery>(source.receive());
    EXPECT_EQ(recovered._message._header._publication, publication);
    EXPECT_GT(recovered._token._consumer._incarnation, predecessor._consumer._incarnation);
    expect_error(source.acknowledge(predecessor), ErrorCode::StaleDelivery);
    require_value(source.acknowledge(recovered._token));
    EXPECT_TRUE(std::holds_alternative<ReceiveTimeout>(source.receive()));
  }

  TEST(PublicRuntimeAdmission, SelfLoopUsesTheSamePublicTaskClaimForSendingAndProcessing)
  {
    PublicRuntimeFixture fixture({._loopback = true});
    auto& task = fixture.created()._creator;
    auto writer = require_value(task.open_writer(fixture.queues()[0]));
    auto source = require_value(task.open_source_set(fixture.sources()[0]));
    const auto bytes = fixture.payload(77);
    const auto publication = require_value(task.next_publication_id());
    require_value(writer.send(publication, fixture.message(bytes)));
    const auto delivery = std::get<Delivery>(source.receive());
    EXPECT_EQ(delivery._message._header._publication._producer, task.claim());
    EXPECT_EQ(delivery._message._header._consumer, task.claim());
    require_value(source.acknowledge(delivery._token));
  }

  TEST(PublicRuntimeAdmission, SchemaPayloadAndOwnershipErrorsLeaveTheIssuedIdRetryable)
  {
    PublicRuntimeFixture fixture;
    auto& producer = fixture.created()._creator;
    auto writer = require_value(producer.open_writer(fixture.queues()[0]));
    auto consumer = require_value(fixture.created()._session.attach_task(fixture.consumer_ids()[0]));
    expect_error(consumer.open_writer(fixture.queues()[0]), ErrorCode::WrongOwner);
    expect_error(producer.open_source_set(fixture.sources()[0]), ErrorCode::WrongOwner);
    const auto bytes = fixture.payload(9);
    const auto wide = fixture.payload(9, 1);
    const auto publication = require_value(producer.next_publication_id());
    auto mismatched = fixture.message(wide, 0);
    ASSERT_TRUE(std::holds_alternative<Error>(writer.send(publication, mismatched)));
    auto wrongLength = fixture.message(bytes);
    ++wrongLength._header._payloadBytes;
    expect_error(writer.send(publication, wrongLength), ErrorCode::InvalidArgument);
    expect_accepted(require_value(writer.send(publication, fixture.message(bytes))), publication, fixture.queues()[0]);
    EXPECT_EQ(require_value(producer.publication_status(publication))._status, AcceptanceStatus::Accepted);
  }
}
