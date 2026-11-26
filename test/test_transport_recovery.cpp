// Combined native qualification: independent producer and consumer processes
// overlap public operations, die at deliberate journal cuts, and are replaced.
// Private reads/fill/cut seams below measure the real public path; they do not
// implement another transport or claim an application-level recovery protocol.
#include "source_fixture.h"

#include "delivery_words.h"
#include "mapped_payload.h"
#include "native_fixture.h"
#include "native_log_budget.h"
#include "native_process.h"
#include "result_assertions.h"
#include "topology_state.h"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <tuple>
#include <utility>
#include <variant>
#include <vector>

#ifndef ANGARIA_NATIVE_TRANSPORT_WORKER
#error ANGARIA_NATIVE_TRANSPORT_WORKER must name the combined transport worker
#endif

namespace
{
  using namespace angaria;
  namespace runtime = angaria::detail;
  namespace mapped = runtime::mapped;
  namespace support = angaria::test_support;
  namespace fs = std::filesystem;
  using support::require_value;

  std::unique_ptr<support::NativeProcess> worker(const fs::path& directory, std::string_view mode,
      mapped::JournalCheckpoint point = mapped::JournalCheckpoint::Committed,
      std::uint64_t index = 0, std::uint32_t value = 1)
  {
    return std::make_unique<support::NativeProcess>(std::vector<std::string>{
        ANGARIA_NATIVE_TRANSPORT_WORKER, directory.string(), std::string{mode},
        std::to_string(static_cast<unsigned>(point)), std::to_string(index), std::to_string(value)});
  }

  void kill_at_checkpoint(const fs::path& directory, std::string_view mode,
      mapped::JournalCheckpoint point, std::uint64_t index = 0, std::uint32_t value = 1)
  {
    auto process = worker(directory, mode, point, index, value);
    process->expect('R');
    process->terminate();
  }

  struct TransportSetup
  {
    SessionId _session;
    TaskId _producer;
    TaskId _consumer;
    SourceQueueSetId _source;
    QueueId _queue;
    SchemaId _schema;
    PublicationId _first;
  };

  class TransportRecoveryTest : public testing::Test
  {
  protected:
    void SetUp() override
    {
      auto pattern = (fs::temp_directory_path() / "angaria-transport-failure-XXXXXX").string();
      ASSERT_NE(::mkdtemp(pattern.data()), nullptr);
      _root = pattern;
    }

    void TearDown() override
    {
      std::error_code ignored;
      fs::remove_all(_root, ignored);
    }

    TransportSetup create(const fs::path& directory, DeliveryPolicy policy = DeliveryPolicy::RecoverFromLog)
    {
      auto scenario = support::create_source_scenario(directory, true, TaskKind::Process, policy);
      auto writer = require_value(scenario._created._creator.open_writer(scenario._queue));
      const auto first = support::send_source_value(scenario, writer, 1);
      return {scenario._created._session.identity(), first._producer._task, scenario._consumer,
              scenario._source, scenario._queue, scenario._schema, first};
    }

    DeliveryToken read_token(support::NativeProcess& process, char stage, const TransportSetup& setup)
    {
      process.expect(stage);
      const auto incarnation = process.receive<std::uint64_t>();
      const auto sequence = process.receive<std::uint64_t>();
      const auto issuance = process.receive<std::uint64_t>();
      return {{setup._session, setup._consumer, incarnation}, setup._source, setup._queue, sequence, issuance};
    }

    PublicationId read_publication(support::NativeProcess& process, char stage, const TransportSetup& setup)
    {
      process.expect(stage);
      const auto incarnation = process.receive<std::uint64_t>();
      const auto sequence = process.receive<std::uint64_t>();
      return {{setup._session, setup._producer, incarnation}, sequence};
    }

    void drain(SourceQueueSet& source, const BinaryPayloadCodec& codec,
        std::uint64_t first, std::uint64_t last, const DeliveryToken& stale)
    {
      for (auto sequence = first; sequence <= last; ++sequence)
      {
        auto event = source.receive();
        ASSERT_TRUE(std::holds_alternative<Delivery>(event));
        const auto current = std::get<Delivery>(std::move(event));
        EXPECT_EQ(current._token._sequence, sequence);
        const auto copy = require_value(current._message.copy_message(4));
        EXPECT_EQ(copy._header, current._message._header);
        const auto decoded = require_value(copy.decode(codec));
        ASSERT_EQ(decoded.size(), 1u);
        EXPECT_EQ(std::get<std::uint32_t>(decoded.front()), sequence);
        const auto image = support::recovered_image(_directory);
        support::expect_error(source.acknowledge(stale), ErrorCode::StaleDelivery);
        EXPECT_EQ(support::recovered_image(_directory), image);
        require_value(source.acknowledge(current._token));
      }
      EXPECT_TRUE(std::holds_alternative<ReceiveTimeout>(source.receive()));
      require_value(source.close());
    }

    fs::path _root;
    fs::path _directory;
  };

  using CrashCase = std::tuple<DeliveryPolicy, mapped::JournalCheckpoint>;

  class PinnedProducerCrashTest : public TransportRecoveryTest,
      public testing::WithParamInterface<CrashCase>
  {
  };

  TEST_P(PinnedProducerCrashTest, AProducerCrashCannotRevokeTheLiveConsumerPinOrItsAcceptanceHistory)
  {
    const auto [policy, point] = GetParam();
    _directory = _root / "session";
    const auto setup = create(_directory, policy);
    const auto initial_revision = support::journal_snapshot(_directory)._revision;
    auto consumer = worker(_directory, "pin");
    const auto old = read_token(*consumer, 'P', setup);
    EXPECT_EQ(old._sequence, 1u);
    PublicationId uncertain;
    {
      auto producer = worker(_directory, "produce", point, 0, 4);
      uncertain = read_publication(*producer, 'I', setup);
      producer->expect('R');
      producer->terminate();
    }

    // Repeat interruption before any public operation gets to settle the
    // uncertain append. Prepared rollback and committed redo remain distinct.
    const auto recovery_point = point == mapped::JournalCheckpoint::Prepared ?
        mapped::JournalCheckpoint::Rollback : mapped::JournalCheckpoint::RedoWord;
    kill_at_checkpoint(_directory, "recover", recovery_point, 0);
    kill_at_checkpoint(_directory, "recover", recovery_point,
        point == mapped::JournalCheckpoint::Prepared ? 0 : 1);

    consumer->proceed();
    consumer->expect('B'); // Child compared full header, token, address and bytes.
    consumer->expect('R');
    consumer->terminate();

    auto session = require_value(Session::attach(_directory));
    auto producer = require_value(session.attach_task("Producer"));
    EXPECT_GT(producer.claim()._incarnation, uncertain._producer._incarnation);
    const auto committed = point != mapped::JournalCheckpoint::Prepared;
    const auto outcome = require_value(producer.publication_status(uncertain));
    EXPECT_EQ(outcome._status, committed ? AcceptanceStatus::Accepted : AcceptanceStatus::Rejected);
    EXPECT_EQ(require_value(producer.publication_status(setup._first))._status, AcceptanceStatus::Accepted);
    const auto observed = support::recovered_image(_directory);
    EXPECT_EQ(require_value(producer.publication_status(uncertain))._status, outcome._status);
    EXPECT_EQ(support::recovered_image(_directory), observed);

    auto replacement = require_value(session.attach_task("Consumer"));
    EXPECT_GT(replacement.claim()._incarnation, old._consumer._incarnation);
    auto source = require_value(replacement.open_source_set("Inputs"));
    const auto last = committed ? 4u : 3u;
    const auto first = policy == DeliveryPolicy::AllowLoss ? last - 1 : 1u;
    if (policy == DeliveryPolicy::AllowLoss)
    {
      auto event = source.receive();
      ASSERT_TRUE(std::holds_alternative<DeliveryGap>(event));
      const auto gap = std::get<DeliveryGap>(event);
      EXPECT_EQ(gap._queue, setup._queue);
      EXPECT_EQ(gap._firstSequence, 1u);
      EXPECT_EQ(gap._lastSequence, first - 1);
    }
    const auto codec = require_value(BinaryPayloadCodec::from_registry(session.registry(), setup._schema, 4));
    drain(source, codec, first, last, old);
    const auto after = support::journal_snapshot(_directory);
    // Delivery/recovery changes no Registry definitions. The publication's
    // recorded Queue topology revision is a distinct, earlier revision.
    EXPECT_EQ(after._revision, initial_revision);
  }

  std::string crash_case_name(const testing::TestParamInfo<CrashCase>& parameter)
  {
    const auto [policy, point] = parameter.param;
    return std::string{policy == DeliveryPolicy::AllowLoss ? "AllowedLoss" : "Recover"} +
        (point == mapped::JournalCheckpoint::Prepared ? "Prepared" :
         point == mapped::JournalCheckpoint::Committed ? "Committed" : "PartialRedo");
  }

  INSTANTIATE_TEST_SUITE_P(DeliveryPolicies, PinnedProducerCrashTest,
      testing::Combine(testing::Values(DeliveryPolicy::RecoverFromLog, DeliveryPolicy::AllowLoss),
          testing::Values(mapped::JournalCheckpoint::Prepared, mapped::JournalCheckpoint::Committed,
              mapped::JournalCheckpoint::RedoWord)), crash_case_name);

  TEST_F(TransportRecoveryTest, ANewProducerSendFirstRecoversTheTerminatedConsumersCommittedAcknowledgment)
  {
    for (const auto point : {mapped::JournalCheckpoint::Committed, mapped::JournalCheckpoint::RedoWord})
    {
      SCOPED_TRACE(static_cast<unsigned>(point));
      _directory = _root / std::to_string(static_cast<unsigned>(point));
      const auto setup = create(_directory);
      auto consumer = worker(_directory, "ack", point,
          point == mapped::JournalCheckpoint::RedoWord ? 3 : 0);
      const auto old = read_token(*consumer, 'A', setup);
      consumer->expect('R');
      consumer->terminate();

      // This separate producer's Session::attach is the first recovery entrant.
      // The parent intentionally performs no read that could settle the ACK.
      auto sender = worker(_directory, "send", mapped::JournalCheckpoint::Committed, 0, 2);
      const auto second = read_publication(*sender, 'S', setup);
      sender->wait_success();
      auto session = require_value(Session::attach(_directory));
      {
        runtime::RuntimeAccess access(support::open_runtime_state(_directory));
        const auto topology = runtime::runtime_topology(access);
        const auto queue = access.read_words(topology._queues.front()._stateOffset, 12);
        const auto first = runtime::runtime_publication_metadata(access, access.record_references(9).front());
        const auto receipt = require_value(mapped::decode_receipt_words(
            access.read_words(first._destinations.front()._receiptReference, 16)));
        EXPECT_EQ(receipt._state, mapped::ReceiptState::Acknowledged);
        EXPECT_EQ(queue[8], 1u);
        EXPECT_NE(queue[9], 0u); // Fresh send reopened the formerly empty chain.
        EXPECT_EQ(queue[10], static_cast<std::uint64_t>(receipt._acknowledgedAt.time_since_epoch().count()));
        EXPECT_EQ(queue[11], 1u);
      }
      auto producer = require_value(session.attach_task("Producer"));
      EXPECT_EQ(require_value(producer.publication_status(second))._status, AcceptanceStatus::Accepted);
      auto replacement = require_value(session.attach_task("Consumer"));
      auto source = require_value(replacement.open_source_set("Inputs"));
      const auto codec = require_value(BinaryPayloadCodec::from_registry(session.registry(), setup._schema, 4));
      drain(source, codec, 2, 2, old);
    }
  }

  TEST_F(TransportRecoveryTest, MixedTerminalHistoryAndBothOwnerDeathsRecoverAtTheExactLogCeiling)
  {
    _directory = _root / "full";
    TransportSetup setup;
    std::unique_ptr<support::NativeProcess> consumer;
    std::vector<PublicationId> ids;
    DeliveryToken old;
    {
      auto scenario = support::create_source_scenario(_directory, true, TaskKind::Process, DeliveryPolicy::AllowLoss);
      setup = {scenario._created._session.identity(), scenario._created._creator.claim()._task,
          scenario._consumer, scenario._source, scenario._queue, scenario._schema, {}};
      auto writer = require_value(scenario._created._creator.open_writer(setup._queue));
      ids.push_back(support::send_source_value(scenario, writer, 1));
      setup._first = ids.front();
      {
        auto task = require_value(scenario._created._session.attach_task("Consumer"));
        auto source = require_value(task.open_source_set("Inputs"));
        auto event = source.receive();
        ASSERT_TRUE(std::holds_alternative<Delivery>(event));
        require_value(source.acknowledge(std::get<Delivery>(event)._token));
        require_value(source.close());
      }
      for (std::uint32_t value = 2; value <= 5; ++value)
      {
        ids.push_back(support::send_source_value(scenario, writer, value));
      }
      {
        auto task = require_value(scenario._created._session.attach_task("Consumer"));
        auto source = require_value(task.open_source_set("Inputs"));
        auto event = source.receive();
        ASSERT_TRUE(std::holds_alternative<DeliveryGap>(event));
        EXPECT_EQ(std::get<DeliveryGap>(event)._firstSequence, 2u);
        EXPECT_EQ(std::get<DeliveryGap>(event)._lastSequence, 3u);
        require_value(source.close());
      }
      consumer = worker(_directory, "pin", mapped::JournalCheckpoint::Committed, 0, 4);
      old = read_token(*consumer, 'P', setup);
      EXPECT_EQ(old._sequence, 4u);
      ids.push_back(support::send_source_value(scenario, writer, 6));
      support::fill_log_with_queue_revision(support::open_runtime_state(_directory), setup._queue,
          scenario._created._creator.claim());
      const auto full = support::journal_snapshot(_directory);
      EXPECT_EQ(full._tail, full._fileBytes);
      const auto image = support::recovered_image(_directory);
      const auto bytes = require_value(support::source_codec(scenario).encode({std::uint32_t{6}}));
      EXPECT_EQ(require_value(writer.send(ids.back(), support::source_message(setup._schema, bytes)))._status,
          AcceptanceStatus::Accepted);
      EXPECT_EQ(support::recovered_image(_directory), image);
    }
    const auto full = support::journal_snapshot(_directory);
    auto producer_owner = worker(_directory, "hold");
    producer_owner->expect('H');
    const auto killed_producer_incarnation = producer_owner->receive<std::uint64_t>();
    producer_owner->expect('R');
    producer_owner->terminate();
    consumer->proceed();
    consumer->expect('B');
    consumer->expect('R');
    consumer->terminate();

    // Now the full log retains Ack1, Lost2..3, Issued4, Pending5..6. Interrupt
    // captured old-claim cleanup and then interrupt its recovery twice again.
    kill_at_checkpoint(_directory, "cleanup", mapped::JournalCheckpoint::RedoWord, 0,
        static_cast<std::uint16_t>(setup._consumer));
    kill_at_checkpoint(_directory, "recover", mapped::JournalCheckpoint::RedoWord, 0);
    kill_at_checkpoint(_directory, "recover", mapped::JournalCheckpoint::RedoWord, 1);
    auto session = require_value(Session::attach(_directory));
    auto producer = require_value(session.attach_task("Producer"));
    EXPECT_GT(producer.claim()._incarnation, killed_producer_incarnation);
    for (const auto id : ids)
    {
      EXPECT_EQ(require_value(producer.publication_status(id))._status, AcceptanceStatus::Accepted);
    }
    auto writer = require_value(producer.open_writer("Updates"));
    const auto fresh = require_value(producer.next_publication_id());
    const auto codec = require_value(BinaryPayloadCodec::from_registry(session.registry(), setup._schema, 4));
    const auto bytes = require_value(codec.encode({std::uint32_t{7}}));
    const auto image = support::recovered_image(_directory);
    support::expect_error(writer.send(fresh, support::source_message(setup._schema, bytes)), ErrorCode::LogFull);
    EXPECT_EQ(support::recovered_image(_directory), image);

    auto replacement = require_value(session.attach_task("Consumer"));
    auto source = require_value(replacement.open_source_set("Inputs"));
    auto event = source.receive();
    ASSERT_TRUE(std::holds_alternative<DeliveryGap>(event));
    EXPECT_EQ(std::get<DeliveryGap>(event)._firstSequence, 4u);
    EXPECT_EQ(std::get<DeliveryGap>(event)._lastSequence, 4u);
    drain(source, codec, 5, 6, old);
    const auto after = support::journal_snapshot(_directory);
    EXPECT_EQ(after._tail, full._tail);
    EXPECT_EQ(after._fileBytes, full._fileBytes);
    EXPECT_EQ(after._revision, full._revision);
  }
} // namespace
