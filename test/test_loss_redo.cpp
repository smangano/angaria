// Public setup creates real direct messages. The hand-written intents and
// fault checkpoints below qualify private journal validation and replay; an
// application requests allowed loss through SourceQueueSet::receive().
#include "source_fixture.h"

#include "angaria/detail/byte_operations.h"
#include "delivery_words.h"
#include "loss_redo.h"
#include "mapped_payload.h"
#include "native_fixture.h"
#include "result_assertions.h"
#include "topology_state.h"

#include <gtest/gtest.h>

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <system_error>
#include <variant>
#include <vector>

namespace
{
  using namespace angaria;
  namespace runtime = angaria::detail;
  namespace mapped = runtime::mapped;
  namespace support = angaria::test_support;
  namespace fs = std::filesystem;
  using support::expect_error;
  using support::require_value;

  class PersistentLossFault final : public mapped::JournalFaultInjector
  {
  public:
    OperationResult checkpoint(mapped::JournalCheckpoint point, std::uint64_t index) override
    {
      if (point == mapped::JournalCheckpoint::RedoWord && index == 0)
      {
        return Error{ErrorCode::IoError, "Injected failure after the first Lost state store", "test.loss"};
      }
      return std::monostate{};
    }
  };

  class LossRedoTest : public testing::Test
  {
  protected:
    void SetUp() override
    {
      auto pattern = (fs::temp_directory_path() / "angaria-loss-redo-XXXXXX").string();
      ASSERT_NE(::mkdtemp(pattern.data()), nullptr);
      _root = pattern;
      _directory = _root / "session";
    }

    void TearDown() override
    {
      _consumer.reset();
      _scenario.reset();
      std::error_code ignored;
      fs::remove_all(_root, ignored);
    }

    void create(DeliveryPolicy policy = DeliveryPolicy::AllowLoss, std::uint32_t count = 5)
    {
      _scenario = support::create_source_scenario(_directory, true, TaskKind::Process, policy);
      auto writer = require_value(_scenario->_created._creator.open_writer(_scenario->_queue));
      for (std::uint32_t value = 1; value <= count; ++value)
      {
        support::send_source_value(*_scenario, writer, value);
      }
      _consumer = require_value(_scenario->_created._session.attach_task(_scenario->_consumer));
      _claim = _consumer->claim();
      _producer = _scenario->_created._creator.claim()._task;
    }

    mapped::TransactionPlan plan(runtime::RuntimeAccess& access, std::int64_t timestamp = -7)
    {
      const auto topology = runtime::runtime_topology(access);
      _queue = topology._queues.front()._stateOffset;
      _source = topology._sources.front()._stateOffset;
      const auto references = access.record_references(9);
      _receipts.clear();
      for (const auto reference : references)
      {
        _receipts.push_back(runtime::runtime_publication_metadata(access, reference)
                                ._destinations.front()._receiptReference);
      }
      const auto retained = runtime::runtime_publication_metadata(access, references.at(3));
      mapped::TransactionPlan result;
      result._base = access._snapshot;
      result._actor = _claim._task;
      result._actorIncarnation = _claim._incarnation;
      result._newRevision = access._snapshot._revision;
      mapped::RedoCommand loss{mapped::RedoOpcode::LoseRange, _queue, std::vector<std::byte>(32)};
      runtime::write_unsigned_le(loss._payload, 0, std::uint64_t{1});
      runtime::write_unsigned_le(loss._payload, 8, std::uint64_t{3});
      runtime::write_unsigned_le(loss._payload, 16, std::bit_cast<std::uint64_t>(timestamp));
      runtime::write_unsigned_le(loss._payload, 24, static_cast<std::uint16_t>(_scenario->_source));
      result._commands.push_back(std::move(loss));
      const std::array<std::uint64_t, 2> progress{3, retained._layout._destinationsOffset};
      result._commands.push_back(runtime::runtime_write_words(_queue + 64, progress));
      return result;
    }

    std::vector<std::byte> raw_image()
    {
      auto file = require_value(mapped::NativeFile::open_existing(
          _directory / "session.ang", mapped::FileAccess::ReadOnly));
      std::vector<std::byte> image(static_cast<std::size_t>(require_value(file.size())));
      require_value(file.read_exact(0, image));
      return image;
    }

    fs::path _root;
    fs::path _directory;
    std::optional<support::SourceScenario> _scenario;
    std::optional<Task> _consumer;
    TaskClaim _claim;
    TaskId _producer{};
    std::uint64_t _queue{};
    std::uint64_t _source{};
    std::vector<std::uint64_t> _receipts;
  };

  TEST_F(LossRedoTest, CompactIntentConsumesNoAllocationAndPreservesSourceAndAcknowledgmentWords)
  {
    create();
    runtime::RuntimeAccess access(support::open_runtime_state(_directory));
    const auto loss = plan(access, 0); // Epoch zero is a present terminal timestamp.
    const auto source = access.read_words(_source, 32);
    const auto acknowledgment = access.read_words(_queue + 80, 2);
    const auto committed = require_value(access._journal.execute(access._guard, loss));
    EXPECT_EQ(committed._tail, loss._base._tail);
    EXPECT_EQ(committed._manifest, loss._base._manifest);
    EXPECT_EQ(require_value(access._file->size()), loss._base._fileBytes);
    EXPECT_EQ(access.read_words(_source, 32), source);
    EXPECT_EQ(access.read_words(_queue + 80, 2), acknowledgment);
    EXPECT_EQ(access.read_words(_queue + 64, 1).front(), 3u);
    for (std::size_t index = 0; index != _receipts.size(); ++index)
    {
      const auto receipt = require_value(mapped::decode_receipt_words(
          access.read_words(_receipts[index], 16), mapped::ReceiptProfile::AllowLoss));
      EXPECT_EQ(receipt._state, index < 3 ? mapped::ReceiptState::Lost : mapped::ReceiptState::Pending);
      EXPECT_EQ(receipt._presence, index < 3 ? 4u : 0u);
      EXPECT_EQ(receipt._terminalAt, UtcTimestamp{});
    }
    // The persisted stream stays 80 bytes regardless of the skipped range.
    const auto header = require_value(mapped::decode_journal_header(
        access.read_bytes(mapped::journal_offset, mapped::journal_header_bytes)));
    EXPECT_EQ(header._inlineBytes, 80u);
  }

  TEST_F(LossRedoTest, EveryMalformedIntentIsRejectedBeforePreparingOrChangingAnyWord)
  {
    create();
    runtime::RuntimeAccess access(support::open_runtime_state(_directory));
    const auto valid = plan(access);
    const auto before = raw_image();
    for (unsigned variant = 0; variant != 10; ++variant)
    {
      SCOPED_TRACE(variant);
      auto malformed = valid;
      switch (variant)
      {
      case 0: // The first pending delivery must not be skipped.
        runtime::write_unsigned_le(malformed._commands[0]._payload, 0, std::uint64_t{2});
        break;
      case 1: // The authoritative current ring window begins at four.
        runtime::write_unsigned_le(malformed._commands[0]._payload, 8, std::uint64_t{4});
        break;
      case 2:
        malformed._commands[0]._payload[31] = std::byte{1};
        break;
      case 3:
        runtime::write_unsigned_le(malformed._commands[0]._payload, 24, std::uint16_t{2});
        break;
      case 4:
        malformed._actor = _producer;
        break;
      case 5:
        ++malformed._actorIncarnation;
        break;
      case 6:
        runtime::write_unsigned_le(malformed._commands[1]._payload, 0, std::uint64_t{2});
        break;
      case 7:
        runtime::write_unsigned_le(malformed._commands[1]._payload, 8, std::uint64_t{0});
        break;
      case 8: // A later mutable write must not erase discovery or a new pin.
        malformed._commands.push_back(runtime::runtime_write_words(_source, std::array<std::uint64_t, 1>{0}));
        break;
      case 9:
        ++malformed._commands[0]._target;
        break;
      }
      ASSERT_TRUE(std::holds_alternative<Error>(access._journal.execute(access._guard, malformed)));
      EXPECT_EQ(raw_image(), before);
    }
  }

  TEST_F(LossRedoTest, RecoverPolicyAndLegacyCapabilitiesRejectLossWithoutAnyWrite)
  {
    create(DeliveryPolicy::RecoverFromLog);
    runtime::RuntimeAccess access(support::open_runtime_state(_directory));
    const auto loss = plan(access);
    const auto before = raw_image();
    expect_error(access._journal.execute(access._guard, loss), ErrorCode::InvalidArgument);
    const auto layout = *mapped::make_fixed_layout(access._session->_controller->configuration()._limits);
    // Both older native and legacy readers pass allowLoss=false. Admission
    // rejects the opcode before consulting its participant discovery tables.
    expect_error(mapped::validate_loss_commands(*access._file, layout, {}, loss._commands,
        loss._actor, loss._actorIncarnation, false, false, ErrorCode::InvalidArgument), ErrorCode::UnsupportedPolicy);
    expect_error(mapped::validate_loss_commands(*access._file, layout, {}, loss._commands,
        loss._actor, loss._actorIncarnation, false, true), ErrorCode::IoError);
    EXPECT_EQ(raw_image(), before);
  }

  TEST_F(LossRedoTest, ReplayRepairsEveryFieldAfterTheStateStoreBeforeStrictReceiptDecoding)
  {
    create();
    PersistentLossFault fault;
    {
      runtime::RuntimeAccess access(support::open_runtime_state(_directory), true, &fault);
      expect_error(access._journal.execute(access._guard, plan(access)), ErrorCode::OutcomeUnknown);
      const auto partial = access.read_words(_receipts.front(), 16);
      EXPECT_EQ(partial[0], 4u);
      EXPECT_EQ(partial[1], 0u); // Presence and timestamp have not been installed.
      expect_error(mapped::decode_receipt_words(partial, mapped::ReceiptProfile::AllowLoss), ErrorCode::IoError);
    }
    {
      runtime::RuntimeAccess repaired(support::open_runtime_state(_directory));
      const auto receipt = require_value(mapped::decode_receipt_words(
          repaired.read_words(_receipts.front(), 16), mapped::ReceiptProfile::AllowLoss));
      EXPECT_EQ(receipt._state, mapped::ReceiptState::Lost);
      EXPECT_EQ(receipt._presence, 4u);
      EXPECT_EQ(receipt._terminalAt, UtcTimestamp{std::chrono::nanoseconds{-7}});
      EXPECT_EQ(repaired.read_words(_queue + 64, 1).front(), 3u);
    }
    const auto once = raw_image();
    require_value(Session::attach(_directory));
    EXPECT_EQ(raw_image(), once);
  }

  TEST_F(LossRedoTest, CommittedReplayNeverOverwritesANewerSourcePin)
  {
    create();
    PersistentLossFault fault;
    {
      runtime::RuntimeAccess access(support::open_runtime_state(_directory), true, &fault);
      expect_error(access._journal.execute(access._guard, plan(access)), ErrorCode::OutcomeUnknown);
    }
    // This impossible intervening pin simulates damaged persisted authority.
    // Recovery must fail before repairing even the first partial Lost receipt.
    support::set_atomic_word(_directory, _source + 16, 99);
    const auto before = raw_image();
    expect_error(Session::attach(_directory), ErrorCode::IoError);
    EXPECT_EQ(raw_image(), before);
    support::set_atomic_word(_directory, _source + 16, 0);
    require_value(Session::attach(_directory));
  }

  TEST_F(LossRedoTest, IssuedReceiptAndSourcePinAreRejectedBeforePreparingLoss)
  {
    create(DeliveryPolicy::AllowLoss, 1);
    auto source = require_value(_consumer->open_source_set(_scenario->_source));
    const auto received = source.receive();
    ASSERT_TRUE(std::holds_alternative<Delivery>(received));
    auto writer = require_value(_scenario->_created._creator.open_writer(_scenario->_queue));
    for (std::uint32_t value = 2; value <= 5; ++value)
    {
      support::send_source_value(*_scenario, writer, value);
    }
    {
      runtime::RuntimeAccess access(support::open_runtime_state(_directory));
      const auto loss = plan(access);
      const auto before = raw_image();
      expect_error(access._journal.execute(access._guard, loss), ErrorCode::InvalidArgument);
      EXPECT_EQ(raw_image(), before);
    }
    require_value(source.release(std::get<Delivery>(received)._token));
  }

  TEST_F(LossRedoTest, RecoveryValidatesTheFollowingWatermarkBeforeRepairingAnyReceipt)
  {
    create();
    PersistentLossFault fault;
    std::uint64_t next{};
    {
      runtime::RuntimeAccess access(support::open_runtime_state(_directory), true, &fault);
      const auto loss = plan(access);
      next = runtime::read_unsigned_le<std::uint64_t>(loss._commands[1]._payload, 8);
      expect_error(access._journal.execute(access._guard, loss), ErrorCode::OutcomeUnknown);
    }
    // First command occupies 48 bytes. The next command's second payload word
    // is byte 72 of the inline stream; corrupt it after committed authority.
    const auto followingLink = mapped::journal_offset + mapped::journal_header_bytes + 72;
    support::set_atomic_word(_directory, followingLink, 0);
    const auto before = raw_image();
    expect_error(Session::attach(_directory), ErrorCode::IoError);
    EXPECT_EQ(raw_image(), before);
    support::set_atomic_word(_directory, followingLink, next);
    require_value(Session::attach(_directory));
  }

  TEST_F(LossRedoTest, LossNeverRewritesAnAlreadyAcknowledgedReceiptInItsCapturedRange)
  {
    create();
    std::vector<std::uint64_t> acknowledged;
    {
      runtime::RuntimeAccess access(support::open_runtime_state(_directory));
      const auto loss = plan(access);
      // Strict public receiving cannot produce this interrupted progress shape.
      // Deliberately retain a complete earlier terminal result inside the
      // captured range to qualify the journal's preserve-Acknowledged rule.
      mapped::Receipt receipt;
      receipt._state = mapped::ReceiptState::Acknowledged;
      receipt._presence = 7;
      receipt._consumerTask = loss._actor;
      receipt._sourceSet = _scenario->_source;
      receipt._consumerIncarnation = loss._actorIncarnation;
      receipt._issuance = 1;
      const auto encoded = require_value(mapped::encode_receipt_words(receipt, mapped::ReceiptProfile::AllowLoss));
      for (std::size_t word = 0; word != encoded.size(); ++word)
      {
        support::set_atomic_word(_directory, _receipts.at(1) + word * 8, encoded[word]);
      }
      support::set_atomic_word(_directory, _source, 1);
      support::set_atomic_word(_directory, _source + 8, loss._actorIncarnation);
      support::set_atomic_word(_directory, _queue + 88, 1);
      acknowledged = access.read_words(_receipts.at(1), 16);
      require_value(access._journal.execute(access._guard, loss));
      EXPECT_EQ(access.read_words(_receipts.at(1), 16), acknowledged);
      EXPECT_EQ(access.read_words(_queue + 80, 2), (std::vector<std::uint64_t>{0, 1}));
    }
    require_value(Session::attach(_directory));
  }

  TEST_F(LossRedoTest, ClaimCleanupRejectsLostOnARecoverQueueBeforeChangingTheClaim)
  {
    create(DeliveryPolicy::RecoverFromLog);
    {
      runtime::RuntimeAccess access(support::open_runtime_state(_directory));
      auto cleanup = plan(access);
      const auto task = runtime::runtime_task(access, _claim._task);
      cleanup._commands.clear();
      mapped::RedoCommand release{mapped::RedoOpcode::ReleaseClaim, task._stateOffset,
                                 std::vector<std::byte>(16)};
      runtime::write_unsigned_le(release._payload, 0, _claim._incarnation);
      cleanup._commands.push_back(std::move(release));
      // The Session capability admits Lost representation, but the Queue's
      // individual delivery policy does not. Cleanup must qualify that policy
      // before detaching a Task or clearing any Source slot.
      support::set_atomic_word(_directory, _receipts.front(), 4);
      support::set_atomic_word(_directory, _receipts.front() + 8, std::uint64_t{4} << 32);
      support::set_atomic_word(_directory, _queue + 64, 1);
      const auto before = raw_image();
      expect_error(access._journal.execute(access._guard, cleanup), ErrorCode::InvalidArgument);
      EXPECT_EQ(raw_image(), before);
      support::set_atomic_word(_directory, _receipts.front(), 1);
      support::set_atomic_word(_directory, _receipts.front() + 8, 0);
      support::set_atomic_word(_directory, _queue + 64, 0);
    }
    require_value(Session::attach(_directory));
  }
}
