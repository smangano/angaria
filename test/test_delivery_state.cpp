// Private native-delivery representation fixtures. These tests explain stored
// receipt/ring invariants using local byte copies; they do not demonstrate live
// Session delivery. Golden bytes come from the documented layout independently
// of the production encoders. Reference existence and Queue ownership follow in
// the Session/history validation step.

#include "delivery_state.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace
{
  using namespace angaria;
  namespace mapped = angaria::detail::mapped;

  template <typename T>
  T require_value(Result<T> result)
  {
    if (const auto* error = std::get_if<Error>(&result))
    {
      throw std::runtime_error(error->_path + ": " + error->_message);
    }
    return std::get<T>(std::move(result));
  }

  template <typename T>
  void expect_error(const Result<T>& result, ErrorCode code)
  {
    ASSERT_TRUE(std::holds_alternative<Error>(result));
    const auto& error = std::get<Error>(result);
    EXPECT_EQ(error._code, code) << error._message;
    EXPECT_FALSE(error._path.empty());
    EXPECT_FALSE(error._message.empty());
  }

  void put_integer(std::span<std::byte> bytes, std::size_t offset,
                   std::uint64_t value, std::size_t width = 8)
  {
    for (std::size_t byte = 0; byte < width; ++byte)
    {
      bytes[offset + byte] = static_cast<std::byte>((value >> (8 * byte)) & 0xff);
    }
  }

  mapped::Receipt acknowledged_receipt()
  {
    mapped::Receipt receipt;
    receipt._state = mapped::ReceiptState::Acknowledged;
    receipt._consumerTask = TaskId{65535};
    receipt._sourceSet = SourceQueueSetId{0x1234};
    receipt._presence = 7;
    receipt._consumerIncarnation = 0xfedcba9876543210u;
    receipt._issuance = std::numeric_limits<std::uint64_t>::max();
    receipt._dequeuedAt = UtcTimestamp{std::chrono::nanoseconds{-7}};
    // A backward-moving wall clock is legal; terminal time is not ordered
    // against the historical dequeue time.
    receipt._acknowledgedAt = UtcTimestamp{std::chrono::nanoseconds{std::numeric_limits<std::int64_t>::min()}};
    receipt._terminalAt = receipt._acknowledgedAt;
    return receipt;
  }

  mapped::ReceiptBytes golden_receipt()
  {
    mapped::ReceiptBytes bytes{};
    put_integer(bytes, 0, 3);
    put_integer(bytes, 8, 65535, 2);
    put_integer(bytes, 10, 0x1234, 2);
    put_integer(bytes, 12, 7, 4);
    put_integer(bytes, 16, 0xfedcba9876543210u);
    put_integer(bytes, 24, 0xffffffffffffffffu);
    put_integer(bytes, 32, 0xfffffffffffffff9u);
    put_integer(bytes, 40, 0x8000000000000000u);
    put_integer(bytes, 48, 0x8000000000000000u);
    return bytes;
  }

  mapped::RingSlot slot(std::uint64_t sequence, std::uint64_t capacity)
  {
    return mapped::RingSlot{sequence, 8384, 8192, 1 + (sequence - 1) / capacity};
  }

  void put_slot(std::span<std::byte> bytes, std::size_t index, std::uint64_t sequence,
                std::uint64_t generation)
  {
    const auto offset = index * 32;
    put_integer(bytes, offset, sequence);
    put_integer(bytes, offset + 8, 8384);
    put_integer(bytes, offset + 16, 8192);
    put_integer(bytes, offset + 24, generation);
  }
} // namespace

TEST(NativeReceiptCodec, IndependentGoldenBytesPreserveWidthsAndSignedTimes)
{
  const auto golden = golden_receipt();
  const auto expected = acknowledged_receipt();
  EXPECT_EQ(require_value(mapped::encode_receipt(expected)), golden);
  EXPECT_EQ(require_value(mapped::decode_receipt(golden)), expected);
}

TEST(NativeReceiptCodec, UnusedAndInitialPendingHaveNoInventedConsumer)
{
  const mapped::Receipt unused;
  EXPECT_EQ(require_value(mapped::encode_receipt(unused)), mapped::ReceiptBytes{});
  auto pending = unused;
  pending._state = mapped::ReceiptState::Pending;
  mapped::ReceiptBytes golden{};
  golden[0] = std::byte{1};
  EXPECT_EQ(require_value(mapped::encode_receipt(pending)), golden);
  EXPECT_EQ(require_value(mapped::decode_receipt(golden)), pending);
}

TEST(NativeReceiptCodec, PresenceMakesEpochZeroARealTime)
{
  auto receipt = acknowledged_receipt();
  receipt._dequeuedAt = UtcTimestamp{};
  receipt._acknowledgedAt = UtcTimestamp{};
  receipt._terminalAt = UtcTimestamp{};
  auto decoded = require_value(mapped::decode_receipt(require_value(mapped::encode_receipt(receipt))));
  EXPECT_EQ(decoded, receipt);
  EXPECT_EQ(decoded._presence, 7u);
}

TEST(NativeReceiptCodec, ReleasedPendingRetainsHistoricalIdentity)
{
  // Release ends the pin but preserves the last issuance identity so cleanup
  // replay can find it. The codec represents this state without executing it.
  auto receipt = acknowledged_receipt();
  receipt._state = mapped::ReceiptState::Pending;
  receipt._presence = 1;
  receipt._acknowledgedAt = UtcTimestamp{};
  receipt._terminalAt = UtcTimestamp{};
  EXPECT_EQ(require_value(mapped::decode_receipt(require_value(mapped::encode_receipt(receipt)))), receipt);
  receipt._state = mapped::ReceiptState::Issued;
  EXPECT_EQ(require_value(mapped::decode_receipt(require_value(mapped::encode_receipt(receipt)))), receipt);
}

TEST(NativeReceiptCodec, ExhaustiveStatePresenceMatrixRejectsImpossibleCombinations)
{
  for (std::uint64_t state = 0; state <= 4; ++state)
  {
    for (std::uint32_t presence = 0; presence < 16; ++presence)
    {
      SCOPED_TRACE(std::to_string(state) + "/" + std::to_string(presence));
      mapped::ReceiptBytes bytes{};
      put_integer(bytes, 0, state);
      put_integer(bytes, 12, presence, 4);
      if ((presence & 1) != 0)
      {
        put_integer(bytes, 8, 1, 2);
        put_integer(bytes, 10, 2, 2);
        put_integer(bytes, 16, 3);
        put_integer(bytes, 24, 4);
      }
      const auto valid = (state == 0 && presence == 0) ||
                         (state == 1 && (presence == 0 || presence == 1)) ||
                         (state == 2 && presence == 1) ||
                         (state == 3 && presence == 7) ||
                         (state == 4 && (presence == 4 || presence == 5));
      const auto decoded = mapped::decode_receipt(bytes, mapped::ReceiptProfile::AllowLoss);
      if (valid)
      {
        ASSERT_TRUE(std::holds_alternative<mapped::Receipt>(decoded));
        EXPECT_EQ(require_value(mapped::encode_receipt(std::get<mapped::Receipt>(decoded),
                                                       mapped::ReceiptProfile::AllowLoss)), bytes);
      }
      else
      {
        expect_error(decoded, ErrorCode::IoError);
      }
    }
  }
}

TEST(NativeReceiptCodec, LossRequiresExplicitCodecProfile)
{
  // This private profile flag exercises representation admission. It does not
  // advertise allowed loss on currently created native Sessions.
  mapped::Receipt lost;
  lost._state = mapped::ReceiptState::Lost;
  lost._presence = 4;
  lost._terminalAt = UtcTimestamp{std::chrono::nanoseconds{std::numeric_limits<std::int64_t>::max()}};
  const auto bytes = require_value(mapped::encode_receipt(lost, mapped::ReceiptProfile::AllowLoss));
  EXPECT_EQ(require_value(mapped::decode_receipt(bytes, mapped::ReceiptProfile::AllowLoss)), lost);
  expect_error(mapped::encode_receipt(lost), ErrorCode::InvalidArgument);
  expect_error(mapped::decode_receipt(bytes), ErrorCode::IoError);
  expect_error(mapped::encode_receipt({}, static_cast<mapped::ReceiptProfile>(99)), ErrorCode::InvalidArgument);
  expect_error(mapped::decode_receipt(mapped::ReceiptBytes{}, static_cast<mapped::ReceiptProfile>(99)), ErrorCode::IoError);
}

TEST(NativeReceiptCodec, CompleteConsumerTagsAreRequiredInEveryHistoricalState)
{
  const auto golden = golden_receipt();
  for (const auto offset : {8u, 10u, 16u, 24u})
  {
    auto bytes = golden;
    put_integer(bytes, offset, 0, offset < 16 ? 2 : 8);
    expect_error(mapped::decode_receipt(bytes), ErrorCode::IoError);
  }
  auto receipt = acknowledged_receipt();
  receipt._consumerTask = TaskId::Invalid;
  expect_error(mapped::encode_receipt(receipt), ErrorCode::InvalidArgument);
}

TEST(NativeReceiptCodec, AbsentMetadataAndMismatchedAcknowledgmentAreRejected)
{
  mapped::ReceiptBytes initial{};
  put_integer(initial, 0, 1);
  for (const auto offset : {8u, 10u, 16u, 24u, 32u, 40u, 48u})
  {
    auto bytes = initial;
    bytes[offset] = std::byte{1};
    expect_error(mapped::decode_receipt(bytes), ErrorCode::IoError);
  }
  auto receipt = acknowledged_receipt();
  receipt._terminalAt = UtcTimestamp{};
  expect_error(mapped::encode_receipt(receipt), ErrorCode::InvalidArgument);
  auto bytes = golden_receipt();
  bytes[48] = std::byte{1};
  expect_error(mapped::decode_receipt(bytes), ErrorCode::IoError);
}

TEST(NativeReceiptCodec, LengthStateAndEveryReservedByteAreChecked)
{
  const auto golden = golden_receipt();
  for (std::size_t length = 0; length < golden.size(); ++length)
  {
    expect_error(mapped::decode_receipt(std::span{golden}.first(length)), ErrorCode::IoError);
  }
  std::vector<std::byte> oversized(129);
  expect_error(mapped::decode_receipt(oversized), ErrorCode::IoError);
  auto bytes = golden;
  put_integer(bytes, 0, 0x100000003u);
  expect_error(mapped::decode_receipt(bytes), ErrorCode::IoError);
  for (std::size_t offset = 56; offset < golden.size(); ++offset)
  {
    bytes = golden;
    bytes[offset] = std::byte{1};
    expect_error(mapped::decode_receipt(bytes), ErrorCode::IoError);
  }
  auto receipt = acknowledged_receipt();
  receipt._state = static_cast<mapped::ReceiptState>(99);
  expect_error(mapped::encode_receipt(receipt), ErrorCode::InvalidArgument);
}

TEST(NativeRingCodec, IndependentGoldenSlotPreservesAllFourWords)
{
  mapped::RingSlotBytes golden{};
  put_slot(golden, 0, 8, 3);
  const auto expected = slot(8, 3);
  EXPECT_EQ(require_value(mapped::encode_ring_slot(expected, 3, 1)), golden);
  EXPECT_EQ(require_value(mapped::decode_ring_slot(golden, 3, 1)), expected);
}

TEST(NativeRingCodec, CapacityOneSupportsMaximumSequenceAndGeneration)
{
  // There is no packed valid bit: the highest sequence and generation remain
  // representable, rather than being silently limited to 63 bits.
  constexpr auto maximum = std::numeric_limits<std::uint64_t>::max();
  mapped::RingSlotBytes golden{};
  put_slot(golden, 0, maximum, maximum);
  const auto expected = slot(maximum, 1);
  EXPECT_EQ(require_value(mapped::encode_ring_slot(expected, 1, 0)), golden);
  EXPECT_EQ(require_value(mapped::decode_ring_slot(golden, 1, 0)), expected);
  const std::array slots{expected};
  const auto encoded = require_value(mapped::encode_ring_slots(slots, maximum, 128));
  EXPECT_EQ(require_value(mapped::decode_ring_slots(encoded, 1, maximum, 128)),
            std::vector<mapped::RingSlot>{expected});
}

TEST(NativeRingCodec, EmptySlotRequiresEveryWordZero)
{
  EXPECT_EQ(require_value(mapped::encode_ring_slot({}, 1, 0)), mapped::RingSlotBytes{});
  for (const auto offset : {8u, 16u, 24u})
  {
    mapped::RingSlotBytes bytes{};
    put_integer(bytes, offset, 8192);
    expect_error(mapped::decode_ring_slot(bytes, 1, 0), ErrorCode::IoError);
  }
}

TEST(NativeRingCodec, InvalidCapacityIndexGenerationAndReferencesAreRejected)
{
  const auto valid = slot(8, 3);
  expect_error(mapped::encode_ring_slot(valid, 0, 0), ErrorCode::InvalidArgument);
  expect_error(mapped::encode_ring_slot(valid, 3, 3), ErrorCode::InvalidArgument);
  expect_error(mapped::encode_ring_slot(valid, 3, 0), ErrorCode::InvalidArgument);
  auto invalid = valid;
  invalid._generation = 2;
  expect_error(mapped::encode_ring_slot(invalid, 3, 1), ErrorCode::InvalidArgument);
  const auto golden = require_value(mapped::encode_ring_slot(valid, 3, 1));
  for (const auto reference : {0u, 1u, 4095u, 8193u})
  {
    auto bytes = golden;
    put_integer(bytes, 8, reference);
    expect_error(mapped::decode_ring_slot(bytes, 3, 1), ErrorCode::IoError);
    bytes = golden;
    put_integer(bytes, 16, reference);
    expect_error(mapped::decode_ring_slot(bytes, 3, 1), ErrorCode::IoError);
  }
  for (const auto offset : {8u, 16u})
  {
    auto bytes = golden;
    put_integer(bytes, offset, std::numeric_limits<std::uint64_t>::max() - 7);
    expect_error(mapped::decode_ring_slot(bytes, 3, 1), ErrorCode::IoError);
  }
  for (std::size_t length = 0; length < golden.size(); ++length)
  {
    expect_error(mapped::decode_ring_slot(std::span{golden}.first(length), 3, 1), ErrorCode::IoError);
  }
  expect_error(mapped::decode_ring_slot(std::vector<std::byte>(33), 3, 1), ErrorCode::IoError);
}

TEST(NativeRingCodec, WrappedWindowUsesLogicalCapacityAndZeroAllocationPadding)
{
  // C=3, N=8 retains sequences 6..8. Physical order is 7,8,6; the final
  // 32 bytes complete the allocation and do not add a fourth logical slot.
  std::vector<std::byte> golden(128);
  put_slot(golden, 0, 7, 3);
  put_slot(golden, 1, 8, 3);
  put_slot(golden, 2, 6, 2);
  const std::array expected{slot(7, 3), slot(8, 3), slot(6, 3)};
  EXPECT_EQ(require_value(mapped::ring_record_bytes(3, 192)), 192u);
  EXPECT_EQ(require_value(mapped::encode_ring_slots(expected, 8, 192)), golden);
  EXPECT_EQ(require_value(mapped::decode_ring_slots(golden, 3, 8, 192)),
            (std::vector<mapped::RingSlot>{expected.begin(), expected.end()}));
  for (std::size_t offset = 96; offset < golden.size(); ++offset)
  {
    auto bytes = golden;
    bytes[offset] = std::byte{1};
    expect_error(mapped::decode_ring_slots(bytes, 3, 8, 192), ErrorCode::IoError);
  }
}

TEST(NativeRingCodec, InitialAndPartlyFilledWindowsKeepUnassignedSlotsEmpty)
{
  const std::array<mapped::RingSlot, 3> empty{};
  const std::vector<std::byte> goldenEmpty(128);
  EXPECT_EQ(require_value(mapped::encode_ring_slots(empty, 0, 192)), goldenEmpty);
  EXPECT_EQ(require_value(mapped::decode_ring_slots(goldenEmpty, 3, 0, 192)),
            (std::vector<mapped::RingSlot>{empty.begin(), empty.end()}));
  std::vector<std::byte> goldenPartial(128);
  put_slot(goldenPartial, 0, 1, 1);
  put_slot(goldenPartial, 1, 2, 1);
  const std::array partial{slot(1, 3), slot(2, 3), mapped::RingSlot{}};
  EXPECT_EQ(require_value(mapped::encode_ring_slots(partial, 2, 192)), goldenPartial);
  EXPECT_EQ(require_value(mapped::decode_ring_slots(goldenPartial, 3, 2, 192)),
            (std::vector<mapped::RingSlot>{partial.begin(), partial.end()}));
}

TEST(NativeRingCodec, IndividuallyValidStaleSlotsDoNotEstablishCurrentWindow)
{
  std::array stale{slot(4, 3), slot(8, 3), slot(6, 3)};
  // Sequence 4 has a valid index and generation, but N=8 requires 7 here.
  ASSERT_TRUE(std::holds_alternative<mapped::RingSlotBytes>(mapped::encode_ring_slot(stale[0], 3, 0)));
  expect_error(mapped::encode_ring_slots(stale, 8, 192), ErrorCode::InvalidArgument);
  std::vector<std::byte> bytes(128);
  put_slot(bytes, 0, 4, 2);
  put_slot(bytes, 1, 8, 3);
  put_slot(bytes, 2, 6, 2);
  expect_error(mapped::decode_ring_slots(bytes, 3, 8, 192), ErrorCode::IoError);
  const std::array tooEarly{slot(1, 3), mapped::RingSlot{}, mapped::RingSlot{}};
  expect_error(mapped::encode_ring_slots(tooEarly, 0, 192), ErrorCode::InvalidArgument);
}

TEST(NativeRingCodec, MaximumWindowWithSeveralSlotsDoesNotWrapArithmetic)
{
  constexpr auto maximum = std::numeric_limits<std::uint64_t>::max();
  std::array<mapped::RingSlot, 3> slots;
  for (const auto sequence : {maximum - 2, maximum - 1, maximum})
  {
    slots[(sequence - 1) % slots.size()] = slot(sequence, slots.size());
  }
  const auto encoded = require_value(mapped::encode_ring_slots(slots, maximum, 192));
  EXPECT_EQ(require_value(mapped::decode_ring_slots(encoded, 3, maximum, 192)),
            (std::vector<mapped::RingSlot>{slots.begin(), slots.end()}));
}

TEST(NativeRingCodec, CompleteAllocationIsBoundedBeforeDecodeAllocation)
{
  constexpr auto maximum = std::numeric_limits<std::uint64_t>::max();
  expect_error(mapped::ring_record_bytes(0, 192), ErrorCode::InvalidArgument);
  expect_error(mapped::ring_record_bytes(3, 191), ErrorCode::InvalidArgument);
  expect_error(mapped::ring_record_bytes(3, 0), ErrorCode::InvalidArgument);
  expect_error(mapped::ring_record_bytes(3, maximum), ErrorCode::InvalidArgument);
  expect_error(mapped::ring_record_bytes(maximum, mapped::max_log_bytes), ErrorCode::InvalidArgument);
  expect_error(mapped::ring_record_bytes(maximum / 32, mapped::max_log_bytes), ErrorCode::InvalidArgument);
  expect_error(mapped::decode_ring_slots({}, maximum, 0, mapped::max_log_bytes), ErrorCode::IoError);
  expect_error(mapped::decode_ring_slots({}, 0, 0, 192), ErrorCode::IoError);
  const std::array<mapped::RingSlot, 3> empty{};
  expect_error(mapped::encode_ring_slots(empty, 0, 191), ErrorCode::InvalidArgument);
  const std::vector<std::byte> bytes(128);
  expect_error(mapped::decode_ring_slots(bytes, 3, 0, 191), ErrorCode::IoError);
  expect_error(mapped::decode_ring_slots(std::span{bytes}.first(127), 3, 0, 192), ErrorCode::IoError);
  expect_error(mapped::decode_ring_slots(std::vector<std::byte>(192), 3, 0, 256), ErrorCode::IoError);
  expect_error(mapped::decode_ring_slots(bytes, 5, 0, 256), ErrorCode::IoError);
}
