// Deliberate persisted-state fixtures. Public send/receive builds real history;
// atomic edits then qualify which allowed-loss histories a reader may accept.
// Applications request loss through SourceQueueSet::receive(), never these edits.
#include "source_fixture.h"

#include "delivery_words.h"
#include "mapped_payload.h"
#include "native_fixture.h"
#include "publication_metadata.h"
#include "result_assertions.h"
#include "topology_state.h"

#include <gtest/gtest.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <optional>
#include <utility>
#include <variant>
#include <vector>

namespace
{
  using namespace angaria;
  namespace runtime = angaria::detail;
  namespace mapped = angaria::detail::mapped;
  namespace support = angaria::test_support;
  namespace fs = std::filesystem;
  using support::expect_error;
  using support::require_value;

  struct RetainedDelivery
  {
    std::uint64_t _link{};
    std::uint64_t _receipt{};
  };

  class LossValidationTest : public testing::Test
  {
  protected:
    void SetUp() override
    {
      auto pattern = (fs::temp_directory_path() / "angaria-loss-validation-XXXXXX").string();
      ASSERT_NE(::mkdtemp(pattern.data()), nullptr);
      _root = pattern;
      _directory = _root / "session";
    }

    void TearDown() override
    {
      _scenario.reset();
      std::error_code ignored;
      fs::remove_all(_root, ignored);
    }

    void create(DeliveryPolicy policy = DeliveryPolicy::AllowLoss)
    {
      _scenario = support::create_source_scenario(_directory, true, TaskKind::Process, policy);
    }

    void inspect_history()
    {
      runtime::RuntimeAccess access(support::open_runtime_state(_directory));
      const auto topology = runtime::runtime_topology(access);
      _queueState = topology._queues.front()._stateOffset;
      _deliveries.clear();
      for (const auto reference : access.record_references(9))
      {
        const auto metadata = runtime::runtime_publication_metadata(access, reference);
        _deliveries.push_back({reference + mapped::record_envelope_bytes + mapped::publication_fixed_bytes,
                              metadata._destinations.front()._receiptReference});
      }
    }

    mapped::Receipt receipt(std::size_t index)
    {
      runtime::RuntimeAccess access(support::open_runtime_state(_directory));
      return require_value(mapped::decode_receipt_words(access.read_words(_deliveries.at(index)._receipt, 16),
                                                        mapped::ReceiptProfile::AllowLoss));
    }

    void write_receipt(std::size_t index, const mapped::Receipt& receipt)
    {
      const auto words = require_value(mapped::encode_receipt_words(receipt, mapped::ReceiptProfile::AllowLoss));
      for (std::size_t word = 0; word < words.size(); ++word)
      {
        support::set_atomic_word(_directory, _deliveries.at(index)._receipt + word * 8, words[word]);
      }
    }

    // Install only the terminal suffix requested by a fixture. Actual earlier
    // acknowledgments and their Queue timestamp must survive subsequent loss.
    void lose_prefix(std::size_t first, std::size_t last)
    {
      for (std::size_t sequence = first; sequence <= last; ++sequence)
      {
        auto lost = receipt(sequence - 1);
        lost._state = mapped::ReceiptState::Lost;
        lost._presence |= 4;
        lost._terminalAt = UtcTimestamp{std::chrono::nanoseconds{-7}};
        write_receipt(sequence - 1, lost);
      }
      support::set_atomic_word(_directory, _queueState + 64, last);
      support::set_atomic_word(_directory, _queueState + 72,
          last == _deliveries.size() ? 0 : _deliveries[last]._link);
    }

    void reject_without_writes()
    {
      const auto before = support::recovered_image(_directory);
      expect_error(Session::attach(_directory), ErrorCode::IoError);
      EXPECT_EQ(support::recovered_image(_directory), before);
    }

    fs::path _root;
    fs::path _directory;
    std::optional<support::SourceScenario> _scenario;
    std::uint64_t _queueState{};
    std::vector<RetainedDelivery> _deliveries;
  };

  TEST_F(LossValidationTest, MixedAcknowledgedAndLostPrefixRetainsTheLastActualAcknowledgment)
  {
    create();
    auto& scenario = *_scenario;
    auto writer = require_value(scenario._created._creator.open_writer(scenario._queue));
    support::send_source_value(scenario, writer, 1);
    auto consumer = require_value(scenario._created._session.attach_task(scenario._consumer));
    auto source = require_value(consumer.open_source_set(scenario._source));
    auto received = source.receive();
    ASSERT_TRUE(std::holds_alternative<Delivery>(received));
    for (std::uint32_t value = 2; value <= 5; ++value)
    {
      support::send_source_value(scenario, writer, value);
    }
    require_value(source.acknowledge(std::get<Delivery>(received)._token));
    inspect_history();
    const auto before = support::recovered_image(_directory);
    const auto acknowledgment = before.at(static_cast<std::size_t>(_queueState / 8 + 10));
    lose_prefix(2, 3);

    // C=2 and N=5 give the current ring window [4,5]. Only the overwritten
    // prefix may be terminal Lost, and loss does not invent an acknowledgment.
    const auto settled = support::recovered_image(_directory);
    EXPECT_EQ(settled.at(static_cast<std::size_t>(_queueState / 8 + 10)), acknowledgment);
    EXPECT_EQ(settled.at(static_cast<std::size_t>(_queueState / 8 + 11)), 1u);
    require_value(Session::attach(_directory));
    EXPECT_EQ(support::recovered_image(_directory), settled);
  }

  TEST_F(LossValidationTest, LostReleasedReceiptPreservesAndValidatesItsHistoricalConsumerTags)
  {
    create();
    auto& scenario = *_scenario;
    auto writer = require_value(scenario._created._creator.open_writer(scenario._queue));
    support::send_source_value(scenario, writer, 1);
    auto consumer = require_value(scenario._created._session.attach_task(scenario._consumer));
    auto source = require_value(consumer.open_source_set(scenario._source));
    auto received = source.receive();
    ASSERT_TRUE(std::holds_alternative<Delivery>(received));
    require_value(source.release(std::get<Delivery>(received)._token));
    for (std::uint32_t value = 2; value <= 5; ++value)
    {
      support::send_source_value(scenario, writer, value);
    }
    inspect_history();
    const auto original = receipt(0);
    lose_prefix(1, 3);
    const auto lost = receipt(0);
    EXPECT_EQ(lost._consumerTask, original._consumerTask);
    EXPECT_EQ(lost._consumerIncarnation, original._consumerIncarnation);
    EXPECT_EQ(lost._issuance, original._issuance);
    EXPECT_EQ(lost._dequeuedAt, original._dequeuedAt);
    require_value(Session::attach(_directory));

    auto inventedClaim = lost;
    ++inventedClaim._consumerIncarnation;
    write_receipt(0, inventedClaim);
    reject_without_writes();
  }

  TEST_F(LossValidationTest, RecoverQueueCannotContainLostReceiptsEvenWhenTheSessionSupportsLoss)
  {
    create(DeliveryPolicy::RecoverFromLog);
    auto& scenario = *_scenario;
    auto writer = require_value(scenario._created._creator.open_writer(scenario._queue));
    for (std::uint32_t value = 1; value <= 3; ++value)
    {
      support::send_source_value(scenario, writer, value);
    }
    inspect_history();
    lose_prefix(1, 1);
    reject_without_writes();
  }

  TEST_F(LossValidationTest, LossCannotReachIntoTheAuthoritativeLatestRingWindow)
  {
    create();
    auto& scenario = *_scenario;
    auto writer = require_value(scenario._created._creator.open_writer(scenario._queue));
    for (std::uint32_t value = 1; value <= 3; ++value)
    {
      support::send_source_value(scenario, writer, value);
    }
    inspect_history();
    lose_prefix(1, 2); // Sequence 2 is still present in the latest window [2,3].
    reject_without_writes();
  }

  TEST_F(LossValidationTest, TerminalLossCannotSkipAnEarlierPendingDelivery)
  {
    create();
    auto& scenario = *_scenario;
    auto writer = require_value(scenario._created._creator.open_writer(scenario._queue));
    for (std::uint32_t value = 1; value <= 5; ++value)
    {
      support::send_source_value(scenario, writer, value);
    }
    inspect_history();
    lose_prefix(2, 3); // Sequence 1 remains Pending, so this is not a prefix.
    reject_without_writes();
  }

  TEST_F(LossValidationTest, LossCannotSilentlyRevokeAnIssuedSourcePin)
  {
    create();
    auto& scenario = *_scenario;
    auto writer = require_value(scenario._created._creator.open_writer(scenario._queue));
    support::send_source_value(scenario, writer, 1);
    auto consumer = require_value(scenario._created._session.attach_task(scenario._consumer));
    auto source = require_value(consumer.open_source_set(scenario._source));
    ASSERT_TRUE(std::holds_alternative<Delivery>(source.receive()));
    for (std::uint32_t value = 2; value <= 5; ++value)
    {
      support::send_source_value(scenario, writer, value);
    }
    inspect_history();
    lose_prefix(1, 3); // The Source slot still pins sequence 1 and must reject.
    reject_without_writes();
  }

  TEST_F(LossValidationTest, CoreProfileCannotInterpretStoredLostReceipts)
  {
    create(DeliveryPolicy::RecoverFromLog);
    {
      auto& scenario = *_scenario;
      auto writer = require_value(scenario._created._creator.open_writer(scenario._queue));
      for (std::uint32_t value = 1; value <= 3; ++value)
      {
        support::send_source_value(scenario, writer, value);
      }
      inspect_history();
      lose_prefix(1, 1);
    }
    _scenario.reset();
    // A deliberate, offline old-profile fixture. No public API downgrades or
    // upgrades a persisted Session's negotiated required capabilities.
    support::set_atomic_word(_directory, 56, 7);
    reject_without_writes();
  }
} // namespace
