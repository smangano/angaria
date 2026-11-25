#pragma once

#include "angaria/message_record.h"

#include <cstdint>
#include <string>

namespace angaria
{
  class Session;

  struct InspectionLimits
  {
    std::uint64_t _maxRecordBytes{1048576};
    std::uint64_t _maxCatalogBytes{16777216};
    std::uint64_t _maxTextBytes{4194304};
  };

  // Owns only retained Field/schema definitions, never a Session or Task claim.
  // Rendered output is bounded and contains no unescaped control/non-ASCII bytes.
  class MessageInspector
  {
  public:
    static Result<MessageInspector> from_session(const Session& session, InspectionLimits limits = {});
    static Result<MessageInspector> from_snapshot(SessionId schemaSession, const RegistrySnapshot& snapshot,
                                                   InspectionLimits limits = {});

    MessageInspector(const MessageInspector&) = default;
    MessageInspector& operator=(const MessageInspector& other);
    MessageInspector(MessageInspector&& other) noexcept;
    MessageInspector& operator=(MessageInspector&& other) noexcept;

    SessionId session() const;
    Result<std::string> inspect(std::span<const std::byte> record) const;
    Result<std::string> inspect(const MessageRecordView& record) const;

  private:
    MessageInspector(SessionId session, RegistrySnapshot catalog, InspectionLimits limits);
    SessionId _session{};
    RegistrySnapshot _catalog;
    InspectionLimits _limits;
  };
} // namespace angaria
