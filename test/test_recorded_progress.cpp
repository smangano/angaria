// Storage-only qualification for consumer-owned progress. The connection inode
// supplies binding identity; these fixtures do not construct public Task claims
// or move message bytes. Expected wire bytes are authored independently.
#include "recorded_progress.h"
#include "producer_log_fixture.h"
#include "result_assertions.h"
#include "wire_fixture.h"

#include <gtest/gtest.h>

#include <array>
#include <bit>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <optional>
#include <vector>

namespace
{
  using namespace angaria;
  namespace recorded = angaria::detail::recorded;
  namespace mapped = angaria::detail::mapped;
  namespace support = angaria::test_support;
  using support::expect_error;
  using support::require_value;
  using support::put_integer;

  class RecordedProgressTest : public testing::Test
  {
  protected:
    void SetUp() override
    {
      auto connection = require_value(mapped::NativeFile::create_exclusive(_directory.path() / "connection", 4096));
      require_value(connection.reserve(4096));
      _options._path = _directory.path() / "progress";
      _options._session = {0x0102030405060708, 0x1112131415161718};
      _options._consumer = TaskId{6};
      _options._source = SourceQueueSetId{2};
      _options._queue = QueueId{4};
      _options._connectionGeneration = 9;
      _options._connectionIdentity = require_value(connection.identity());
      _options._publicationCapacity = 8;
      _options._liveAbiProfile = require_value(mapped::qualify_native_profile())._liveAbiProfile;
      require_value(recorded::ProgressStore::create(_options));
    }

    std::vector<std::byte> stored_bytes() const
    {
      auto file = require_value(mapped::NativeFile::open_existing(_options._path, mapped::FileAccess::ReadOnly));
      std::vector<std::byte> bytes(static_cast<std::size_t>(require_value(file.size())));
      require_value(file.read_exact(0, bytes));
      return bytes;
    }

    static recorded::ProgressImage next_image(recorded::ProgressStore& store)
    {
      const auto current = require_value(store.snapshot());
      recorded::ProgressImage image;
      image._generation = current._generation + 1;
      image._source = current._source;
      return image;
    }

    static void attach(recorded::ProgressStore& store, std::uint64_t actor = 7)
    {
      auto image = next_image(store);
      image._source._actorIncarnation = actor;
      image._source._issuance = 0;
      require_value(store.commit(image));
    }

    static recorded::ProgressImage issue_image(recorded::ProgressStore& store, UtcTimestamp time)
    {
      auto image = next_image(store);
      auto& source = image._source;
      ++source._issuance;
      source._pinSequence = source._acknowledged + 1;
      source._pinIssuance = source._issuance;
      source._credit = source._pinSequence;
      source._dequeuedAt = time;
      image._receiptSequence = source._pinSequence;
      image._receipt = {recorded::ProgressReceiptState::Issued, source._actorIncarnation, source._issuance, time, std::nullopt};
      return image;
    }

    support::ProducerLogDirectory _directory;
    recorded::ProgressOptions _options;
  };

  class ErrorAtProgress final : public recorded::ProgressObserver
  {
  public:
    OperationResult checkpoint(recorded::ProgressCheckpoint point) override
    {
      if (_point && *_point == point)
      {
        _point.reset();
        return Error{ErrorCode::IoError, "Injected progress boundary error", "test"};
      }
      return std::monostate{};
    }

    std::optional<recorded::ProgressCheckpoint> _point;
  };
}

TEST_F(RecordedProgressTest, BootstrapHeaderAndIssuanceImageMatchIndependentBytes)
{
  auto file = require_value(mapped::NativeFile::open_existing(_options._path, mapped::FileAccess::ReadOnly));
  const auto identity = require_value(file.identity());
  std::vector<std::byte> expected(8192 + 8 * 64);
  constexpr std::array magic{'A', 'N', 'G', 'A', 'P', 'R', 'O', 'G'};
  for (std::size_t index = 0; index != magic.size(); ++index)
  {
    expected[index] = static_cast<std::byte>(magic[index]);
  }
  put_integer(expected, 8, 1, 2);
  put_integer(expected, 10, 0, 2);
  put_integer(expected, 12, 8192, 4);
  put_integer(expected, 16, 0x0102030405060708);
  put_integer(expected, 24, 0x1112131415161718);
  put_integer(expected, 32, 6, 2);
  put_integer(expected, 34, 2, 2);
  put_integer(expected, 36, 4, 2);
  put_integer(expected, 40, 9);
  put_integer(expected, 48, _options._connectionIdentity._device);
  put_integer(expected, 56, _options._connectionIdentity._inode);
  put_integer(expected, 64, 8);
  put_integer(expected, 72, identity._device);
  put_integer(expected, 80, identity._inode);
  put_integer(expected, 88, expected.size());
  put_integer(expected, 96, _options._liveAbiProfile, 4);
  put_integer(expected, 100, 2, 2);
  put_integer(expected, 102, 8, 2);
  put_integer(expected, 104, 1);
  EXPECT_EQ(stored_bytes(), expected);

  // Close the owner before comparing raw control bytes: no live writer races
  // these independent file fixtures or performs a non-atomic mapped read.
  {
    auto store = require_value(recorded::ProgressStore::open(_options));
    attach(store);
    require_value(store.commit(issue_image(store, UtcTimestamp{std::chrono::nanoseconds{-7}})));
  }
  put_integer(expected, 256, 2);
  put_integer(expected, 512, 2);
  std::array<std::byte, 80> attached{};
  put_integer(attached, 0, 7);
  auto issued = attached;
  put_integer(issued, 8, 1);
  put_integer(issued, 16, 1);
  put_integer(issued, 24, 1);
  put_integer(issued, 40, 1);
  put_integer(issued, 48, 1);
  put_integer(issued, 56, std::bit_cast<std::uint64_t>(std::int64_t{-7}));
  std::array<std::byte, 64> receipt{};
  put_integer(receipt, 0, 1);
  put_integer(receipt, 8, 7);
  put_integer(receipt, 16, 1);
  put_integer(receipt, 24, 1);
  put_integer(receipt, 32, std::bit_cast<std::uint64_t>(std::int64_t{-7}));
  std::copy(issued.begin(), issued.end(), expected.begin() + 1024);
  std::copy(receipt.begin(), receipt.end(), expected.begin() + 8192);
  for (const auto generation : {1u, 2u})
  {
    const auto offset = 4096 + (generation % 2) * 256;
    put_integer(expected, offset, generation);
    put_integer(expected, offset + 8, 0x0102030405060708);
    put_integer(expected, offset + 16, 0x1112131415161718);
    put_integer(expected, offset + 24, 9);
    put_integer(expected, offset + 32, 4, 2);
    put_integer(expected, offset + 34, 2, 2);
    put_integer(expected, offset + 36, 6, 2);
    const auto& source = generation == 1 ? attached : issued;
    std::copy(source.begin(), source.end(), expected.begin() + offset + 48);
    if (generation == 2)
    {
      put_integer(expected, offset + 40, 1);
      std::copy(receipt.begin(), receipt.end(), expected.begin() + offset + 128);
    }
  }
  EXPECT_EQ(stored_bytes(), expected);
}

TEST_F(RecordedProgressTest, ReleaseRetainsIssuanceAndAcknowledgmentPreservesExplicitTimePresence)
{
  auto store = require_value(recorded::ProgressStore::open(_options));
  attach(store);
  const UtcTimestamp firstTime{std::chrono::nanoseconds{-7}};
  require_value(store.commit(issue_image(store, firstTime)));
  auto release = next_image(store);
  release._source._pinSequence = 0;
  release._source._pinIssuance = 0;
  release._source._dequeuedAt.reset();
  release._receiptSequence = 1;
  release._receipt = require_value(store.receipt(1));
  release._receipt._state = recorded::ProgressReceiptState::Pending;
  require_value(store.commit(release));
  const auto pending = require_value(store.receipt(1));
  EXPECT_EQ(pending._dequeuedAt, firstTime);
  EXPECT_EQ(pending._issuance, 1u);
  EXPECT_EQ(require_value(store.snapshot())._source._credit, 1u);

  require_value(store.commit(issue_image(store, UtcTimestamp{})));
  auto acknowledgment = next_image(store);
  acknowledgment._source._pinSequence = 0;
  acknowledgment._source._pinIssuance = 0;
  acknowledgment._source._dequeuedAt.reset();
  acknowledgment._source._acknowledged = 1;
  acknowledgment._source._lastAcknowledgedAt = UtcTimestamp{std::chrono::nanoseconds{-9}};
  acknowledgment._receiptSequence = 1;
  acknowledgment._receipt = require_value(store.receipt(1));
  acknowledgment._receipt._state = recorded::ProgressReceiptState::Acknowledged;
  acknowledgment._receipt._acknowledgedAt = acknowledgment._source._lastAcknowledgedAt;
  require_value(store.commit(acknowledgment));
  const auto terminal = require_value(store.receipt(1));
  EXPECT_EQ(terminal._issuance, 2u);
  EXPECT_EQ(terminal._dequeuedAt, UtcTimestamp{});
  EXPECT_EQ(terminal._acknowledgedAt, UtcTimestamp{std::chrono::nanoseconds{-9}});
  EXPECT_EQ(require_value(store.snapshot())._source._lastAcknowledgedAt, terminal._acknowledgedAt);
  expect_error(store.receipt(0), ErrorCode::InvalidArgument);
  expect_error(store.receipt(9), ErrorCode::InvalidArgument);
}

TEST_F(RecordedProgressTest, FailedInactiveImagesDoNotCommitAndSelectedImagesSettleExactly)
{
  for (const auto point : {recorded::ProgressCheckpoint::PartialImage, recorded::ProgressCheckpoint::ImageWritten,
       recorded::ProgressCheckpoint::BeforeSelector, recorded::ProgressCheckpoint::SelectorPublished,
       recorded::ProgressCheckpoint::PartialReceipt, recorded::ProgressCheckpoint::ReceiptInstalled,
       recorded::ProgressCheckpoint::PartialSource, recorded::ProgressCheckpoint::SourceInstalled,
       recorded::ProgressCheckpoint::BeforeApplied, recorded::ProgressCheckpoint::AppliedPublished})
  {
    std::filesystem::remove(_options._path);
    require_value(recorded::ProgressStore::create(_options));
    ErrorAtProgress observer;
    auto store = require_value(recorded::ProgressStore::open(_options, &observer));
    attach(store);
    observer._point = point;
    const auto attempted = issue_image(store, UtcTimestamp{});
    const bool selected = point != recorded::ProgressCheckpoint::PartialImage &&
                          point != recorded::ProgressCheckpoint::ImageWritten &&
                          point != recorded::ProgressCheckpoint::BeforeSelector;
    expect_error(store.commit(attempted), selected ? ErrorCode::OutcomeUnknown : ErrorCode::IoError);
    if (selected)
    {
      expect_error(store.snapshot(), ErrorCode::IoError);
    }
    require_value(store.recover());
    auto snapshot = require_value(store.snapshot());
    EXPECT_EQ(snapshot._generation, selected ? 2u : 1u);
    EXPECT_EQ(snapshot._source._pinSequence, selected ? 1u : 0u);
    const auto receipt = require_value(store.receipt(1));
    EXPECT_EQ(receipt._state, selected ? recorded::ProgressReceiptState::Issued : recorded::ProgressReceiptState::Pending);
    if (selected)
    {
      EXPECT_EQ(receipt._dequeuedAt, UtcTimestamp{});
    }
    else
    {
      // Reusing the same inactive slot materializes the entire next image;
      // an earlier partial or complete unselected body has no authority.
      require_value(store.commit(attempted));
    }
    store = {};
    auto reopened = require_value(recorded::ProgressStore::open(_options));
    EXPECT_EQ(require_value(reopened.snapshot())._source._pinSequence, 1u);
    EXPECT_EQ(require_value(reopened.receipt(1))._dequeuedAt, UtcTimestamp{});
  }
}

TEST_F(RecordedProgressTest, UnsupportedOrMismatchedHeadersRejectWithoutWrites)
{
  const auto before = stored_bytes();
  auto mismatch = _options;
  ++mismatch._connectionGeneration;
  expect_error(recorded::ProgressStore::open(mismatch), ErrorCode::ConfigurationMismatch);
  EXPECT_EQ(stored_bytes(), before);
  auto file = require_value(mapped::NativeFile::open_existing(_options._path, mapped::FileAccess::ReadWrite));
  auto changed = before;
  put_integer(changed, 8, 2, 2);
  require_value(file.write_exact(0, changed));
  expect_error(recorded::ProgressStore::open(_options), ErrorCode::UnsupportedPolicy);
  EXPECT_EQ(stored_bytes(), changed);
}

TEST_F(RecordedProgressTest, ActualInodeClaimAndSelfIdentityPreventDuplicateProgressOwners)
{
  auto owner = require_value(recorded::ProgressStore::open(_options));
  expect_error(recorded::ProgressStore::open(_options), ErrorCode::OwnershipBusy);
  auto copied = _options;
  copied._path = _directory.path() / "copied-progress";
  std::filesystem::copy_file(_options._path, copied._path);
  expect_error(recorded::ProgressStore::open(copied), ErrorCode::ConfigurationMismatch);
  auto linked = _options;
  linked._path = _directory.path() / "linked-progress";
  std::filesystem::create_hard_link(_options._path, linked._path);
  expect_error(recorded::ProgressStore::open(linked), ErrorCode::IoError);
}

TEST_F(RecordedProgressTest, InvalidTransitionsLeaveSelectedProgressUnchanged)
{
  auto store = require_value(recorded::ProgressStore::open(_options));
  attach(store);
  auto invalid = issue_image(store, UtcTimestamp{});
  invalid._generation = 9;
  expect_error(store.commit(invalid), ErrorCode::InvalidArgument);
  EXPECT_EQ(require_value(store.snapshot())._generation, 1u);
  invalid = issue_image(store, UtcTimestamp{});
  invalid._source._credit = 2;
  expect_error(store.commit(invalid), ErrorCode::InvalidArgument);
  EXPECT_EQ(require_value(store.receipt(1)), recorded::ProgressReceipt{});
  require_value(store.commit(issue_image(store, UtcTimestamp{})));
}
