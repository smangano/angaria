#pragma once

#include "angaria/results.h"

#include <string>
#include <string_view>
#include <utility>
#include <variant>

namespace angaria::detail
{
  // Private control flow for the byte codecs. Each entry point still selects
  // its own error code/path and its policy for allocation/unexpected failures.
  struct CodecFailure
  {
    Error _error;
  };

  [[noreturn]] inline void codec_fail(ErrorCode code, std::string_view path, std::string_view message)
  {
    throw CodecFailure{Error{code, std::string{message}, std::string{path}}};
  }

  inline void codec_require(bool condition, ErrorCode code, std::string_view path, std::string_view message)
  {
    if (!condition)
    {
      codec_fail(code, path, message);
    }
  }

  // Failure must hold Error as its first aggregate member. Keep the caller's
  // distinct exception type so its existing catch boundary stays authoritative.
  template <typename Failure, typename T>
  T value_or_throw(Result<T> result)
  {
    if (auto* error = std::get_if<Error>(&result))
    {
      throw Failure{std::move(*error)};
    }
    return std::get<T>(std::move(result));
  }

  inline const char* error_name(ErrorCode code)
  {
    switch (code)
    {
    case ErrorCode::None: return "None";
    case ErrorCode::InvalidArgument: return "InvalidArgument";
    case ErrorCode::InvalidState: return "InvalidState";
    case ErrorCode::InvalidConfiguration: return "InvalidConfiguration";
    case ErrorCode::ConfigurationMismatch: return "ConfigurationMismatch";
    case ErrorCode::NotFound: return "NotFound";
    case ErrorCode::Conflict: return "Conflict";
    case ErrorCode::CapacityExceeded: return "CapacityExceeded";
    case ErrorCode::OwnershipBusy: return "OwnershipBusy";
    case ErrorCode::WrongOwner: return "WrongOwner";
    case ErrorCode::ReadOnlyRegistry: return "ReadOnlyRegistry";
    case ErrorCode::NotActive: return "NotActive";
    case ErrorCode::UnsupportedPolicy: return "UnsupportedPolicy";
    case ErrorCode::SchemaConflict: return "SchemaConflict";
    case ErrorCode::LogFull: return "LogFull";
    case ErrorCode::StaleDelivery: return "StaleDelivery";
    case ErrorCode::OutputBufferTooSmall: return "OutputBufferTooSmall";
    case ErrorCode::HistoryUnavailable: return "HistoryUnavailable";
    case ErrorCode::IoError: return "IoError";
    case ErrorCode::OutcomeUnknown: return "OutcomeUnknown";
    }
    return "UnknownError";
  }
} // namespace angaria::detail
