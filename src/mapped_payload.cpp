#include "mapped_payload.h"

#include "runtime_state.h"

#include <utility>

#include <unistd.h>

namespace angaria::detail
{
  MappedPayload::MappedPayload(mapped::MappedRegion region, std::span<const std::byte> bytes,
                                std::uint64_t ownerPid) noexcept
      : _region(std::move(region)), _bytes(bytes), _ownerPid(ownerPid)
  {
  }

  MappedPayload::MappedPayload(MappedPayload&& other) noexcept
      : _region(std::move(other._region)),
        _bytes(std::exchange(other._bytes, {})),
        _ownerPid(std::exchange(other._ownerPid, 0))
  {
  }

  MappedPayload& MappedPayload::operator=(MappedPayload&& other) noexcept
  {
    if (this != &other)
    {
      _region = std::move(other._region);
      _bytes = std::exchange(other._bytes, {});
      _ownerPid = std::exchange(other._ownerPid, 0);
    }
    return *this;
  }

  Result<std::span<const std::byte>> MappedPayload::bytes() const
  {
    if (_ownerPid == 0)
    {
      return Error{ErrorCode::InvalidState, "Payload has no owned mapping", "publication.payload"};
    }
    if (_ownerPid != static_cast<std::uint64_t>(::getpid()))
    {
      return Error{ErrorCode::WrongOwner, "Payload belongs to another process", "publication.payload"};
    }
    return _bytes;
  }

  mapped::PublicationMetadata runtime_publication_metadata(RuntimeAccess& access,
                                                            std::uint64_t reference)
  {
    runtime_value(access._guard.check_owner());
    const auto& envelope = access.record_envelope(reference, 9);
    const auto limit = access._session->_controller->configuration()._maxLogBytes;
    const auto fixed = access.read_bytes(reference + mapped::record_envelope_bytes,
                                         mapped::publication_fixed_bytes);
    const auto prefix = runtime_value(mapped::decode_publication_prefix(fixed, envelope._immutableBytes,
                                                                         reference, limit));
    if (prefix._layout._totalBytes != envelope._totalBytes)
    {
      runtime_fail(ErrorCode::IoError, "publication.layout", "Publication allocation disagrees with its envelope");
    }
    if (prefix._header._publishingSet != PublishingQueueSetId::Invalid)
    {
      runtime_fail(ErrorCode::UnsupportedPolicy, "publication.publishing", "Native Publishing acceptance requires a later capability");
    }
    // The checked prefix bounds both reads before allocation. Application
    // bytes between destinations and padding are never needed for metadata
    // inspection, regardless of the size of the retained payload plane.
    const auto rowsBytes = std::uint64_t{prefix._destinationCount} * mapped::destination_row_bytes;
    const auto destinations = access.read_bytes(prefix._layout._destinationsOffset, rowsBytes);
    const auto payloadEnd = prefix._layout._destinationsOffset + rowsBytes + prefix._payloadBytes;
    const auto padding = access.read_bytes(payloadEnd, prefix._layout._receiptsOffset - payloadEnd);
    return runtime_value(mapped::decode_publication_metadata(fixed, destinations, padding,
                                                              envelope._immutableBytes, reference, limit));
  }

  Result<MappedPayload> map_publication_payload(RuntimeAccess& access, std::uint64_t reference)
  {
    try
    {
      // Check ownership before the empty-payload path as well. An empty slice
      // still belongs to the process which validated the committed record.
      runtime_value(access._guard.check_owner());
      if ((access._session->_controller->configuration()._requiredFeatures & mapped::native_delivery_feature) == 0)
      {
        runtime_fail(ErrorCode::UnsupportedPolicy, "publication.payload", "Session does not enable native delivery storage");
      }
      const auto metadata = runtime_publication_metadata(access, reference);
      mapped::MappedRegion region;
      std::span<const std::byte> bytes;
      if (metadata._payloadBytes != 0)
      {
        // Publication geometry puts this entire slice before immutable padding
        // and receipts, within the committed prefix captured under this gate.
        region = runtime_value(access._file->map_read_only(metadata._layout._payloadOffset,
                                                           metadata._payloadBytes));
        bytes = runtime_value(region.bytes());
      }
      return MappedPayload(std::move(region), bytes, static_cast<std::uint64_t>(::getpid()));
    }
    catch (...)
    {
      return runtime_caught_error();
    }
  }
} // namespace angaria::detail
