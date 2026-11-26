#pragma once

#include "angaria/descriptors.h"
#include "angaria/detail/mapped_format.h"
#include "angaria/results.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace angaria::detail::mapped
{
  inline constexpr std::size_t receipt_bytes = 128;

  enum class ReceiptState
  {
    Unused,
    Pending,
    Issued,
    Acknowledged,
    Lost
  };

  // A fixture must opt into loss explicitly. This is a codec context, not
  // admission of a new live Session capability or an executable transition.
  enum class ReceiptProfile
  {
    Core,
    AllowLoss
  };

  struct Receipt
  {
    ReceiptState _state{ReceiptState::Unused};
    TaskId _consumerTask{};
    SourceQueueSetId _sourceSet{};
    std::uint32_t _presence{};
    std::uint64_t _consumerIncarnation{};
    std::uint64_t _issuance{};
    UtcTimestamp _dequeuedAt{};
    UtcTimestamp _acknowledgedAt{};
    UtcTimestamp _terminalAt{};

    bool operator==(const Receipt&) const = default;
  };

  struct RingSlot
  {
    std::uint64_t _queueSequence{};
    std::uint64_t _deliveryLink{};
    std::uint64_t _publicationReference{};
    std::uint64_t _generation{};

    bool operator==(const RingSlot&) const = default;
  };

  using ReceiptBytes = std::array<std::byte, receipt_bytes>;
  using RingSlotBytes = std::array<std::byte, ring_slot_bytes>;

  // Operate on local byte copies only. A future live caller must copy mutable
  // words with the native atomic adapter under the Session gate after recovery.
  Result<ReceiptBytes> encode_receipt(const Receipt& receipt,
                                       ReceiptProfile profile = ReceiptProfile::Core);
  Result<Receipt> decode_receipt(std::span<const std::byte> bytes,
                                  ReceiptProfile profile = ReceiptProfile::Core);
  Result<RingSlotBytes> encode_ring_slot(const RingSlot& slot, std::uint64_t capacity,
                                         std::uint64_t index);
  Result<RingSlot> decode_ring_slot(std::span<const std::byte> bytes, std::uint64_t capacity,
                                    std::uint64_t index);

  // maxRecordBytes bounds the entire Ring record, including its 64-byte envelope.
  // Encoded/decoded spans below contain only the mutable suffix: C slots followed
  // by zero padding to 64 bytes. The caller supplies C independently of padding.
  Result<std::uint64_t> ring_record_bytes(std::uint64_t capacity,
                                          std::uint64_t maxRecordBytes);
  Result<std::vector<std::byte>> encode_ring_slots(std::span<const RingSlot> slots,
                                                   std::uint64_t lastAssigned,
                                                   std::uint64_t maxRecordBytes);
  Result<std::vector<RingSlot>> decode_ring_slots(std::span<const std::byte> bytes,
                                                  std::uint64_t capacity,
                                                  std::uint64_t lastAssigned,
                                                  std::uint64_t maxRecordBytes);
} // namespace angaria::detail::mapped
