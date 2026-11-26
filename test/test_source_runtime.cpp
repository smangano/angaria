// Public Source examples: receive a borrowed message, inspect/copy its complete
// header, then acknowledge or release the exact token. The private hooks and
// byte snapshots are explicitly labeled failure/ownership qualification seams.
#include "source_fixture.h"

#include "native_fixture.h"
#include "native_log_budget.h"
#include "process_support.h"
#include "result_assertions.h"
#include "source_runtime_state.h"
#include "wire_fixture.h"

#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <latch>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <system_error>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

#include <sys/wait.h>
#include <unistd.h>

namespace
{
  using namespace angaria;
  namespace runtime = angaria::detail;
  namespace mapped = angaria::detail::mapped;
  namespace support = angaria::test_support;
  namespace fs = std::filesystem;
  using support::expect_error;
  using support::require_value;
  using namespace std::chrono_literals;

  Delivery receive_delivery(SourceQueueSet& source, std::chrono::nanoseconds timeout = 0ns)
  {
    auto result = source.receive(timeout);
    if (const auto* error = std::get_if<Error>(&result))
    {
      throw std::runtime_error(error->_path + ": " + error->_message);
    }
    if (!std::holds_alternative<Delivery>(result))
    {
      throw std::runtime_error("Expected a delivery, not a timeout or gap");
    }
    return std::get<Delivery>(std::move(result));
  }

  void expect_receive_error(const ReceiveResult& result, ErrorCode code)
  {
    ASSERT_TRUE(std::holds_alternative<Error>(result));
    const auto& error = std::get<Error>(result);
    EXPECT_EQ(error._code, code) << error._path << ": " << error._message;
    EXPECT_FALSE(error._path.empty());
    EXPECT_FALSE(error._message.empty());
  }

  std::vector<std::byte> payload_bytes(const MessageView& message)
  {
    return {message._payload.begin(), message._payload.end()};
  }

  class SourceRuntimeTest : public testing::Test
  {
  protected:
    void SetUp() override
    {
      auto pattern = (fs::temp_directory_path() / "angaria-source-api-XXXXXX").string();
      ASSERT_NE(::mkdtemp(pattern.data()), nullptr);
      _root = pattern;
      _directory = _root / "session";
    }

    void TearDown() override
    {
      std::error_code ignored;
      fs::remove_all(_root, ignored);
    }

    fs::path _root;
    fs::path _directory;
  };

  // Acceptance and consumer processing are separate events. A Delivery presents
  // one consistent header, while its token carries the live acknowledgment duty.
  TEST_F(SourceRuntimeTest, ReceivedMessageExposesTheCompleteHeaderAndOwningCopiesOutliveTheClaim)
  {
    auto scenario = support::create_source_scenario(_directory);
    auto writer = require_value(scenario._created._creator.open_writer("Updates"));
    const auto publication = support::send_source_value(scenario, writer, 42, 0xdeadbeef);
    const auto codec = support::source_codec(scenario);
    std::optional<Task> consumer{require_value(scenario._created._session.attach_task("Consumer"))};
    const auto claim = consumer->claim();
    auto source = require_value(consumer->open_source_set("Inputs"));
    const auto delivery = receive_delivery(source);
    const auto header = delivery._message._header;
    EXPECT_EQ(header._publication, publication);
    EXPECT_EQ(header._schema, scenario._schema);
    EXPECT_EQ(header._format, PayloadFormat::Binary);
    EXPECT_EQ(header._flags, 0xdeadbeefu);
    EXPECT_EQ(header._payloadBytes, 4u);
    EXPECT_EQ(header._queue, scenario._queue);
    EXPECT_EQ(header._queueSequence, 1u);
    EXPECT_EQ(header._consumer, claim);
    ASSERT_TRUE(header._enqueuedAt);
    ASSERT_TRUE(header._dequeuedAt);
    EXPECT_EQ(header._publishedAt, *header._enqueuedAt);
    EXPECT_EQ(delivery.context()._header, header);
    EXPECT_FALSE(delivery.context()._replayed);
    expect_error(delivery._message.copy_message(3), ErrorCode::CapacityExceeded);
    auto saved = require_value(delivery._message.copy_message(4));
    EXPECT_EQ(saved._header, header);
    EXPECT_NE(saved._payload.data(), delivery._message._payload.data());
    require_value(source.acknowledge(delivery._token));
    require_value(source.close());
    consumer.reset();

    // Retaining a Message neither keeps the old Source pin nor its Task claim.
    // Its exact schema definitions remain usable after that consumer is replaced.
    auto replacement = require_value(scenario._created._session.attach_task("Consumer"));
    EXPECT_EQ(replacement.claim()._incarnation, claim._incarnation + 1);
    EXPECT_EQ(saved._header, header);
    EXPECT_EQ(std::get<std::uint32_t>(require_value(saved.decode(codec)).front()), 42u);
    EXPECT_EQ(saved.view()._header, header);
  }

  TEST_F(SourceRuntimeTest, RepeatedReceiveKeepsTheSameTokenTimeAndMappedAddress)
  {
    auto scenario = support::create_source_scenario(_directory);
    auto writer = require_value(scenario._created._creator.open_writer(scenario._queue));
    support::send_source_value(scenario, writer, 7);
    auto consumer = require_value(scenario._created._session.attach_task(scenario._consumer));
    auto source = require_value(consumer.open_source_set(scenario._source));
    const auto first = receive_delivery(source);
    const auto settled = support::recovered_image(_directory);
    const auto repeated = receive_delivery(source, 1ns);
    EXPECT_EQ(repeated._token, first._token);
    EXPECT_EQ(repeated._message._header, first._message._header);
    EXPECT_EQ(repeated._message._payload.data(), first._message._payload.data());
    EXPECT_EQ(payload_bytes(first._message), payload_bytes(repeated._message));
    EXPECT_EQ(support::recovered_image(_directory), settled);
    require_value(source.acknowledge(repeated._token));
    EXPECT_TRUE(std::holds_alternative<ReceiveTimeout>(source.receive()));
  }

  // The ring caches only two recent references. An already borrowed payload
  // stays at its address while later sends grow the file and overwrite its slot.
  TEST_F(SourceRuntimeTest, BorrowedPayloadSurvivesRingOverwriteAndHistoryDeliversTheBacklog)
  {
    auto scenario = support::create_source_scenario(_directory);
    auto writer = require_value(scenario._created._creator.open_writer(scenario._queue));
    support::send_source_value(scenario, writer, 1);
    auto consumer = require_value(scenario._created._session.attach_task(scenario._consumer));
    auto source = require_value(consumer.open_source_set(scenario._source));
    const auto first = receive_delivery(source);
    const auto original = payload_bytes(first._message);
    const auto address = first._message._payload.data();
    for (std::uint32_t value = 2; value != 7; ++value)
    {
      support::send_source_value(scenario, writer, value);
    }
    EXPECT_EQ(payload_bytes(first._message), original);
    EXPECT_EQ(receive_delivery(source)._message._payload.data(), address);
    require_value(source.acknowledge(first._token));
    const auto codec = support::source_codec(scenario);
    for (std::uint32_t value = 2; value != 7; ++value)
    {
      const auto next = receive_delivery(source);
      EXPECT_EQ(next._message._header._queueSequence, value);
      const auto saved = require_value(next._message.copy_message(4));
      EXPECT_EQ(std::get<std::uint32_t>(require_value(saved.decode(codec)).front()), value);
      require_value(source.acknowledge(next._token));
    }
    EXPECT_TRUE(std::holds_alternative<ReceiveTimeout>(source.receive()));
  }

  TEST_F(SourceRuntimeTest, ReleaseMakesTheSameMessageAvailableWithANewIssuance)
  {
    auto scenario = support::create_source_scenario(_directory);
    auto writer = require_value(scenario._created._creator.open_writer(scenario._queue));
    const auto publication = support::send_source_value(scenario, writer, 10);
    auto consumer = require_value(scenario._created._session.attach_task(scenario._consumer));
    auto source = require_value(consumer.open_source_set(scenario._source));
    const auto first = receive_delivery(source);
    const auto bytes = payload_bytes(first._message);
    require_value(source.release(first._token));
    const auto next = receive_delivery(source);
    EXPECT_EQ(next._message._header._publication, publication);
    EXPECT_EQ(next._token._sequence, first._token._sequence);
    EXPECT_GT(next._token._issuance, first._token._issuance);
    EXPECT_EQ(payload_bytes(next._message), bytes);
    expect_error(source.acknowledge(first._token), ErrorCode::StaleDelivery);
    expect_error(source.release(first._token), ErrorCode::StaleDelivery);
    require_value(source.acknowledge(next._token));
  }

  TEST_F(SourceRuntimeTest, EveryStaleTokenComponentRejectsWithoutEndingTheCurrentBorrow)
  {
    auto scenario = support::create_source_scenario(_directory);
    auto writer = require_value(scenario._created._creator.open_writer(scenario._queue));
    support::send_source_value(scenario, writer, 20);
    auto consumer = require_value(scenario._created._session.attach_task(scenario._consumer));
    auto source = require_value(consumer.open_source_set(scenario._source));
    const auto active = receive_delivery(source);
    std::array<DeliveryToken, 7> tokens;
    tokens.fill(active._token);
    ++tokens[0]._consumer._session._high;
    tokens[1]._consumer._task = TaskId{8};
    ++tokens[2]._consumer._incarnation;
    tokens[3]._sourceSet = SourceQueueSetId{8};
    tokens[4]._queue = QueueId{8};
    ++tokens[5]._sequence;
    ++tokens[6]._issuance;
    const auto before = support::recovered_image(_directory);
    for (const auto& token : tokens)
    {
      expect_error(source.acknowledge(token), ErrorCode::StaleDelivery);
      expect_error(source.release(token), ErrorCode::StaleDelivery);
      const auto repeated = receive_delivery(source);
      EXPECT_EQ(repeated._token, active._token);
      EXPECT_EQ(repeated._message._payload.data(), active._message._payload.data());
    }
    EXPECT_EQ(support::recovered_image(_directory), before);
    require_value(source.acknowledge(active._token));
  }

  TEST_F(SourceRuntimeTest, ExplicitCloseReleasesThePinAndReopeningDoesNotReuseItsToken)
  {
    auto scenario = support::create_source_scenario(_directory);
    auto writer = require_value(scenario._created._creator.open_writer(scenario._queue));
    support::send_source_value(scenario, writer, 30);
    auto consumer = require_value(scenario._created._session.attach_task(scenario._consumer));
    auto source = require_value(consumer.open_source_set(scenario._source));
    const auto old = receive_delivery(source)._token;
    require_value(source.close());
    require_value(source.close());
    expect_receive_error(source.receive(), ErrorCode::InvalidState);
    expect_error(source.acknowledge(old), ErrorCode::InvalidState);
    expect_error(source.release(old), ErrorCode::InvalidState);
    auto reopened = require_value(consumer.open_source_set(scenario._source));
    const auto fresh = receive_delivery(reopened);
    EXPECT_EQ(fresh._token._sequence, old._sequence);
    EXPECT_GT(fresh._token._issuance, old._issuance);
    expect_error(reopened.acknowledge(old), ErrorCode::StaleDelivery);
    require_value(reopened.acknowledge(fresh._token));
  }

  TEST_F(SourceRuntimeTest, DestructionReleasesAnUnacknowledgedDelivery)
  {
    auto scenario = support::create_source_scenario(_directory);
    auto writer = require_value(scenario._created._creator.open_writer(scenario._queue));
    support::send_source_value(scenario, writer, 40);
    auto consumer = require_value(scenario._created._session.attach_task(scenario._consumer));
    DeliveryToken old;
    {
      auto source = require_value(consumer.open_source_set(scenario._source));
      old = receive_delivery(source)._token;
      // No implicit acknowledgment: leaving scope makes this message pending.
    }
    auto reopened = require_value(consumer.open_source_set(scenario._source));
    const auto repeated = receive_delivery(reopened);
    EXPECT_EQ(repeated._token._sequence, old._sequence);
    EXPECT_GT(repeated._token._issuance, old._issuance);
    require_value(reopened.acknowledge(repeated._token));
  }

  TEST_F(SourceRuntimeTest, MoveTransfersViewsAndMoveAssignmentClosesThePreviousSource)
  {
    auto scenario = support::create_source_scenario(_directory);
    auto actor = scenario._created._creator.registry();
    SourceQueueSetDescriptor other;
    other._name = "OtherInputs";
    other._owner = scenario._consumer;
    const auto other_source = require_value(actor.register_source_set(other));
    QueueDescriptor queue;
    queue._name = "OtherUpdates";
    queue._producer = scenario._created._creator.claim()._task;
    queue._sourceSet = other_source;
    const auto other_queue = require_value(actor.register_queue(queue));
    require_value(actor.activate_source_set(other_source));
    require_value(actor.activate_queue(other_queue));
    auto writer = require_value(scenario._created._creator.open_writer(scenario._queue));
    auto other_writer = require_value(scenario._created._creator.open_writer(other_queue));
    support::send_source_value(scenario, writer, 1);
    support::send_source_value(scenario, other_writer, 2);
    auto consumer = require_value(scenario._created._session.attach_task(scenario._consumer));
    auto first_source = require_value(consumer.open_source_set(scenario._source));
    const auto first = receive_delivery(first_source);
    auto moved = std::move(first_source);
    require_value(first_source.close()); // Empty moved-from close is harmless.
    EXPECT_EQ(receive_delivery(moved)._message._payload.data(), first._message._payload.data());
    auto second_source = require_value(consumer.open_source_set(other_source));
    const auto second = receive_delivery(second_source);
    moved = std::move(second_source);
    EXPECT_EQ(receive_delivery(moved)._token, second._token);
    EXPECT_EQ(payload_bytes(second._message), payload_bytes(receive_delivery(moved)._message));
    auto reopened = require_value(consumer.open_source_set(scenario._source));
    const auto redelivered = receive_delivery(reopened);
    EXPECT_EQ(redelivered._token._sequence, first._token._sequence);
    EXPECT_GT(redelivered._token._issuance, first._token._issuance);
    require_value(reopened.acknowledge(redelivered._token));
    require_value(moved.acknowledge(second._token));
  }

  TEST_F(SourceRuntimeTest, AdditionalActiveMembersBlockFreshSelectionButDoNotInvalidateAnExistingPin)
  {
    auto scenario = support::create_source_scenario(_directory);
    auto writer = require_value(scenario._created._creator.open_writer(scenario._queue));
    support::send_source_value(scenario, writer, 1);
    auto consumer = require_value(scenario._created._session.attach_task(scenario._consumer));
    auto source = require_value(consumer.open_source_set(scenario._source));
    const auto active = receive_delivery(source);
    QueueDescriptor second;
    second._name = "SecondUpdates";
    second._producer = scenario._created._creator.claim()._task;
    second._sourceSet = scenario._source;
    const auto id = require_value(scenario._created._creator.registry().register_queue(second));
    // A Draft member is not eligible. Once Active, it affects fresh selection
    // only; the in-flight token remains usable for repeat and acknowledgment.
    EXPECT_EQ(receive_delivery(source)._token, active._token);
    require_value(scenario._created._creator.registry().activate_queue(id));
    EXPECT_EQ(receive_delivery(source)._token, active._token);
    require_value(source.acknowledge(active._token));
    const auto before = support::recovered_image(_directory);
    expect_receive_error(source.receive(), ErrorCode::UnsupportedPolicy);
    EXPECT_EQ(support::recovered_image(_directory), before);
    require_value(source.close());
  }

  TEST_F(SourceRuntimeTest, DraftMembersDoNotBecomeFreshReceiveCandidates)
  {
    auto scenario = support::create_source_scenario(_directory, false);
    auto consumer = require_value(scenario._created._session.attach_task(scenario._consumer));
    auto source = require_value(consumer.open_source_set(scenario._source));
    const auto before = support::recovered_image(_directory);
    EXPECT_TRUE(std::holds_alternative<ReceiveTimeout>(source.receive()));
    EXPECT_TRUE(std::holds_alternative<ReceiveTimeout>(source.receive(2ms)));
    EXPECT_EQ(support::recovered_image(_directory), before);
  }

  TEST_F(SourceRuntimeTest, NegativeAndOverflowingTimeoutsRejectWithoutIssuingAMessage)
  {
    auto scenario = support::create_source_scenario(_directory);
    auto writer = require_value(scenario._created._creator.open_writer(scenario._queue));
    support::send_source_value(scenario, writer, 1);
    auto consumer = require_value(scenario._created._session.attach_task(scenario._consumer));
    auto source = require_value(consumer.open_source_set(scenario._source));
    const auto before = support::recovered_image(_directory);
    expect_receive_error(source.receive(-1ns), ErrorCode::InvalidArgument);
    expect_receive_error(source.receive(std::chrono::nanoseconds::max()), ErrorCode::InvalidArgument);
    EXPECT_EQ(support::recovered_image(_directory), before);
    const auto delivery = receive_delivery(source);
    EXPECT_EQ(delivery._token._issuance, 1u);
    require_value(source.acknowledge(delivery._token));
  }

  TEST_F(SourceRuntimeTest, ZeroAndFiniteWaitsDoNotBlockBehindAnotherGateOwner)
  {
    auto scenario = support::create_source_scenario(_directory);
    auto writer = require_value(scenario._created._creator.open_writer(scenario._queue));
    support::send_source_value(scenario, writer, 1);
    auto consumer = require_value(scenario._created._session.attach_task(scenario._consumer));
    auto source = require_value(consumer.open_source_set(scenario._source));
    auto controller = require_value(mapped::SessionController::open_existing(_directory));
    std::latch held(1);
    std::latch release(1);
    std::optional<Error> holder_error;
    std::thread holder([&]
    {
      auto result = controller->acquire_mutation();
      if (const auto* error = std::get_if<Error>(&result))
      {
        holder_error = *error;
        held.count_down();
        return;
      }
      auto guard = std::get<mapped::MutationGuard>(std::move(result));
      held.count_down();
      release.wait();
    });
    ReceiveResult immediate{ReceiveTimeout{}};
    ReceiveResult finite{ReceiveTimeout{}};
    std::chrono::steady_clock::duration elapsed{};
    try
    {
      held.wait();
      const auto start = std::chrono::steady_clock::now();
      immediate = source.receive();
      finite = source.receive(20ms);
      elapsed = std::chrono::steady_clock::now() - start;
    }
    catch (...)
    {
      release.count_down();
      holder.join();
      throw;
    }
    release.count_down();
    holder.join();
    ASSERT_FALSE(holder_error);
    EXPECT_TRUE(std::holds_alternative<ReceiveTimeout>(immediate));
    EXPECT_TRUE(std::holds_alternative<ReceiveTimeout>(finite));
    EXPECT_LT(elapsed, 2s); // Coarse no-hang bound, not a scheduler latency claim.
    const auto available = receive_delivery(source);
    EXPECT_EQ(available._token._issuance, 1u);
    require_value(source.acknowledge(available._token));
  }

  TEST_F(SourceRuntimeTest, ConcurrentPublicationRemainsDeliverableWhenAPositiveWaitTimesOut)
  {
    // This private observer only confirms that public receive has begun reading
    // the empty Session. It neither fabricates a delivery nor holds the gate to
    // direct the producer's scheduling.
    class ObserveFirstRead final : public runtime::RuntimeAccessReadObserver
    {
    public:
      OperationResult before_read(std::uint64_t, std::uint64_t) override
      {
        {
          std::lock_guard lock{_mutex};
          _observed = true;
        }
        _condition.notify_one();
        return std::monostate{};
      }

      bool wait_for_read()
      {
        std::unique_lock lock{_mutex};
        return _condition.wait_for(lock, 2s, [&]
        {
          return _observed;
        });
      }

    private:
      std::mutex _mutex;
      std::condition_variable _condition;
      bool _observed{};
    };

    auto scenario = support::create_source_scenario(_directory);
    auto writer = require_value(scenario._created._creator.open_writer(scenario._queue));
    auto consumer = require_value(scenario._created._session.attach_task(scenario._consumer));
    // Prepare before the reader starts so only the actual public send contends
    // with it, rather than ID issuance and schema/encoding setup as well.
    const auto bytes = require_value(support::source_codec(scenario).encode({std::uint32_t{99}}));
    const auto publication = require_value(scenario._created._creator.next_publication_id());
    ObserveFirstRead observer;
    runtime::SourceOperationHooks hooks;
    hooks._reads = &observer;
    auto source = require_value(consumer.open_source_set(scenario._source));
    runtime::SourceAccess::set_hooks(source, &hooks);
    std::optional<ReceiveResult> received;
    std::exception_ptr reader_error;
    std::thread reader([&]
    {
      try
      {
        received.emplace(source.receive(2s));
      }
      catch (...)
      {
        reader_error = std::current_exception();
      }
    });
    bool observed{};
    try
    {
      observed = observer.wait_for_read();
      if (observed)
      {
        const auto outcome = require_value(writer.send(publication,
            support::source_message(scenario._schema, bytes)));
        if (outcome._status != AcceptanceStatus::Accepted)
        {
          throw std::runtime_error("Expected the concurrent publication to be accepted");
        }
      }
    }
    catch (...)
    {
      // The receiver has its own finite deadline, so even an unsuccessful
      // producer must join it before destroying the shared Source handle.
      reader.join();
      runtime::SourceAccess::set_hooks(source, nullptr);
      throw;
    }
    reader.join();
    runtime::SourceAccess::set_hooks(source, nullptr);
    if (reader_error)
    {
      std::rethrow_exception(reader_error);
    }
    ASSERT_TRUE(observed) << "The receiver did not begin reading the empty Session";
    ASSERT_TRUE(received);
    // The mutation gate is released between checks, but it promises no fair
    // acquisition order. The producer may acquire it only after the finite
    // receive deadline. A timeout must leave that accepted message available;
    // it must not lose the publication or consume an issuance.
    if (std::holds_alternative<ReceiveTimeout>(*received))
    {
      received.emplace(source.receive());
    }
    ASSERT_TRUE(std::holds_alternative<Delivery>(*received));
    const auto delivery = std::get<Delivery>(*received);
    EXPECT_EQ(delivery._message._header._publication, publication);
    EXPECT_EQ(delivery._message._header._queueSequence, 1u);
    EXPECT_EQ(delivery._token._sequence, 1u);
    EXPECT_EQ(delivery._token._issuance, 1u);
    require_value(source.acknowledge(delivery._token));
    EXPECT_TRUE(std::holds_alternative<ReceiveTimeout>(source.receive()));
  }

  TEST_F(SourceRuntimeTest, ReservedReceiveReleaseAcknowledgeAndCloseStillWorkAtAnExactlyFullLog)
  {
    auto scenario = support::create_source_scenario(_directory);
    auto writer = require_value(scenario._created._creator.open_writer(scenario._queue));
    support::send_source_value(scenario, writer, 1);
    auto consumer = require_value(scenario._created._session.attach_task(scenario._consumer));
    auto source = require_value(consumer.open_source_set(scenario._source));
    // A private valid Queue-doc revision fills the remaining append budget.
    // Message creation, receiving, and all subsequent progress stay public.
    support::fill_log_with_queue_revision(support::open_runtime_state(_directory), scenario._queue,
                                            scenario._created._creator.claim());
    const auto full = support::journal_snapshot(_directory);
    EXPECT_EQ(full._tail, 2u * 1024 * 1024);
    const auto next_id = require_value(scenario._created._creator.next_publication_id());
    const auto bytes = require_value(support::source_codec(scenario).encode({std::uint32_t{2}}));
    expect_error(writer.send(next_id, support::source_message(scenario._schema, bytes)), ErrorCode::LogFull);
    const auto first = receive_delivery(source);
    require_value(source.release(first._token));
    const auto second = receive_delivery(source);
    EXPECT_GT(second._token._issuance, first._token._issuance);
    require_value(source.acknowledge(second._token));
    require_value(source.close());
    const auto after = support::journal_snapshot(_directory);
    EXPECT_EQ(after._tail, full._tail);
    EXPECT_EQ(after._fileBytes, full._fileBytes);
  }

  TEST_F(SourceRuntimeTest, EmptyAndOpaquePayloadsCarryBoundHeadersWithoutBinaryInterpretation)
  {
    auto scenario = support::create_source_scenario(_directory);
    auto writer = require_value(scenario._created._creator.open_writer(scenario._queue));
    auto consumer = require_value(scenario._created._session.attach_task(scenario._consumer));
    auto source = require_value(consumer.open_source_set(scenario._source));
    for (const auto& bytes : {std::vector<std::byte>{}, support::octets({0xff, 0, '<', '{'})})
    {
      const auto publication = require_value(scenario._created._creator.next_publication_id());
      require_value(writer.send(publication, support::source_message(scenario._schema, bytes, PayloadFormat::Text)));
      const auto delivery = receive_delivery(source);
      EXPECT_EQ(delivery._message._header._payloadBytes, bytes.size());
      EXPECT_EQ(delivery._message._header._publication, publication);
      EXPECT_EQ(payload_bytes(delivery._message), bytes);
      const auto saved = require_value(delivery._message.copy_message(bytes.size()));
      EXPECT_EQ(saved._header, delivery._message._header);
      EXPECT_EQ(saved._payload, bytes);
      expect_error(saved.decode(support::source_codec(scenario)), ErrorCode::UnsupportedPolicy);
      require_value(source.acknowledge(delivery._token));
    }
  }

  TEST_F(SourceRuntimeTest, ClosingTheLastSourceOwnerReleasesItsRetainedTaskClaim)
  {
    auto scenario = support::create_source_scenario(_directory);
    auto source = [&]
    {
      auto consumer = require_value(scenario._created._session.attach_task(scenario._consumer));
      return require_value(consumer.open_source_set(scenario._source));
    }();
    expect_error(scenario._created._session.attach_task(scenario._consumer), ErrorCode::OwnershipBusy);
    require_value(source.close());
    // close() must drop the final Task owner after releasing the operation
    // gate; otherwise implicit detachment would recursively acquire that gate.
    auto successor = require_value(scenario._created._session.attach_task(scenario._consumer));
    EXPECT_EQ(successor.claim()._incarnation, 2u);
  }

  TEST_F(SourceRuntimeTest, ThreadOwnershipRejectionLeavesTheSourceActiveAndItsPinUnchanged)
  {
    auto scenario = support::create_source_scenario(_directory, true, TaskKind::Thread);
    auto writer = require_value(scenario._created._creator.open_writer(scenario._queue));
    support::send_source_value(scenario, writer, 1);
    auto consumer = require_value(scenario._created._session.attach_task(scenario._consumer));
    auto source = require_value(consumer.open_source_set(scenario._source));
    const auto delivery = receive_delivery(source);
    const auto before = support::recovered_image(_directory);
    std::thread stranger([&]
    {
      expect_receive_error(source.receive(), ErrorCode::WrongOwner);
      expect_error(source.acknowledge(delivery._token), ErrorCode::WrongOwner);
      expect_error(source.release(delivery._token), ErrorCode::WrongOwner);
      expect_error(source.close(), ErrorCode::WrongOwner);
    });
    stranger.join();
    EXPECT_EQ(support::recovered_image(_directory), before);
    EXPECT_EQ(receive_delivery(source)._message._payload.data(), delivery._message._payload.data());
    require_value(source.acknowledge(delivery._token));
  }

  TEST_F(SourceRuntimeTest, InheritedSourceRejectsBeforeAccessingTheParentsGateOrMutexes)
  {
    auto scenario = support::create_source_scenario(_directory);
    auto writer = require_value(scenario._created._creator.open_writer(scenario._queue));
    support::send_source_value(scenario, writer, 1);
    auto consumer = require_value(scenario._created._session.attach_task(scenario._consumer));
    auto source = require_value(consumer.open_source_set(scenario._source));
    const auto delivery = receive_delivery(source);
    const auto before = support::recovered_image(_directory);
    auto pid = ::fork();
    ASSERT_NE(pid, -1);
    if (pid == 0)
    {
      const auto wrong = [](const auto& result)
      {
        const auto* error = std::get_if<Error>(&result);
        return error && error->_code == ErrorCode::WrongOwner;
      };
      const auto valid = wrong(source.receive()) && wrong(source.acknowledge(delivery._token)) &&
          wrong(source.release(delivery._token)) && wrong(source.close());
      ::_exit(valid ? 0 : 1);
    }
    int status{};
    try
    {
      status = support::wait_for_exit(pid, "Cannot reap inherited Source check",
                                         "Inherited Source ownership check did not finish");
      pid = -1;
    }
    catch (...)
    {
      support::terminate(pid, {"Cannot stop inherited Source check", "Cannot reap inherited Source check",
                               "Inherited Source termination timed out", "Inherited Source was not killed"});
      throw;
    }
    EXPECT_EQ(support::exit_code(status), 0);
    EXPECT_EQ(support::recovered_image(_directory), before);
    require_value(source.acknowledge(delivery._token));
  }

  TEST_F(SourceRuntimeTest, LegacyProfilesRejectDeliveryButAllowIdleSourceCloseWithoutDeliveryWrites)
  {
    for (const auto minor : {std::uint16_t{0}, std::uint16_t{1}})
    {
      const auto directory = _root / ("legacy-" + std::to_string(minor));
      SourceQueueSetId source_id;
      QueueId queue_id;
      {
        auto scenario = support::create_source_scenario(directory);
        source_id = scenario._source;
        queue_id = scenario._queue;
      }
      // Deliberately install a supported predecessor profile only after every
      // public owner closes and before any Publication exists. This is a test
      // fixture, never an application migration or profile-upgrade operation.
      {
        auto file = require_value(mapped::NativeFile::open_existing(directory / "session.ang",
                                                                     mapped::FileAccess::ReadWrite));
        const std::array<std::byte, 2> format{static_cast<std::byte>(minor)};
        const std::array<std::byte, 8> features{minor == 0 ? std::byte{1} : std::byte{3}};
        require_value(file.write_exact(10, format));
        require_value(file.write_exact(56, features));
      }
      auto session = require_value(Session::attach(directory));
      auto consumer = require_value(session.attach_task("Consumer"));
      const auto token = DeliveryToken{consumer.claim(), source_id, queue_id, 1, 1};
      auto source = require_value(consumer.open_source_set(source_id));
      const auto before = support::recovered_image(directory);
      expect_receive_error(source.receive(), ErrorCode::UnsupportedPolicy);
      expect_error(source.acknowledge(token), ErrorCode::UnsupportedPolicy);
      expect_error(source.release(token), ErrorCode::UnsupportedPolicy);
      require_value(source.close());
      EXPECT_EQ(support::recovered_image(directory), before);
    }
  }

  // These hooks inject local preparation and journal failures after normal
  // public setup. They exercise the production Source path without presenting
  // fault injection as part of the application API.
  class SourceJournalFailure final : public mapped::JournalFaultInjector
  {
  public:
    SourceJournalFailure(mapped::JournalCheckpoint point, bool persistent = true)
        : _point(point), _persistent(persistent)
    {
    }

    OperationResult checkpoint(mapped::JournalCheckpoint point, std::uint64_t index) override
    {
      if (_enabled && point == _point && index == 0 && (_persistent || _failures == 0))
      {
        ++_failures;
        return Error{ErrorCode::IoError, "Injected Source journal failure", "test.source_journal"};
      }
      return std::monostate{};
    }

    bool _enabled{true};
    unsigned _failures{};

  private:
    mapped::JournalCheckpoint _point;
    bool _persistent;
  };

  class PreparationFailure final : public runtime::SourceOperationHooks
  {
  public:
    explicit PreparationFailure(bool binding, SourceJournalFailure* cleanupFailure = nullptr)
        : _binding(binding), _cleanupFailure(cleanupFailure)
    {
    }

    OperationResult before_mapping(const runtime::DeliveryProgress&) override
    {
      return _binding ? OperationResult{std::monostate{}} : fail_once();
    }

    OperationResult before_binding(const runtime::DeliveryProgress&) override
    {
      return _binding ? fail_once() : OperationResult{std::monostate{}};
    }

    unsigned _failures{};

  private:
    OperationResult fail_once()
    {
      if (_failures++ == 0)
      {
        if (_cleanupFailure)
        {
          _cleanupFailure->_enabled = true;
        }
        return Error{ErrorCode::IoError, "Injected local message preparation failure", "test.source_preparation"};
      }
      return std::monostate{};
    }

    bool _binding;
    SourceJournalFailure* _cleanupFailure;
  };

  TEST_F(SourceRuntimeTest, MappingOrBindingFailureReturnsTheUnexposedDeliveryToPending)
  {
    for (const auto binding : {false, true})
    {
      const auto directory = _root / (binding ? "binding" : "mapping");
      auto scenario = support::create_source_scenario(directory);
      auto writer = require_value(scenario._created._creator.open_writer(scenario._queue));
      support::send_source_value(scenario, writer, 1);
      auto consumer = require_value(scenario._created._session.attach_task(scenario._consumer));
      PreparationFailure failure(binding);
      auto source = require_value(consumer.open_source_set(scenario._source));
      runtime::SourceAccess::set_hooks(source, &failure);
      expect_receive_error(source.receive(), ErrorCode::IoError);
      const auto delivery = receive_delivery(source);
      EXPECT_EQ(delivery._token._sequence, 1u);
      EXPECT_EQ(delivery._token._issuance, 2u);
      require_value(source.acknowledge(delivery._token));
      runtime::SourceAccess::set_hooks(source, nullptr);
    }
  }

  TEST_F(SourceRuntimeTest, KnownUnchangedAcknowledgmentFailurePreservesTheCurrentBorrow)
  {
    auto scenario = support::create_source_scenario(_directory);
    auto writer = require_value(scenario._created._creator.open_writer(scenario._queue));
    support::send_source_value(scenario, writer, 1);
    auto consumer = require_value(scenario._created._session.attach_task(scenario._consumer));
    SourceJournalFailure failure(mapped::JournalCheckpoint::Prepared, false);
    runtime::SourceOperationHooks hooks;
    hooks._journal = &failure;
    auto source = require_value(consumer.open_source_set(scenario._source));
    const auto delivery = receive_delivery(source);
    const auto before_snapshot = support::journal_snapshot(_directory);
    const auto before = support::recovered_image(_directory);
    runtime::SourceAccess::set_hooks(source, &hooks);
    expect_error(source.acknowledge(delivery._token), ErrorCode::IoError);
    runtime::SourceAccess::set_hooks(source, nullptr);
    const auto after = support::recovered_image(_directory);
    EXPECT_EQ(support::journal_snapshot(_directory), before_snapshot);
    ASSERT_EQ(after.size(), before.size());
    // Prepared rollback may leave the attempted inline intent as journal
    // scratch. Its Idle control word and every word outside that scratch must
    // match, including all receipt, Source, Queue, allocation, and payload state.
    constexpr auto control = mapped::journal_offset / sizeof(std::uint64_t);
    constexpr auto scratch_end = (mapped::journal_offset + mapped::journal_bytes) / sizeof(std::uint64_t);
    EXPECT_EQ(after[control], static_cast<std::uint64_t>(mapped::JournalControl::Idle));
    for (std::size_t word = 0; word != before.size(); ++word)
    {
      if (word > control && word < scratch_end)
      {
        continue;
      }
      EXPECT_EQ(after[word], before[word]) << "Native word offset " << word * sizeof(std::uint64_t);
    }
    const auto repeat = receive_delivery(source);
    EXPECT_EQ(repeat._token, delivery._token);
    EXPECT_EQ(repeat._message._payload.data(), delivery._message._payload.data());
    EXPECT_EQ(payload_bytes(repeat._message), payload_bytes(delivery._message));
    require_value(source.acknowledge(delivery._token));
  }

  TEST_F(SourceRuntimeTest, UncertainAcknowledgmentInvalidatesTheBorrowAndCleanRecoverySettlesIt)
  {
    auto scenario = support::create_source_scenario(_directory);
    auto writer = require_value(scenario._created._creator.open_writer(scenario._queue));
    support::send_source_value(scenario, writer, 1);
    auto consumer = require_value(scenario._created._session.attach_task(scenario._consumer));
    SourceJournalFailure failure(mapped::JournalCheckpoint::RedoWord);
    runtime::SourceOperationHooks hooks;
    hooks._journal = &failure;
    auto source = require_value(consumer.open_source_set(scenario._source));
    const auto old_token = receive_delivery(source)._token;
    runtime::SourceAccess::set_hooks(source, &hooks);
    expect_error(source.acknowledge(old_token), ErrorCode::OutcomeUnknown);
    // Do not inspect an old MessageView after an uncertain progress result.
    // A clean operation first recovers the journal; acknowledgment has committed.
    runtime::SourceAccess::set_hooks(source, nullptr);
    EXPECT_TRUE(std::holds_alternative<ReceiveTimeout>(source.receive()));
    expect_error(source.release(old_token), ErrorCode::StaleDelivery);
    require_value(source.close());
  }

  TEST_F(SourceRuntimeTest, FailedExplicitCloseFencesTheHandleUntilCleanupIsRetried)
  {
    auto scenario = support::create_source_scenario(_directory);
    auto writer = require_value(scenario._created._creator.open_writer(scenario._queue));
    support::send_source_value(scenario, writer, 1);
    auto consumer = require_value(scenario._created._session.attach_task(scenario._consumer));
    SourceJournalFailure failure(mapped::JournalCheckpoint::RedoWord);
    runtime::SourceOperationHooks hooks;
    hooks._journal = &failure;
    auto source = require_value(consumer.open_source_set(scenario._source));
    const auto old = receive_delivery(source)._token;
    runtime::SourceAccess::set_hooks(source, &hooks);
    expect_error(source.close(), ErrorCode::OutcomeUnknown);
    expect_receive_error(source.receive(), ErrorCode::InvalidState);
    expect_error(source.acknowledge(old), ErrorCode::InvalidState);
    expect_error(source.release(old), ErrorCode::InvalidState);
    expect_error(consumer.open_source_set(scenario._source), ErrorCode::OwnershipBusy);
    runtime::SourceAccess::set_hooks(source, nullptr);
    require_value(source.close());
    auto reopened = require_value(consumer.open_source_set(scenario._source));
    const auto next = receive_delivery(reopened);
    EXPECT_GT(next._token._issuance, old._issuance);
    require_value(reopened.acknowledge(next._token));
  }

  TEST_F(SourceRuntimeTest, DestructionDefersFailedCleanupAndReopeningReconcilesTheExactOldPin)
  {
    auto scenario = support::create_source_scenario(_directory);
    auto writer = require_value(scenario._created._creator.open_writer(scenario._queue));
    support::send_source_value(scenario, writer, 1);
    auto consumer = require_value(scenario._created._session.attach_task(scenario._consumer));
    SourceJournalFailure failure(mapped::JournalCheckpoint::RedoWord);
    runtime::SourceOperationHooks hooks;
    hooks._journal = &failure;
    std::shared_ptr<runtime::SourceReservation> marker;
    DeliveryToken old;
    {
      auto source = require_value(consumer.open_source_set(scenario._source));
      old = receive_delivery(source)._token;
      marker = runtime::SourceAccess::state(source)->_reservation;
      runtime::SourceAccess::set_hooks(source, &hooks);
    }
    EXPECT_EQ(marker->_phase.load(), runtime::SourcePhase::DeferredClose);
    // The deferred marker owns no abandoned handle or payload mapping. A new
    // public opening must recover and finish that exact old cleanup first.
    auto reopened = require_value(consumer.open_source_set(scenario._source));
    EXPECT_EQ(marker->_phase.load(), runtime::SourcePhase::Closed);
    const auto next = receive_delivery(reopened);
    EXPECT_GT(next._token._issuance, old._issuance);
    expect_error(reopened.acknowledge(old), ErrorCode::StaleDelivery);
    require_value(reopened.acknowledge(next._token));
  }

  TEST_F(SourceRuntimeTest, UnreturnedIssuanceRemainsDiscoverableByCloseAfterPreparationCleanupFails)
  {
    auto scenario = support::create_source_scenario(_directory);
    auto writer = require_value(scenario._created._creator.open_writer(scenario._queue));
    support::send_source_value(scenario, writer, 1);
    auto consumer = require_value(scenario._created._session.attach_task(scenario._consumer));
    SourceJournalFailure failure(mapped::JournalCheckpoint::RedoWord);
    failure._enabled = false;
    PreparationFailure hooks(true, &failure);
    hooks._journal = &failure;
    auto source = require_value(consumer.open_source_set(scenario._source));
    runtime::SourceAccess::set_hooks(source, &hooks);
    expect_receive_error(source.receive(), ErrorCode::OutcomeUnknown);
    // No Delivery reached the application. Close must use the separately saved
    // issuance identity, rather than relying on a returned view or token.
    runtime::SourceAccess::set_hooks(source, nullptr);
    require_value(source.close());
    auto reopened = require_value(consumer.open_source_set(scenario._source));
    const auto next = receive_delivery(reopened);
    EXPECT_EQ(next._token._sequence, 1u);
    EXPECT_EQ(next._token._issuance, 2u);
    require_value(reopened.acknowledge(next._token));
  }

  TEST_F(SourceRuntimeTest, DeferredMarkersDoNotKeepTheLastConsumerTaskOwnerAlive)
  {
    auto scenario = support::create_source_scenario(_directory);
    auto writer = require_value(scenario._created._creator.open_writer(scenario._queue));
    support::send_source_value(scenario, writer, 1);
    SourceJournalFailure failure(mapped::JournalCheckpoint::RedoWord);
    runtime::SourceOperationHooks hooks;
    hooks._journal = &failure;
    std::weak_ptr<runtime::TaskState> old_task;
    std::shared_ptr<runtime::SourceReservation> marker;
    {
      auto consumer = require_value(scenario._created._session.attach_task(scenario._consumer));
      auto source = require_value(consumer.open_source_set(scenario._source));
      (void)receive_delivery(source);
      old_task = runtime::SourceAccess::state(source)->_task;
      marker = runtime::SourceAccess::state(source)->_reservation;
      runtime::SourceAccess::set_hooks(source, &hooks);
    }
    EXPECT_TRUE(old_task.expired());
    // Replacement's claim cleanup settles any old pin even if the last local
    // owner disappeared with a deferred reservation. The marker itself owns no
    // Task and cannot form a lease-retaining reference cycle.
    auto replacement = require_value(scenario._created._session.attach_task(scenario._consumer));
    auto source = require_value(replacement.open_source_set(scenario._source));
    const auto delivery = receive_delivery(source);
    EXPECT_EQ(delivery._token._consumer, replacement.claim());
    require_value(source.acknowledge(delivery._token));
  }
} // namespace
