#pragma once

#include "native_file.h"
#include "publication_metadata.h"

#include <cstddef>
#include <cstdint>
#include <span>

namespace angaria::detail
{
  class RuntimeAccess;

  // A private owner of the exact immutable application payload slice. Its
  // read-only mapping and inode lease outlive the operation gate and Session.
  // Returned spans borrow this owner; moving it transfers that lifetime.
  class MappedPayload
  {
  public:
    MappedPayload() noexcept = default;
    ~MappedPayload() = default;
    MappedPayload(MappedPayload&& other) noexcept;
    MappedPayload& operator=(MappedPayload&& other) noexcept;
    MappedPayload(const MappedPayload&) = delete;
    MappedPayload& operator=(const MappedPayload&) = delete;

    Result<std::span<const std::byte>> bytes() const;

  private:
    friend Result<MappedPayload> map_publication_payload(RuntimeAccess& access,
                                                          std::uint64_t reference);
    MappedPayload(mapped::MappedRegion region, std::span<const std::byte> bytes,
                    std::uint64_t ownerPid) noexcept;

    mapped::MappedRegion _region;
    std::span<const std::byte> _bytes;
    std::uint64_t _ownerPid{};
  };

  // Read only bounded metadata, never the application bytes. The gate-bound
  // access establishes an exact committed Publication boundary. These helpers
  // check storage shape; the delivery validator checks schema/topology/receipt
  // semantics separately before an endpoint may offer a delivery.
  mapped::PublicationMetadata runtime_publication_metadata(RuntimeAccess& access,
                                                            std::uint64_t reference);
  Result<MappedPayload> map_publication_payload(RuntimeAccess& access,
                                                 std::uint64_t reference);
} // namespace angaria::detail
