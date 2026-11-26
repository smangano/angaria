#pragma once

#include "journal_validation.h"
#include "native_file.h"
#include "publication_metadata.h"

#include <array>
#include <cstddef>
#include <span>
#include <variant>

namespace angaria::detail::mapped
{
  // Compact redo discovers retained direct deliveries from bounded immutable
  // metadata. Read at most one row and seven alignment bytes; payload bytes
  // are never read or copied, even when a Publication contains a large body.
  inline Result<PublicationMetadata> read_journal_publication(
      NativeFile& file, const JournalRecordRange& record, ErrorCode code)
  {
    std::array<std::byte, publication_fixed_bytes> fixed{};
    if (const auto result = file.read_exact(record._offset + record_envelope_bytes, fixed);
        std::holds_alternative<Error>(result))
    {
      return std::get<Error>(result);
    }
    const auto decoded = decode_publication_prefix(fixed, record._envelope._immutableBytes,
                                                    record._offset, max_log_bytes);
    if (const auto* error = std::get_if<Error>(&decoded))
    {
      return *error;
    }
    const auto& prefix = std::get<PublicationPrefix>(decoded);
    if (record._envelope._readiness != 2 || prefix._destinationCount != 1 ||
        prefix._header._publishingSet != PublishingQueueSetId::Invalid ||
        prefix._layout._totalBytes != record._envelope._totalBytes)
    {
      return Error{code, "Compact redo requires committed canonical direct Publications", "journal.metadata"};
    }
    DestinationRowBytes row{};
    if (const auto result = file.read_exact(prefix._layout._destinationsOffset, row);
        std::holds_alternative<Error>(result))
    {
      return std::get<Error>(result);
    }
    const auto payloadEnd = prefix._layout._payloadOffset + prefix._payloadBytes;
    const auto paddingBytes = prefix._layout._receiptsOffset - payloadEnd;
    std::array<std::byte, 7> padding{};
    if (paddingBytes > padding.size())
    {
      return Error{code, "Publication padding exceeds its word boundary", "journal.metadata"};
    }
    const auto suffix = std::span{padding}.first(static_cast<std::size_t>(paddingBytes));
    if (const auto result = file.read_exact(payloadEnd, suffix); std::holds_alternative<Error>(result))
    {
      return std::get<Error>(result);
    }
    return decode_publication_metadata(fixed, row, suffix,
        record._envelope._immutableBytes, record._offset, max_log_bytes);
  }
} // namespace angaria::detail::mapped
