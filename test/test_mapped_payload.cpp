// Private storage examples, not public send/receive. The shared fixture writes
// real committed Publication records with deliberately minimal catalog context.
// These tests show how a read-only payload owner survives its operation gate,
// while invalid references never become borrowed application spans.

#include "mapped_payload.h"
#include "process_support.h"
#include "publication_storage_fixture.h"
#include "result_assertions.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <utility>
#include <vector>

#include <sys/wait.h>
#include <unistd.h>

namespace
{
  using namespace angaria;
  namespace runtime = angaria::detail;
  namespace mapped = angaria::detail::mapped;
  using angaria::test_support::PublicationStorageFixture;
  using angaria::test_support::expect_error;
  using angaria::test_support::put_integer;
  using angaria::test_support::require_value;

  runtime::MappedPayload map_payload(PublicationStorageFixture& fixture)
  {
    runtime::RuntimeAccess access(fixture._state, false);
    return require_value(runtime::map_publication_payload(access, fixture._publication._reference));
  }

  std::vector<std::byte> image(PublicationStorageFixture& fixture)
  {
    auto file = require_value(mapped::NativeFile::open_existing(fixture._directory / "session.ang",
                                                               mapped::FileAccess::ReadOnly));
    std::vector<std::byte> bytes(static_cast<std::size_t>(require_value(file.size())));
    require_value(file.read_exact(0, bytes));
    return bytes;
  }

  class MetadataReadGuard final : public runtime::RuntimeAccessReadObserver
  {
  public:
    explicit MetadataReadGuard(const angaria::test_support::StoredPayloadPublication& publication)
        : _publication(publication)
    {
    }

    OperationResult before_read(std::uint64_t offset, std::uint64_t length) override
    {
      if (length != 0 && offset < _publication._payloadOffset + _publication._payloadBytes &&
          _publication._payloadOffset < offset + length)
      {
        return Error{ErrorCode::IoError, "Payload bytes must be mapped rather than copied", "test.payload"};
      }
      return std::monostate{};
    }

  private:
    const angaria::test_support::StoredPayloadPublication& _publication;
  };

  TEST(MappedPayloadTest, ExactApplicationSliceOutlivesGateAndController)
  {
    PublicationStorageFixture fixture;
    MetadataReadGuard observer(fixture._publication);
    runtime::MappedPayload payload;
    {
      runtime::RuntimeAccess access(fixture._state, false, nullptr, &observer);
      payload = require_value(runtime::map_publication_payload(access, fixture._publication._reference));
    }
    fixture._state.reset();

    // No Session, controller, or operation gate remains. The payload owns the
    // descriptor/mapping lease, and excludes every header, padding, and receipt.
    const auto bytes = require_value(payload.bytes());
    ASSERT_EQ(bytes.size(), fixture._publication._payloadBytes);
    EXPECT_NE(fixture._publication._payloadOffset % static_cast<std::uint64_t>(::sysconf(_SC_PAGESIZE)), 0u);
    EXPECT_EQ(bytes.front(), std::byte{0x41});
    EXPECT_EQ(bytes.back(), std::byte{0x5a});
    EXPECT_TRUE(std::all_of(bytes.begin() + 1, bytes.end() - 1,
                           [](std::byte value)
                           {
                             return value == std::byte{0xa5};
                           }));
  }

  TEST(MappedPayloadTest, MoveTransfersOwnershipAndInvalidatesMovedFromSpans)
  {
    PublicationStorageFixture fixture;
    auto original = map_payload(fixture);
    const auto* address = require_value(original.bytes()).data();
    runtime::MappedPayload moved(std::move(original));
    expect_error(original.bytes(), ErrorCode::InvalidState);
    EXPECT_EQ(require_value(moved.bytes()).data(), address);

    runtime::MappedPayload assigned;
    assigned = std::move(moved);
    expect_error(moved.bytes(), ErrorCode::InvalidState);
    EXPECT_EQ(require_value(assigned.bytes()).data(), address);
    assigned = runtime::MappedPayload{};
    expect_error(assigned.bytes(), ErrorCode::InvalidState);
  }

  TEST(MappedPayloadTest, EmptyPayloadHasAValidOwnerWithoutANativeZeroLengthMapping)
  {
    PublicationStorageFixture fixture(0);
    auto payload = map_payload(fixture);
    fixture._state.reset();
    const auto bytes = require_value(payload.bytes());
    EXPECT_TRUE(bytes.empty());
    EXPECT_EQ(bytes.data(), nullptr);
    runtime::MappedPayload moved(std::move(payload));
    expect_error(payload.bytes(), ErrorCode::InvalidState);
    EXPECT_TRUE(require_value(moved.bytes()).empty());
  }

  TEST(MappedPayloadTest, RejectsInteriorWrongKindAndUncommittedReferencesWithoutWrites)
  {
    PublicationStorageFixture fixture;
    const auto before = image(fixture);
    runtime::RuntimeAccess access(fixture._state, false);
    const auto arena = mapped::make_fixed_layout(fixture._configuration._limits)->_arenaBegin;
    const std::array references{std::uint64_t{0}, arena, fixture._publication._reference + 64,
                                fixture._publication._receiptOffset, access._snapshot._tail};
    for (const auto reference : references)
    {
      expect_error(runtime::map_publication_payload(access, reference), ErrorCode::IoError);
    }
    EXPECT_EQ(image(fixture), before);
  }

  TEST(MappedPayloadTest, CorruptPayloadGeometryCannotExposeMutableReceipts)
  {
    PublicationStorageFixture fixture;
    runtime::RuntimeAccess access(fixture._state, false);
    // Deliberate quiescent corruption: redirect the logical payload offset at a
    // receipt. Canonical metadata geometry must reject it before calling mmap.
    std::array<std::byte, 8> offset{};
    put_integer(offset, 0, fixture._publication._receiptOffset);
    require_value(access._file->write_exact(fixture._publication._reference + 64 + 56, offset));
    const auto before = image(fixture);
    expect_error(runtime::map_publication_payload(access, fixture._publication._reference), ErrorCode::IoError);
    EXPECT_EQ(image(fixture), before);
  }

  TEST(MappedPayloadTest, CorruptImmutablePaddingRejectsBeforeMapping)
  {
    PublicationStorageFixture fixture;
    runtime::RuntimeAccess access(fixture._state, false);
    const auto payloadEnd = fixture._publication._payloadOffset + fixture._publication._payloadBytes;
    ASSERT_LT(payloadEnd, fixture._publication._receiptOffset);
    const std::array nonzero{std::byte{0xff}};
    require_value(access._file->write_exact(payloadEnd, nonzero));
    const auto before = image(fixture);
    expect_error(runtime::map_publication_payload(access, fixture._publication._reference), ErrorCode::IoError);
    EXPECT_EQ(image(fixture), before);
  }

  TEST(MappedPayloadTest, MappingRequiresTheNativeDeliveryCapability)
  {
    PublicationStorageFixture fixture;
    fixture._state.reset();
    // This is a private storage fixture, so changing its quiescent profile is
    // deliberate incompatibility setup rather than a supported Session upgrade.
    auto file = require_value(mapped::NativeFile::open_existing(fixture._directory / "session.ang",
                                                               mapped::FileAccess::ReadWrite));
    std::array<std::byte, 8> word{};
    put_integer(word, 0, 1);
    require_value(file.write_exact(10, std::span{word}.first(2)));
    put_integer(word, 0, 3);
    require_value(file.write_exact(56, word));
    file = {};
    fixture._state = angaria::test_support::open_runtime_state(fixture._directory);
    const auto before = image(fixture);
    runtime::RuntimeAccess access(fixture._state, false);
    expect_error(runtime::map_publication_payload(access, fixture._publication._reference), ErrorCode::UnsupportedPolicy);
    EXPECT_EQ(image(fixture), before);
  }

  TEST(MappedPayloadTest, EmptyAndNonemptyOwnersRejectInheritedProcessUse)
  {
    PublicationStorageFixture nonemptyFixture;
    PublicationStorageFixture emptyFixture(0);
    auto nonempty = map_payload(nonemptyFixture);
    auto empty = map_payload(emptyFixture);
    // As in native ownership qualification, a quiescent fork child checks only
    // rejection and calls _exit. Applications exec before using Angaria there.
    const auto child = ::fork();
    ASSERT_NE(child, -1);
    if (child == 0)
    {
      const auto wrong_owner = [](const auto& result)
      {
        const auto* error = std::get_if<Error>(&result);
        return error != nullptr && error->_code == ErrorCode::WrongOwner;
      };
      ::_exit(wrong_owner(nonempty.bytes()) && wrong_owner(empty.bytes()) ? 0 : 1);
    }
    const auto status = angaria::test_support::wait_for_exit(
        child, "Cannot wait for payload ownership child", "Payload ownership child did not exit");
    ASSERT_TRUE(WIFEXITED(status));
    EXPECT_EQ(WEXITSTATUS(status), 0);
    EXPECT_FALSE(require_value(nonempty.bytes()).empty());
    EXPECT_TRUE(require_value(empty.bytes()).empty());
  }
} // namespace
