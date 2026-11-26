// Focused lifecycle failure qualification. Public setup/receive establish real
// claims and pins. Private fault hooks and deliberate successor-pin setup test
// exact cleanup fences; they are not application APIs or normal usage patterns.
#include "source_fixture.h"

#include "native_fixture.h"
#include "result_assertions.h"
#include "source_runtime_state.h"

#include <gtest/gtest.h>

#include <array>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <system_error>
#include <variant>

namespace
{
  using namespace angaria;
  namespace runtime = angaria::detail;
  namespace mapped = runtime::mapped;
  namespace support = angaria::test_support;
  namespace fs = std::filesystem;
  using support::expect_error;
  using support::require_value;

  class PersistentJournalFailure final : public mapped::JournalFaultInjector
  {
  public:
    explicit PersistentJournalFailure(mapped::JournalCheckpoint point) : _point(point)
    {
    }

    OperationResult checkpoint(mapped::JournalCheckpoint point, std::uint64_t index) override
    {
      if (point == _point && index == 0)
      {
        return Error{ErrorCode::IoError, "Injected lifecycle journal failure", "test.source_lifecycle"};
      }
      return std::monostate{};
    }

  private:
    mapped::JournalCheckpoint _point;
  };

  class SourceLifecycleTest : public testing::Test
  {
  protected:
    void SetUp() override
    {
      auto pattern = (fs::temp_directory_path() / "angaria-source-lifecycle-XXXXXX").string();
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

  TEST_F(SourceLifecycleTest, DeferredOldIdentityCannotClearANewerPinOrBypassTaskProgressFences)
  {
    auto scenario = support::create_source_scenario(_directory);
    auto producer_writer = require_value(scenario._created._creator.open_writer(scenario._queue));
    support::send_source_value(scenario, producer_writer, 42);
    auto consumer = require_value(scenario._created._session.attach_task(scenario._consumer));

    // Give the same consumer Task an output too. Deferred cleanup must fence
    // fresh acceptance while keeping recorded outcomes available to diagnostics.
    auto registry = scenario._created._creator.registry();
    SourceQueueSetDescriptor destination;
    destination._name = "ProducerInputs";
    destination._owner = scenario._created._creator.claim()._task;
    const auto destination_id = require_value(registry.register_source_set(destination));
    QueueDescriptor output;
    output._name = "ConsumerOutput";
    output._producer = scenario._consumer;
    output._sourceSet = destination_id;
    const auto output_id = require_value(registry.register_queue(output));
    require_value(registry.activate_source_set(destination_id));
    require_value(registry.activate_queue(output_id));
    auto consumer_writer = require_value(consumer.open_writer(output_id));
    const auto accepted = require_value(consumer.next_publication_id());
    const auto fresh = require_value(consumer.next_publication_id());
    const auto bytes = require_value(support::source_codec(scenario).encode({std::uint32_t{17}}));
    const auto message = support::source_message(scenario._schema, bytes);
    ASSERT_EQ(require_value(consumer_writer.send(accepted, message))._status, AcceptanceStatus::Accepted);

    PersistentJournalFailure failure(mapped::JournalCheckpoint::JournalBodyWord);
    runtime::SourceOperationHooks hooks;
    hooks._journal = &failure;
    std::shared_ptr<runtime::TaskState> task;
    std::shared_ptr<runtime::SourceReservation> marker;
    DeliveryToken original;
    {
      auto source = require_value(consumer.open_source_set(scenario._source));
      const auto received = source.receive();
      ASSERT_TRUE(std::holds_alternative<Delivery>(received));
      original = std::get<Delivery>(received)._token;
      const auto state = runtime::SourceAccess::state(source);
      task = runtime::runtime_source_task(state);
      marker = state->_reservation;
      runtime::SourceAccess::set_hooks(source, &hooks);
    }
    ASSERT_EQ(marker->_phase.load(), runtime::SourcePhase::DeferredClose);
    ASSERT_EQ(marker->_token, original);

    DeliveryToken newer;
    {
      // Deliberately bypass the public reservation to install a newer pin.
      // Cleanup must fail closed even for this unexpected private test state.
      runtime::RuntimeAccess access(task->_session);
      require_value(runtime::runtime_release_delivery(access, original));
      newer = require_value(runtime::runtime_issue_delivery(access, task->_claim,
          scenario._source, scenario._queue, UtcTimestamp{std::chrono::nanoseconds{7}}))._token;
    }
    ASSERT_GT(newer._issuance, original._issuance);
    const auto settled = support::recovered_image(_directory);
    // Lookup and an identical repeat create no delivery progress. They remain
    // usable while cleanup cannot settle; neither may clear the newer pin or
    // need a fresh operation generation solely to report a recorded outcome.
    EXPECT_EQ(require_value(consumer_writer.send(accepted, message))._status, AcceptanceStatus::Accepted);
    EXPECT_EQ(require_value(consumer.publication_status(accepted))._status, AcceptanceStatus::Accepted);
    EXPECT_EQ(require_value(consumer.publication_status(fresh))._status, AcceptanceStatus::Rejected);
    std::array<DestinationOutcome, 1> outcomes;
    const auto repeated = consumer_writer.send(accepted, message, outcomes);
    EXPECT_EQ(repeated._error, ErrorCode::None);
    EXPECT_EQ(repeated._status, PublicationStatus::Accepted);
    const auto status = consumer.publication_status(accepted, outcomes);
    EXPECT_EQ(status._error, ErrorCode::None);
    EXPECT_EQ(status._status, PublicationStatus::Accepted);
    expect_error(consumer_writer.send(fresh, message), ErrorCode::StaleDelivery);
    EXPECT_EQ(consumer_writer.send(fresh, message, outcomes)._error, ErrorCode::StaleDelivery);
    expect_error(consumer.open_source_set(scenario._source), ErrorCode::StaleDelivery);
    EXPECT_EQ(support::recovered_image(_directory), settled);
    EXPECT_EQ(marker->_phase.load(), runtime::SourcePhase::DeferredClose);

    {
      runtime::RuntimeAccess access(task->_session);
      const auto current = require_value(runtime::runtime_issue_delivery(access, task->_claim,
          scenario._source, QueueId::Invalid, UtcTimestamp{}));
      EXPECT_EQ(current._token, newer);
      require_value(runtime::runtime_release_delivery(access, newer));
    }
    // Once the newer borrow ends independently, Source opening retires the old
    // marker without touching any issuance. The failed output ID stays unused.
    auto reopened = require_value(consumer.open_source_set(scenario._source));
    const auto absent = require_value(consumer.publication_status(fresh));
    EXPECT_EQ(absent._status, AcceptanceStatus::Rejected);
    EXPECT_TRUE(absent._destinations.empty());
    EXPECT_EQ(marker->_phase.load(), runtime::SourcePhase::Closed);
  }

  TEST_F(SourceLifecycleTest, CloseDiscoversAnExactIssuanceWhoseTokenWasNeverReturned)
  {
    auto scenario = support::create_source_scenario(_directory);
    auto writer = require_value(scenario._created._creator.open_writer(scenario._queue));
    const auto publication = support::send_source_value(scenario, writer, 9);
    auto consumer = require_value(scenario._created._session.attach_task(scenario._consumer));
    PersistentJournalFailure failure(mapped::JournalCheckpoint::RedoWord);
    runtime::SourceOperationHooks hooks;
    hooks._journal = &failure;
    auto source = require_value(consumer.open_source_set(scenario._source));
    const auto reservation = runtime::SourceAccess::state(source)->_reservation;
    runtime::SourceAccess::set_hooks(source, &hooks);
    const auto failed = source.receive();
    ASSERT_TRUE(std::holds_alternative<Error>(failed));
    EXPECT_EQ(std::get<Error>(failed)._code, ErrorCode::OutcomeUnknown);
    EXPECT_FALSE(reservation->_token);
    ASSERT_TRUE(reservation->_pendingIssuance);
    EXPECT_EQ(*reservation->_pendingIssuance, 1u);

    // The journal committed, but never returned DeliveryProgress to receive.
    // Close must recover it and release only the saved expected issuance.
    runtime::SourceAccess::set_hooks(source, nullptr);
    require_value(source.close());
    auto reopened = require_value(consumer.open_source_set(scenario._source));
    const auto received = reopened.receive();
    ASSERT_TRUE(std::holds_alternative<Delivery>(received));
    const auto delivery = std::get<Delivery>(received);
    EXPECT_EQ(delivery._message._header._publication, publication);
    EXPECT_EQ(delivery._token._sequence, 1u);
    EXPECT_EQ(delivery._token._issuance, 2u);
    require_value(reopened.acknowledge(delivery._token));
  }
} // namespace
