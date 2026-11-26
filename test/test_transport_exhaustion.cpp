// Boundary compositions through the public transport API. The private counter
// writes move a valid idle claim near UINT64_MAX; reaching that state by running
// an application would be impractical. They are qualification seams, not setup
// options. Every message and terminal transition still uses ordinary APIs.
#include "source_fixture.h"

#include "native_fixture.h"
#include "native_log_budget.h"
#include "result_assertions.h"
#include "topology_state.h"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <limits>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>
#include <variant>
#include <vector>

namespace
{
  using namespace angaria;
  namespace runtime = angaria::detail;
  namespace support = angaria::test_support;
  namespace fs = std::filesystem;
  using support::expect_error;
  using support::require_value;
  constexpr auto final_counter = std::numeric_limits<std::uint64_t>::max();

  Delivery receive_delivery(SourceQueueSet& source)
  {
    auto result = source.receive();
    if (const auto* error = std::get_if<Error>(&result))
    {
      throw std::runtime_error(error->_path + ": " + error->_message);
    }
    if (!std::holds_alternative<Delivery>(result))
    {
      throw std::runtime_error("Expected a delivery at the counter boundary");
    }
    return std::get<Delivery>(std::move(result));
  }

  void expect_receive_error(const ReceiveResult& result, ErrorCode code)
  {
    ASSERT_TRUE(std::holds_alternative<Error>(result));
    const auto& error = std::get<Error>(result);
    EXPECT_EQ(error._code, code) << error._path << ": " << error._message;
  }

  class TransportExhaustionTest : public testing::Test
  {
  protected:
    void SetUp() override
    {
      auto pattern = (fs::temp_directory_path() / "angaria-transport-limits-XXXXXX").string();
      ASSERT_NE(::mkdtemp(pattern.data()), nullptr);
      _root = pattern;
      _directory = _root / "session";
    }

    void TearDown() override
    {
      std::error_code ignored;
      fs::remove_all(_root, ignored);
    }

    struct Offsets
    {
      std::uint64_t _source{};
      std::uint64_t _producer{};
    };

    Offsets offsets(TaskId producer) const
    {
      runtime::RuntimeAccess access(support::open_runtime_state(_directory));
      const auto topology = runtime::runtime_topology(access);
      const auto task = runtime::runtime_task(access, producer);
      return {topology._sources.at(0)._stateOffset, task._stateOffset};
    }

    void near_final_issuance(std::uint64_t source, const TaskClaim& consumer) const
    {
      // The Source is idle, so these two words describe a valid existing
      // issuance context without inventing a Queue sequence or receipt.
      support::set_atomic_word(_directory, source, final_counter - 1);
      support::set_atomic_word(_directory, source + 8, consumer._incarnation);
    }

    fs::path _root;
    fs::path _directory;
  };

  TEST_F(TransportExhaustionTest, FinalSourceIssuanceCanBeAcknowledgedAtFullLogAndReplacementResumes)
  {
    auto scenario = support::create_source_scenario(_directory);
    auto writer = require_value(scenario._created._creator.open_writer(scenario._queue));
    for (std::uint32_t value = 1; value != 4; ++value)
    {
      support::send_source_value(scenario, writer, value);
    }
    const auto producer = scenario._created._creator.claim();
    const auto state = support::open_runtime_state(_directory);
    const auto location = offsets(producer._task);
    DeliveryToken exhausted;
    {
      auto consumer = require_value(scenario._created._session.attach_task(scenario._consumer));
      auto source = require_value(consumer.open_source_set(scenario._source));
      near_final_issuance(location._source, consumer.claim());
      support::fill_log_with_queue_revision(state, scenario._queue, producer);
      const auto full = support::journal_snapshot(_directory);
      EXPECT_EQ(full._tail, require_value(scenario._created._session.options())._maxLogBytes);

      // Ring capacity two has already overwritten sequence one. Recovery
      // delivery still issues it with the last representable Source number.
      const auto first = receive_delivery(source);
      exhausted = first._token;
      EXPECT_EQ(exhausted._issuance, final_counter);
      EXPECT_EQ(exhausted._sequence, 1u);
      const auto pinned = support::recovered_image(_directory);
      const auto repeated = receive_delivery(source);
      EXPECT_EQ(repeated._token, exhausted);
      EXPECT_EQ(repeated._message._header, first._message._header);
      EXPECT_EQ(repeated._message._payload.data(), first._message._payload.data());
      EXPECT_EQ(support::recovered_image(_directory), pinned);

      require_value(source.acknowledge(exhausted));
      const auto completed = support::recovered_image(_directory);
      expect_receive_error(source.receive(), ErrorCode::CapacityExceeded);
      EXPECT_EQ(support::recovered_image(_directory), completed);
      require_value(source.close());
    }

    // A genuine successor claim resets only its own issuance context. The
    // old acknowledgment remains terminal and the old token stays stale.
    auto replacement = require_value(scenario._created._session.attach_task(scenario._consumer));
    EXPECT_GT(replacement.claim()._incarnation, exhausted._consumer._incarnation);
    auto source = require_value(replacement.open_source_set(scenario._source));
    const auto next = receive_delivery(source);
    EXPECT_EQ(next._token._sequence, 2u);
    EXPECT_EQ(next._token._issuance, 1u);
    const auto current = support::recovered_image(_directory);
    expect_error(source.acknowledge(exhausted), ErrorCode::StaleDelivery);
    EXPECT_EQ(support::recovered_image(_directory), current);
    require_value(source.acknowledge(next._token));
    const auto last = receive_delivery(source);
    EXPECT_EQ(last._token._sequence, 3u);
    require_value(source.acknowledge(last._token));
    EXPECT_TRUE(std::holds_alternative<ReceiveTimeout>(source.receive()));
    require_value(source.close());
    const auto settled = support::journal_snapshot(_directory);
    EXPECT_EQ(settled._tail, require_value(scenario._created._session.options())._maxLogBytes);
  }

  TEST_F(TransportExhaustionTest, FinalReleasedIssuanceCanBecomeLostAndReplacementStartsANewContext)
  {
    auto scenario = support::create_source_scenario(_directory, true,
        TaskKind::Process, DeliveryPolicy::AllowLoss);
    auto writer = require_value(scenario._created._creator.open_writer(scenario._queue));
    support::send_source_value(scenario, writer, 1);
    const auto producer = scenario._created._creator.claim();
    const auto location = offsets(producer._task);
    DeliveryToken exhausted;
    {
      auto consumer = require_value(scenario._created._session.attach_task(scenario._consumer));
      auto source = require_value(consumer.open_source_set(scenario._source));
      near_final_issuance(location._source, consumer.claim());
      const auto first = receive_delivery(source);
      exhausted = first._token;
      EXPECT_EQ(exhausted._issuance, final_counter);
      for (std::uint32_t value = 2; value != 5; ++value)
      {
        support::send_source_value(scenario, writer, value);
      }
      support::fill_log_with_queue_revision(support::open_runtime_state(_directory), scenario._queue, producer);
      const auto pinned = support::recovered_image(_directory);
      const auto repeated = receive_delivery(source);
      EXPECT_EQ(repeated._token, exhausted);
      EXPECT_EQ(repeated._message._header, first._message._header);
      EXPECT_EQ(repeated._message._payload.data(), first._message._payload.data());
      EXPECT_EQ(support::recovered_image(_directory), pinned);

      // Release needs no new issuance. Once its pin ends, stale Pending work
      // can become Lost, and the gap itself does not consume another number.
      require_value(source.release(exhausted));
      const auto gap_result = source.receive();
      ASSERT_TRUE(std::holds_alternative<DeliveryGap>(gap_result));
      const auto gap = std::get<DeliveryGap>(gap_result);
      EXPECT_EQ(gap._queue, scenario._queue);
      EXPECT_EQ(gap._firstSequence, 1u);
      EXPECT_EQ(gap._lastSequence, 2u);
      const auto lost = support::recovered_image(_directory);
      expect_receive_error(source.receive(), ErrorCode::CapacityExceeded);
      EXPECT_EQ(support::recovered_image(_directory), lost);
      require_value(source.close());
    }

    auto replacement = require_value(scenario._created._session.attach_task(scenario._consumer));
    EXPECT_GT(replacement.claim()._incarnation, exhausted._consumer._incarnation);
    auto source = require_value(replacement.open_source_set(scenario._source));
    const auto retained = receive_delivery(source);
    EXPECT_EQ(retained._token._sequence, 3u);
    EXPECT_EQ(retained._token._issuance, 1u);
    const auto current = support::recovered_image(_directory);
    expect_error(source.release(exhausted), ErrorCode::StaleDelivery);
    EXPECT_EQ(support::recovered_image(_directory), current);
    require_value(source.acknowledge(retained._token));
    const auto last = receive_delivery(source);
    EXPECT_EQ(last._token._sequence, 4u);
    require_value(source.acknowledge(last._token));
    EXPECT_TRUE(std::holds_alternative<ReceiveTimeout>(source.receive()));
    require_value(source.close());
  }

  TEST_F(TransportExhaustionTest, FinalProducerIdentityStaysAcceptedAtFullLogAndReplacementCanInspectIt)
  {
    PublicationId last;
    PublicationId first;
    QueueId queue;
    {
      auto scenario = support::create_source_scenario(_directory);
      queue = scenario._queue;
      auto writer = require_value(scenario._created._creator.open_writer(queue));
      first = support::send_source_value(scenario, writer, 1);
      const auto producer = scenario._created._creator.claim();
      const auto location = offsets(producer._task);
      // Publication identities may have unused issued numbers. Queue sequences
      // remain the real contiguous 1,2 history created by the two sends.
      support::set_atomic_word(_directory, location._producer + 8, final_counter - 1);
      last = require_value(scenario._created._creator.next_publication_id());
      EXPECT_EQ(last._sequence, final_counter);
      const auto bytes = require_value(support::source_codec(scenario).encode({std::uint32_t{2}}));
      const auto message = support::source_message(scenario._schema, bytes);
      const auto accepted = require_value(writer.send(last, message));
      EXPECT_EQ(accepted._status, AcceptanceStatus::Accepted);
      support::fill_log_with_queue_revision(support::open_runtime_state(_directory), queue, producer);

      const auto full = support::recovered_image(_directory);
      expect_error(scenario._created._creator.next_publication_id(), ErrorCode::CapacityExceeded);
      EXPECT_EQ(require_value(writer.send(last, message))._status, AcceptanceStatus::Accepted);
      EXPECT_EQ(require_value(scenario._created._creator.publication_status(last))._status,
                AcceptanceStatus::Accepted);
      const auto changed = require_value(support::source_codec(scenario).encode({std::uint32_t{3}}));
      expect_error(writer.send(last, support::source_message(scenario._schema, changed)), ErrorCode::Conflict);
      EXPECT_EQ(support::recovered_image(_directory), full);
    }

    // Replacement requires reserved state only, even when the former claim
    // exhausted its identity counter and the append arena is exactly full.
    auto session = require_value(Session::attach(_directory));
    auto replacement = require_value(session.attach_task("Producer"));
    EXPECT_GT(replacement.claim()._incarnation, last._producer._incarnation);
    EXPECT_EQ(require_value(replacement.publication_status(last))._status, AcceptanceStatus::Accepted);
    const auto next = require_value(replacement.next_publication_id());
    EXPECT_EQ(next._sequence, 1u);
    auto writer = require_value(replacement.open_writer(queue));
    const auto codec = require_value(BinaryPayloadCodec::from_registry(
        session.registry(), SchemaId{MessageType{1}, 1}, 4));
    const auto bytes = require_value(codec.encode({std::uint32_t{3}}));
    const auto full = support::recovered_image(_directory);
    expect_error(writer.send(next, support::source_message(codec.schema(), bytes)), ErrorCode::LogFull);
    EXPECT_EQ(require_value(replacement.publication_status(next))._status, AcceptanceStatus::Rejected);
    EXPECT_EQ(support::recovered_image(_directory), full);

    auto consumer = require_value(session.attach_task("Consumer"));
    auto source = require_value(consumer.open_source_set("Inputs"));
    const auto prior = receive_delivery(source);
    EXPECT_EQ(prior._token._sequence, 1u);
    EXPECT_EQ(prior._message._header._publication, first);
    require_value(source.acknowledge(prior._token));
    const auto final = receive_delivery(source);
    EXPECT_EQ(final._token._sequence, 2u);
    EXPECT_EQ(final._message._header._publication, last);
    require_value(source.acknowledge(final._token));
    EXPECT_TRUE(std::holds_alternative<ReceiveTimeout>(source.receive()));
    require_value(source.close());
  }

  TEST_F(TransportExhaustionTest, FullLogRejectionsLeaveMixedTerminalAndPinnedHistoryUnchanged)
  {
    auto scenario = support::create_source_scenario(_directory, true,
        TaskKind::Process, DeliveryPolicy::AllowLoss);
    auto writer = require_value(scenario._created._creator.open_writer(scenario._queue));
    std::vector<PublicationId> publications;
    publications.push_back(support::send_source_value(scenario, writer, 1));
    auto consumer = require_value(scenario._created._session.attach_task(scenario._consumer));
    auto source = require_value(consumer.open_source_set(scenario._source));
    const auto first = receive_delivery(source);
    require_value(source.acknowledge(first._token));
    for (std::uint32_t value = 2; value != 7; ++value)
    {
      publications.push_back(support::send_source_value(scenario, writer, value));
    }
    const auto gap_result = source.receive();
    ASSERT_TRUE(std::holds_alternative<DeliveryGap>(gap_result));
    EXPECT_EQ(std::get<DeliveryGap>(gap_result)._firstSequence, 2u);
    EXPECT_EQ(std::get<DeliveryGap>(gap_result)._lastSequence, 4u);
    const auto pinned = receive_delivery(source);
    ASSERT_EQ(pinned._token._sequence, 5u);
    const auto fresh = require_value(scenario._created._creator.next_publication_id());
    support::fill_log_with_queue_revision(support::open_runtime_state(_directory), scenario._queue,
        scenario._created._creator.claim());
    const auto bytes = require_value(support::source_codec(scenario).encode({std::uint32_t{7}}));
    const auto message = support::source_message(scenario._schema, bytes);
    auto wrong_token = pinned._token;
    ++wrong_token._issuance;
    const auto unchanged = support::recovered_image(_directory);

    // Ack(1), Lost(2..4), Issued(5), and Pending(6) coexist. Rejection must not
    // alter any of them or the Source pin, even when append space is exhausted.
    expect_error(writer.send(fresh, message), ErrorCode::LogFull);
    expect_error(writer.send(publications.front(), message), ErrorCode::Conflict);
    auto malformed = message;
    ++malformed._header._payloadBytes;
    expect_error(writer.send(fresh, malformed), ErrorCode::InvalidArgument);
    const auto report = writer.send(publications.front(), message, {});
    EXPECT_EQ(report._error, ErrorCode::OutputBufferTooSmall);
    expect_error(source.acknowledge(wrong_token), ErrorCode::StaleDelivery);
    expect_error(source.release(wrong_token), ErrorCode::StaleDelivery);
    EXPECT_EQ(require_value(scenario._created._creator.publication_status(fresh))._status,
              AcceptanceStatus::Rejected);
    for (const auto publication : publications)
    {
      EXPECT_EQ(require_value(scenario._created._creator.publication_status(publication))._status,
                AcceptanceStatus::Accepted);
    }
    EXPECT_EQ(support::recovered_image(_directory), unchanged);
    const auto repeated = receive_delivery(source);
    EXPECT_EQ(repeated._token, pinned._token);
    EXPECT_EQ(repeated._message._header, pinned._message._header);
    EXPECT_EQ(repeated._message._payload.data(), pinned._message._payload.data());
    EXPECT_EQ(support::recovered_image(_directory), unchanged);
    require_value(source.acknowledge(repeated._token));
    const auto last = receive_delivery(source);
    EXPECT_EQ(last._token._sequence, 6u);
    require_value(source.acknowledge(last._token));
    EXPECT_TRUE(std::holds_alternative<ReceiveTimeout>(source.receive()));
    require_value(source.close());
  }

  TEST_F(TransportExhaustionTest, FinalGlobalGenerationKeepsAnExistingPinAndReadOnlyAcceptanceUsable)
  {
    auto scenario = support::create_source_scenario(_directory);
    auto writer = require_value(scenario._created._creator.open_writer(scenario._queue));
    const auto first = support::send_source_value(scenario, writer, 1);
    support::send_source_value(scenario, writer, 2);
    const auto fresh = require_value(scenario._created._creator.next_publication_id());
    auto consumer = require_value(scenario._created._session.attach_task(scenario._consumer));
    auto source = require_value(consumer.open_source_set(scenario._source));
    const auto prior_generation = support::journal_snapshot(_directory)._generation;

    // Unlike append exhaustion, a globally exhausted operation generation
    // prevents even reserved-state mutation. The final legal operation below
    // is a real public receive; no Publication/Queue history is fabricated.
    support::set_atomic_word(_directory, 120, final_counter - 1);
    const auto pinned = receive_delivery(source);
    EXPECT_EQ(support::journal_snapshot(_directory)._generation, final_counter);
    const auto bytes = require_value(support::source_codec(scenario).encode({std::uint32_t{1}}));
    const auto message = support::source_message(scenario._schema, bytes);
    const auto exhausted = support::recovered_image(_directory);
    EXPECT_EQ(require_value(writer.send(first, message))._status, AcceptanceStatus::Accepted);
    EXPECT_EQ(require_value(scenario._created._creator.publication_status(first))._status,
              AcceptanceStatus::Accepted);
    expect_error(writer.send(fresh, message), ErrorCode::CapacityExceeded);
    expect_error(scenario._created._creator.next_publication_id(), ErrorCode::CapacityExceeded);
    expect_error(source.acknowledge(pinned._token), ErrorCode::CapacityExceeded);
    expect_error(source.release(pinned._token), ErrorCode::CapacityExceeded);
    EXPECT_EQ(support::recovered_image(_directory), exhausted);
    const auto repeated = receive_delivery(source);
    EXPECT_EQ(repeated._token, pinned._token);
    EXPECT_EQ(repeated._message._header, pinned._message._header);
    EXPECT_EQ(repeated._message._payload.data(), pinned._message._payload.data());
    EXPECT_EQ(support::recovered_image(_directory), exhausted);

    // Restore only this private boundary after observing failure. The last
    // operation was FixedState, so no appended record carries its artificial
    // generation; an ordinary next value lets the fixture settle its real pin.
    support::set_atomic_word(_directory, 120, prior_generation + 1);
    require_value(source.acknowledge(pinned._token));
    const auto last = receive_delivery(source);
    EXPECT_EQ(last._token._sequence, 2u);
    require_value(source.acknowledge(last._token));
    require_value(source.close());
  }
}
