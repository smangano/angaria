#pragma once

#include "angaria/identities.h"

#include <cstdint>
#include <span>
#include <string>
#include <variant>

namespace angaria
{
  enum class ErrorCode
  {
    None,
    InvalidArgument,
    InvalidState,
    InvalidConfiguration,
    ConfigurationMismatch,
    NotFound,
    Conflict,
    CapacityExceeded,
    OwnershipBusy,
    WrongOwner,
    ReadOnlyRegistry,
    NotActive,
    UnsupportedPolicy,
    SchemaConflict,
    LogFull,
    StaleDelivery,
    OutputBufferTooSmall,
    HistoryUnavailable,
    IoError,
    OutcomeUnknown
  };

  struct Error
  {
    ErrorCode _code{ErrorCode::InvalidState};
    std::string _message;
    std::string _path;
  };

  template <typename T>
  using Result = std::variant<T, Error>;

  using OperationResult = Result<std::monostate>;

  enum class DestinationStatus
  {
    Accepted,
    Rejected,
    Unknown
  };

  struct DestinationOutcome
  {
    QueueId _queue{};
    DestinationStatus _status{DestinationStatus::Rejected};
    ErrorCode _reason{ErrorCode::InvalidState};
  };

  enum class PublicationStatus
  {
    Accepted,
    Partial,
    Rejected,
    OutcomeUnknown
  };

  // Destination views refer to the output buffer supplied to the operation.
  struct PublicationReport
  {
    PublicationId _publication{};
    PublicationStatus _status{PublicationStatus::OutcomeUnknown};
    std::uint64_t _topologyRevision{};
    std::span<const DestinationOutcome> _destinations;
    std::uint32_t _requiredCapacity{};
    ErrorCode _error{ErrorCode::InvalidState};
  };
} // namespace angaria
