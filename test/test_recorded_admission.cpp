// Expanded private transport binding, beneath public Task admission. These
// fixtures supply explicit authoritative claims and catalog references; public
// lifecycle tests separately establish where that authority comes from.
#include "recorded_queue_fixture.h"
#include "result_assertions.h"

#include <gtest/gtest.h>

#include <array>
#include <exception>
#include <memory>
#include <optional>
#include <thread>
#include <utility>
#include <variant>

namespace
{
  using namespace angaria;
  namespace recorded = angaria::detail::recorded;
  namespace support = angaria::test_support;
  using support::require_value;
  using support::require_recorded_send;

  struct SharedBindings
  {
    recorded::RecordedQueueOptions _first;
    recorded::RecordedQueueOptions _second;
  };

  SharedBindings shared_bindings(const std::filesystem::path& directory)
  {
    auto first = support::recorded_queue_options(directory, 4);
    first._log._queues.push_back(QueueId{5});
    auto& catalog = first._catalog;
    const auto registered = catalog._tasks.front()._registration;
    TaskDescriptor consumer;
    consumer._name = "SecondConsumer";
    catalog._tasks.push_back({TaskId{7}, consumer, registered});
    auto source = catalog._sources.front();
    source._id = SourceQueueSetId{3};
    source._descriptor._name = "SecondInput";
    source._descriptor._owner = TaskId{7};
    source._members = {QueueId{5}};
    catalog._sources.push_back(std::move(source));
    auto queue = catalog._queues.front();
    queue._id = QueueId{5};
    queue._descriptor._name = "SecondQueue";
    queue._descriptor._sourceSet = SourceQueueSetId{3};
    catalog._queues.push_back(std::move(queue));

    // Same logical message type, different exact version and wire width. A
    // cache keyed only by message type would decode the second message wrongly.
    FieldDescriptor field;
    field._id = FieldId{0xee000002};
    field._name = "SmallValue";
    field._base = EFieldBase::BASE_UINT32;
    field._size = 4;
    catalog._fields.push_back({field, registered});
    MessageDescriptor message;
    message._schema = {MessageType{1}, 2};
    message._name = "Sample";
    message._fields = {field._id};
    catalog._messages.push_back({message, registered});
    first._schemaBindings = {{first._schema, first._schemaReference}, {message._schema, 4104}};

    auto second = first;
    second._connectionPath = directory / "second.connection";
    second._queue = QueueId{5};
    second._source = SourceQueueSetId{3};
    second._consumer = TaskId{7};
    return {std::move(first), std::move(second)};
  }

  class InterruptAttachment final : public recorded::ProgressObserver
  {
  public:
    OperationResult checkpoint(recorded::ProgressCheckpoint point) override
    {
      if (point == recorded::ProgressCheckpoint::SelectorPublished)
      {
        return Error{ErrorCode::IoError, "Interrupted selected attachment", "qualification"};
      }
      return std::monostate{};
    }
  };
} // namespace

TEST(RecordedAdmission, OutgoingQueuesShareOneClaimAndAllExactSchemaCodecs)
{
  support::ProducerLogDirectory directory;
  auto options = shared_bindings(directory.path());
  require_value(recorded::ProducerLog::create(options._first._producerPaths, options._first._log));
  require_value(recorded::create_recorded_connection(options._first));
  require_value(recorded::create_recorded_connection(options._second));
  const TaskClaim actor{options._first._log._header._session, TaskId{3}, 9};
  auto log = std::make_shared<recorded::ProducerLog>(require_value(recorded::ProducerLog::open_writer(
      options._first._producerPaths, options._first._log, nullptr, actor)));
  auto first = require_value(recorded::RecordedProducer::open_shared(options._first, log));
  auto sameQueue = require_value(recorded::RecordedProducer::open_shared(options._first, log));
  auto second = require_value(recorded::RecordedProducer::open_shared(options._second, log));
  auto firstConsumer = require_value(recorded::RecordedConsumer::open(options._first));
  auto secondConsumer = require_value(recorded::RecordedConsumer::open(options._second));
  auto codec = support::recorded_queue_codec(options._first);
  auto bytes = support::recorded_queue_payload(codec, 17);
  const auto firstId = require_value(first.next_publication_id());
  require_recorded_send(first.try_send(firstId, support::recorded_queue_message(options._first, bytes)));
  auto smallCodec = require_value(BinaryPayloadCodec::from_snapshot(options._first._catalog, {MessageType{1}, 2}, 64));
  const auto smallBytes = require_value(smallCodec.encode({std::uint32_t{23}}));
  auto small = support::recorded_queue_message(options._first, smallBytes);
  small._header._schema = {MessageType{1}, 2};
  const auto secondId = require_value(second.next_publication_id());
  require_recorded_send(second.try_send(secondId, small));
  EXPECT_EQ(secondId._sequence, firstId._sequence + 1);
  EXPECT_EQ(secondId._producer, actor);

  auto firstMessage = require_value(firstConsumer.try_receive());
  ASSERT_TRUE(firstMessage);
  // Returning credit through one endpoint must not make another writer's cached
  // accepted count appear corrupt. Both handles share the same Task log owner.
  const auto thirdId = require_value(sameQueue.next_publication_id());
  require_recorded_send(sameQueue.try_send(thirdId, small));
  auto firstSmall = require_value(firstConsumer.try_receive());
  ASSERT_TRUE(firstSmall);
  EXPECT_EQ(firstSmall->_header._schema, small._header._schema);
  EXPECT_EQ(firstSmall->_header._queueSequence, 2u);
  EXPECT_EQ(require_value(firstSmall->copy_message(64))._header, firstSmall->_header);
  auto secondMessage = require_value(secondConsumer.try_receive());
  ASSERT_TRUE(secondMessage);
  EXPECT_EQ(secondMessage->_header._publication, secondId);
  EXPECT_EQ(secondMessage->_header._queueSequence, 1u);
  EXPECT_EQ(require_value(smallCodec.decode(secondMessage->_payload)), std::vector<PayloadValue>{std::uint32_t{23}});
  EXPECT_FALSE(require_value(secondConsumer.try_receive()));
  EXPECT_EQ(require_value(log->record_count()), 3u);
}

TEST(RecordedAdmission, ReopeningAnAcknowledgedSourceUnderTheSameTaskDoesNotReuseTokens)
{
  support::ProducerLogDirectory directory;
  auto options = support::recorded_queue_options(directory.path());
  options._acknowledged = true;
  require_value(recorded::create_recorded_queue(options));
  auto producer = require_value(recorded::RecordedProducer::open(options));
  auto codec = support::recorded_queue_codec(options);
  auto bytes = support::recorded_queue_payload(codec, 7);
  require_recorded_send(producer.try_send(require_value(producer.next_publication_id()),
      support::recorded_queue_message(options, bytes)));
  const TaskClaim actor{options._log._header._session, options._consumer, 12};
  auto consumer = require_value(recorded::RecordedAcknowledgedConsumer::open(options, nullptr, nullptr, actor));
  auto delivery = require_value(consumer.try_receive());
  ASSERT_TRUE(delivery);
  const auto previousToken = delivery->_token;
  require_value(consumer.close());
  consumer = require_value(recorded::RecordedAcknowledgedConsumer::open(options, nullptr, nullptr, actor));
  auto replacement = require_value(consumer.try_receive());
  ASSERT_TRUE(replacement);
  EXPECT_NE(replacement->_token, previousToken);
  EXPECT_EQ(replacement->_token._consumer, actor);
  support::expect_error(consumer.acknowledge(previousToken), ErrorCode::StaleDelivery);
  require_value(consumer.acknowledge(replacement->_token));
}

TEST(RecordedAdmission, InterruptedAttachmentCanBeRecoveredAfterSkippedTaskIncarnations)
{
  support::ProducerLogDirectory directory;
  auto options = support::recorded_queue_options(directory.path());
  options._acknowledged = true;
  require_value(recorded::create_recorded_queue(options));
  InterruptAttachment interruption;
  const TaskClaim first{options._log._header._session, options._consumer, 12};
  auto interrupted = recorded::RecordedAcknowledgedConsumer::open(options, nullptr, &interruption, first);
  ASSERT_TRUE(std::holds_alternative<Error>(interrupted));
  // Task incarnation comes from public admission, not the local connection
  // hint; other Task attachments can advance it before this Source reopens.
  const TaskClaim replacement{first._session, first._task, 15};
  auto consumer = require_value(recorded::RecordedAcknowledgedConsumer::open(options, nullptr, nullptr, replacement));
  EXPECT_EQ(require_value(consumer.claim()), replacement);
  EXPECT_FALSE(require_value(consumer.try_receive()));
}

TEST(RecordedAdmission, AuthoritativeProcessClaimsAllowSerializedCallsFromDifferentThreads)
{
  support::ProducerLogDirectory directory;
  auto options = support::recorded_queue_options(directory.path());
  options._acknowledged = true;
  require_value(recorded::create_recorded_queue(options));
  const auto session = options._log._header._session;
  const TaskClaim producerActor{session, options._log._header._producer, 9};
  auto log = std::make_shared<recorded::ProducerLog>(require_value(recorded::ProducerLog::open_writer(
      options._producerPaths, options._log, nullptr, producerActor)));
  auto producer = require_value(recorded::RecordedProducer::open_shared(options, log));
  auto consumer = require_value(recorded::RecordedAcknowledgedConsumer::open(
      options, nullptr, nullptr, TaskClaim{session, options._consumer, 12}));
  auto codec = support::recorded_queue_codec(options);
  auto bytes = support::recorded_queue_payload(codec, 7);
  // No concurrent calls: the public Task and Source mutexes supply this same
  // serialization, while public Thread Tasks retain their stricter affinity.
  std::exception_ptr failure;
  std::thread worker([&]
  {
    try
    {
      require_recorded_send(producer.try_send(require_value(log->next_publication_id()),
          support::recorded_queue_message(options, bytes)));
      auto delivery = require_value(consumer.try_receive());
      ASSERT_TRUE(delivery);
      require_value(consumer.acknowledge(delivery->_token));
    }
    catch (...)
    {
      failure = std::current_exception();
    }
  });
  worker.join();
  if (failure)
  {
    std::rethrow_exception(failure);
  }
  EXPECT_EQ(require_value(consumer.progress())._source._acknowledged, 1u);
}

TEST(RecordedAdmission, TasksWithoutOutgoingQueuesCanIssueIdsWithoutPermittingAppends)
{
  support::ProducerLogDirectory directory;
  const auto paths = support::producer_log_paths(directory.path());
  auto options = support::producer_log_options();
  options._queues.clear();
  require_value(recorded::ProducerLog::create(paths, options));
  const TaskClaim actor{options._header._session, options._header._producer, 9};
  auto log = require_value(recorded::ProducerLog::open_writer(paths, options, nullptr, actor));
  auto id = require_value(log.next_publication_id());
  EXPECT_EQ(id._producer, actor);
  EXPECT_FALSE(require_value(log.lookup(id)));
  const std::array<std::byte, 8> bytes{};
  support::expect_error(log.append(support::producer_append_request(id, bytes)), ErrorCode::InvalidArgument);
}

TEST(RecordedAdmission, ATaskCanSendToItsOwnSource)
{
  support::ProducerLogDirectory directory;
  auto options = support::recorded_queue_options(directory.path());
  options._consumer = options._log._header._producer;
  options._catalog._sources.front()._descriptor._owner = options._consumer;
  require_value(recorded::create_recorded_queue(options));
  auto producer = require_value(recorded::RecordedProducer::open(options));
  auto consumer = require_value(recorded::RecordedConsumer::open(options));
  auto codec = support::recorded_queue_codec(options);
  auto bytes = support::recorded_queue_payload(codec, 7);
  const auto id = require_value(producer.next_publication_id());
  require_recorded_send(producer.try_send(id, support::recorded_queue_message(options, bytes)));
  auto message = require_value(consumer.try_receive());
  ASSERT_TRUE(message);
  ASSERT_TRUE(message->_header._consumer);
  EXPECT_EQ(message->_header._consumer->_task, message->_header._publication._producer._task);
}
