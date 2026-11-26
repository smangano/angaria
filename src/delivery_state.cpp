#include "delivery_state.h"

#include "angaria/detail/byte_operations.h"
#include "angaria/detail/checked_arithmetic.h"
#include "angaria/detail/error_support.h"

#include <algorithm>
#include <bit>
#include <chrono>
#include <limits>
#include <new>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

namespace angaria::detail::mapped
{
  namespace
  {
    using angaria::detail::CodecFailure;
    using angaria::detail::all_zero;
    using angaria::detail::codec_require;
    using angaria::detail::read_unsigned_le;
    using angaria::detail::write_unsigned_le;

    constexpr std::uint64_t envelope_bytes = 64;

    template <typename T, typename Function>
    Result<T> checked_codec(Function&& function)
    {
      try
      {
        return function();
      }
      catch (CodecFailure& failure)
      {
        return std::move(failure._error);
      }
      catch (const std::bad_alloc&)
      {
        return Error{ErrorCode::CapacityExceeded, "Delivery codec allocation failed", "delivery"};
      }
      catch (const std::length_error&)
      {
        return Error{ErrorCode::CapacityExceeded, "Delivery codec allocation exceeds host bounds", "delivery"};
      }
    }

    std::uint64_t state_code(ReceiptState state, ErrorCode code)
    {
      switch (state)
      {
      case ReceiptState::Unused: return 0;
      case ReceiptState::Pending: return 1;
      case ReceiptState::Issued: return 2;
      case ReceiptState::Acknowledged: return 3;
      case ReceiptState::Lost: return 4;
      }
      codec_require(false, code, "receipt.state", "Unknown receipt state");
      return 0;
    }

    ReceiptState read_state(std::uint64_t value, ErrorCode code)
    {
      switch (value)
      {
      case 0: return ReceiptState::Unused;
      case 1: return ReceiptState::Pending;
      case 2: return ReceiptState::Issued;
      case 3: return ReceiptState::Acknowledged;
      case 4: return ReceiptState::Lost;
      }
      codec_require(false, code, "receipt.state", "Unknown receipt state code");
      return ReceiptState::Unused;
    }

    void check_receipt(const Receipt& receipt, ReceiptProfile profile, ErrorCode code)
    {
      codec_require(profile == ReceiptProfile::Core || profile == ReceiptProfile::AllowLoss,
              code, "receipt.profile", "Unknown receipt codec profile");
      state_code(receipt._state, code);
      codec_require((receipt._presence & ~std::uint32_t{7}) == 0,
              code, "receipt.presence", "Unknown receipt presence bits");

      const auto history = (receipt._presence & 1) != 0;
      if (history)
      {
        codec_require(receipt._consumerTask != TaskId::Invalid &&
                    receipt._sourceSet != SourceQueueSetId::Invalid &&
                    receipt._consumerIncarnation != 0 && receipt._issuance != 0,
                code, "receipt.consumer", "Present dequeue metadata requires complete consumer tags");
      }
      else
      {
        codec_require(receipt._consumerTask == TaskId::Invalid &&
                    receipt._sourceSet == SourceQueueSetId::Invalid &&
                    receipt._consumerIncarnation == 0 && receipt._issuance == 0 &&
                    receipt._dequeuedAt == UtcTimestamp{},
                code, "receipt.consumer", "Absent dequeue metadata must be zero");
      }
      codec_require((receipt._presence & 2) != 0 || receipt._acknowledgedAt == UtcTimestamp{},
              code, "receipt.acknowledged_at", "Absent acknowledgment time must be zero");
      codec_require((receipt._presence & 4) != 0 || receipt._terminalAt == UtcTimestamp{},
              code, "receipt.terminal_at", "Absent terminal time must be zero");

      switch (receipt._state)
      {
      case ReceiptState::Unused:
        codec_require(receipt._presence == 0, code, "receipt.presence", "Unused receipt must be entirely zero");
        break;
      case ReceiptState::Pending:
        codec_require(receipt._presence == 0 || receipt._presence == 1,
                code, "receipt.presence", "Pending receipt may retain dequeue metadata only");
        break;
      case ReceiptState::Issued:
        codec_require(receipt._presence == 1, code, "receipt.presence", "Issued receipt requires dequeue metadata only");
        break;
      case ReceiptState::Acknowledged:
        codec_require(receipt._presence == 7 && receipt._acknowledgedAt == receipt._terminalAt,
                code, "receipt.presence", "Acknowledged receipt requires matching acknowledgment and terminal times");
        break;
      case ReceiptState::Lost:
        codec_require(profile == ReceiptProfile::AllowLoss, code, "receipt.profile", "Lost receipt requires the allowed-loss profile");
        codec_require(receipt._presence == 4 || receipt._presence == 5,
                code, "receipt.presence", "Lost receipt requires terminal time and optional dequeue metadata");
        break;
      }
    }

    void check_slot(const RingSlot& slot, std::uint64_t capacity, std::uint64_t index,
                    ErrorCode code)
    {
      codec_require(capacity != 0 && index < capacity, code, "ring.slot", "Invalid ring capacity or slot index");
      if (slot._queueSequence == 0)
      {
        codec_require(slot._deliveryLink == 0 && slot._publicationReference == 0 && slot._generation == 0,
                code, "ring.slot", "Empty ring slot must be entirely zero");
        return;
      }
      codec_require(slot._publicationReference >= superblock_bytes &&
                  slot._publicationReference % section_alignment == 0 &&
                  contains_range(max_log_bytes, slot._publicationReference, envelope_bytes),
              code, "ring.publication_reference", "Invalid Publication record reference");
      codec_require(slot._deliveryLink >= superblock_bytes && slot._deliveryLink % record_alignment == 0 &&
                  contains_range(max_log_bytes, slot._deliveryLink, 48),
              code, "ring.delivery_link", "Invalid destination row reference");
      const auto sequenceIndex = slot._queueSequence - 1;
      codec_require(sequenceIndex % capacity == index && slot._generation == 1 + sequenceIndex / capacity,
              code, "ring.generation", "Ring sequence, index, and generation disagree");
    }

    std::uint64_t record_bytes(std::uint64_t capacity, std::uint64_t limit, ErrorCode code)
    {
      codec_require(capacity != 0 && valid_log_limit(limit), code, "ring.capacity", "Invalid ring capacity or record ceiling");
      const auto slotBytes = checked_multiply(capacity, ring_slot_bytes);
      const auto suffixBytes = slotBytes ? checked_align(*slotBytes, section_alignment) : std::nullopt;
      const auto total = suffixBytes ? checked_add(envelope_bytes, *suffixBytes) : std::nullopt;
      codec_require(total && *total <= limit && *total <= max_log_bytes,
              code, "ring.length", "Ring allocation exceeds its record ceiling");
      const auto hostBytes = checked_multiply(capacity, sizeof(RingSlot));
      codec_require(hostBytes && fits_host_size(*hostBytes) && fits_host_size(*suffixBytes),
              code, "ring.length", "Ring allocation exceeds host bounds");
      return *total;
    }

    RingSlot read_slot(std::span<const std::byte> bytes)
    {
      return RingSlot{read_unsigned_le<std::uint64_t>(bytes, 0), read_unsigned_le<std::uint64_t>(bytes, 8),
                      read_unsigned_le<std::uint64_t>(bytes, 16), read_unsigned_le<std::uint64_t>(bytes, 24)};
    }

    void write_slot(std::span<std::byte> bytes, const RingSlot& slot)
    {
      write_unsigned_le(bytes, 0, slot._queueSequence);
      write_unsigned_le(bytes, 8, slot._deliveryLink);
      write_unsigned_le(bytes, 16, slot._publicationReference);
      write_unsigned_le(bytes, 24, slot._generation);
    }

    void check_window_slot(const RingSlot& slot, std::uint64_t capacity,
                           std::uint64_t index, std::uint64_t lastAssigned, ErrorCode code)
    {
      check_slot(slot, capacity, index, code);
      // Until the first wrap, indices at or after N have never been assigned.
      if (lastAssigned < capacity && index >= lastAssigned)
      {
        codec_require(slot._queueSequence == 0, code, "ring.window", "Unassigned ring slot must be empty");
        return;
      }
      const auto lastIndex = (lastAssigned - 1) % capacity;
      const auto distance = lastIndex >= index ? lastIndex - index : capacity - (index - lastIndex);
      const auto expected = lastAssigned - distance;
      codec_require(slot._queueSequence == expected, code, "ring.window", "Ring slot is outside the authoritative latest window");
    }
  } // namespace

  Result<ReceiptBytes> encode_receipt(const Receipt& receipt, ReceiptProfile profile)
  {
    return checked_codec<ReceiptBytes>([&]
    {
      check_receipt(receipt, profile, ErrorCode::InvalidArgument);
      ReceiptBytes bytes{};
      write_unsigned_le(bytes, 0, state_code(receipt._state, ErrorCode::InvalidArgument));
      write_unsigned_le(bytes, 8, static_cast<std::uint16_t>(receipt._consumerTask));
      write_unsigned_le(bytes, 10, static_cast<std::uint16_t>(receipt._sourceSet));
      write_unsigned_le(bytes, 12, receipt._presence);
      write_unsigned_le(bytes, 16, receipt._consumerIncarnation);
      write_unsigned_le(bytes, 24, receipt._issuance);
      write_unsigned_le(bytes, 32, std::bit_cast<std::uint64_t>(receipt._dequeuedAt.time_since_epoch().count()));
      write_unsigned_le(bytes, 40, std::bit_cast<std::uint64_t>(receipt._acknowledgedAt.time_since_epoch().count()));
      write_unsigned_le(bytes, 48, std::bit_cast<std::uint64_t>(receipt._terminalAt.time_since_epoch().count()));
      return bytes;
    });
  }

  Result<Receipt> decode_receipt(std::span<const std::byte> bytes, ReceiptProfile profile)
  {
    return checked_codec<Receipt>([&]
    {
      constexpr auto code = ErrorCode::IoError;
      codec_require(bytes.size() == receipt_bytes, code, "receipt.length", "Receipt must contain exactly 128 bytes");
      codec_require(all_zero(bytes.subspan(56)), code, "receipt.reserved", "Reserved receipt bytes must be zero");
      Receipt receipt;
      receipt._state = read_state(read_unsigned_le<std::uint64_t>(bytes, 0), code);
      receipt._consumerTask = TaskId{read_unsigned_le<std::uint16_t>(bytes, 8)};
      receipt._sourceSet = SourceQueueSetId{read_unsigned_le<std::uint16_t>(bytes, 10)};
      receipt._presence = read_unsigned_le<std::uint32_t>(bytes, 12);
      receipt._consumerIncarnation = read_unsigned_le<std::uint64_t>(bytes, 16);
      receipt._issuance = read_unsigned_le<std::uint64_t>(bytes, 24);
      receipt._dequeuedAt = UtcTimestamp{std::chrono::nanoseconds{std::bit_cast<std::int64_t>(read_unsigned_le<std::uint64_t>(bytes, 32))}};
      receipt._acknowledgedAt = UtcTimestamp{std::chrono::nanoseconds{std::bit_cast<std::int64_t>(read_unsigned_le<std::uint64_t>(bytes, 40))}};
      receipt._terminalAt = UtcTimestamp{std::chrono::nanoseconds{std::bit_cast<std::int64_t>(read_unsigned_le<std::uint64_t>(bytes, 48))}};
      check_receipt(receipt, profile, code);
      return receipt;
    });
  }

  Result<RingSlotBytes> encode_ring_slot(const RingSlot& slot, std::uint64_t capacity,
                                         std::uint64_t index)
  {
    return checked_codec<RingSlotBytes>([&]
    {
      check_slot(slot, capacity, index, ErrorCode::InvalidArgument);
      RingSlotBytes bytes{};
      write_slot(bytes, slot);
      return bytes;
    });
  }

  Result<RingSlot> decode_ring_slot(std::span<const std::byte> bytes, std::uint64_t capacity,
                                    std::uint64_t index)
  {
    return checked_codec<RingSlot>([&]
    {
      codec_require(bytes.size() == ring_slot_bytes, ErrorCode::IoError, "ring.slot.length", "Ring slot must contain exactly 32 bytes");
      const auto slot = read_slot(bytes);
      check_slot(slot, capacity, index, ErrorCode::IoError);
      return slot;
    });
  }

  Result<std::uint64_t> ring_record_bytes(std::uint64_t capacity, std::uint64_t maxRecordBytes)
  {
    return checked_codec<std::uint64_t>([&]
    {
      return record_bytes(capacity, maxRecordBytes, ErrorCode::InvalidArgument);
    });
  }

  Result<std::vector<std::byte>> encode_ring_slots(std::span<const RingSlot> slots,
                                                   std::uint64_t lastAssigned,
                                                   std::uint64_t maxRecordBytes)
  {
    return checked_codec<std::vector<std::byte>>([&]
    {
      const std::uint64_t capacity = slots.size();
      const auto total = record_bytes(capacity, maxRecordBytes, ErrorCode::InvalidArgument);
      for (std::size_t index = 0; index < slots.size(); ++index)
      {
        check_window_slot(slots[index], capacity, index, lastAssigned, ErrorCode::InvalidArgument);
      }
      std::vector<std::byte> bytes(static_cast<std::size_t>(total - envelope_bytes));
      for (std::size_t index = 0; index < slots.size(); ++index)
      {
        write_slot(std::span{bytes}.subspan(index * ring_slot_bytes, ring_slot_bytes), slots[index]);
      }
      return bytes;
    });
  }

  Result<std::vector<RingSlot>> decode_ring_slots(std::span<const std::byte> bytes,
                                                  std::uint64_t capacity,
                                                  std::uint64_t lastAssigned,
                                                  std::uint64_t maxRecordBytes)
  {
    return checked_codec<std::vector<RingSlot>>([&]
    {
      constexpr auto code = ErrorCode::IoError;
      const auto total = record_bytes(capacity, maxRecordBytes, code);
      codec_require(bytes.size() == total - envelope_bytes, code, "ring.length", "Ring suffix length disagrees with capacity");
      const auto slotBytes = static_cast<std::size_t>(capacity * ring_slot_bytes);
      codec_require(all_zero(bytes.subspan(slotBytes)), code, "ring.padding", "Ring allocation padding must be zero");
      // Validate all untrusted words before allocating the returned owning copy.
      for (std::size_t index = 0; index < static_cast<std::size_t>(capacity); ++index)
      {
        const auto slot = read_slot(bytes.subspan(index * ring_slot_bytes, ring_slot_bytes));
        check_window_slot(slot, capacity, index, lastAssigned, code);
      }
      std::vector<RingSlot> slots(static_cast<std::size_t>(capacity));
      for (std::size_t index = 0; index < slots.size(); ++index)
      {
        slots[index] = read_slot(bytes.subspan(index * ring_slot_bytes, ring_slot_bytes));
      }
      return slots;
    });
  }
} // namespace angaria::detail::mapped
