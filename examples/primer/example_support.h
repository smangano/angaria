#pragma once

#include "angaria/runtime.h"

#include <filesystem>
#include <stdexcept>
#include <string>
#include <utility>
#include <variant>

namespace primer
{
  // These names make failures readable at the terminal. The Error also gives
  // the setting or resource path and a more detailed explanation.
  inline const char* error_name(angaria::ErrorCode code)
  {
    switch (code)
    {
    case angaria::ErrorCode::None:
      return "None";
    case angaria::ErrorCode::InvalidArgument:
      return "InvalidArgument";
    case angaria::ErrorCode::InvalidState:
      return "InvalidState";
    case angaria::ErrorCode::InvalidConfiguration:
      return "InvalidConfiguration";
    case angaria::ErrorCode::ConfigurationMismatch:
      return "ConfigurationMismatch";
    case angaria::ErrorCode::NotFound:
      return "NotFound";
    case angaria::ErrorCode::Conflict:
      return "Conflict";
    case angaria::ErrorCode::CapacityExceeded:
      return "CapacityExceeded";
    case angaria::ErrorCode::OwnershipBusy:
      return "OwnershipBusy";
    case angaria::ErrorCode::WrongOwner:
      return "WrongOwner";
    case angaria::ErrorCode::ReadOnlyRegistry:
      return "ReadOnlyRegistry";
    case angaria::ErrorCode::NotActive:
      return "NotActive";
    case angaria::ErrorCode::UnsupportedPolicy:
      return "UnsupportedPolicy";
    case angaria::ErrorCode::SchemaConflict:
      return "SchemaConflict";
    case angaria::ErrorCode::LogFull:
      return "LogFull";
    case angaria::ErrorCode::StaleDelivery:
      return "StaleDelivery";
    case angaria::ErrorCode::OutputBufferTooSmall:
      return "OutputBufferTooSmall";
    case angaria::ErrorCode::HistoryUnavailable:
      return "HistoryUnavailable";
    case angaria::ErrorCode::IoError:
      return "IoError";
    case angaria::ErrorCode::OutcomeUnknown:
      return "OutcomeUnknown";
    }
    return "UnknownError";
  }

  // Result<T> contains either T or Error. Keep that choice visible here, then
  // let each small program's catch block print unexpected failures and exit.
  template <typename T>
  T value(angaria::Result<T> result)
  {
    if (const auto* error = std::get_if<angaria::Error>(&result))
    {
      throw std::runtime_error(std::string{error_name(error->_code)} + " at " +
                               error->_path + ": " + error->_message);
    }
    return std::get<T>(std::move(result));
  }

  // Later examples reuse the small profile spelled out in 01_session.cpp.
  // The application chooses these bounds before creating its Session.
  inline angaria::SessionOptions session_options(const std::filesystem::path& directory,
                                                 const std::string& name)
  {
    angaria::SessionOptions options;
    options._name = name;
    options._directory = directory;
    options._maxLogBytes = 1024 * 1024;
    options._limits = angaria::CapacityLimits::empty().tasks(8).queues(8)
        .source_sets(4).publishing_sets(4).memberships_per_set(4)
        .fields(16).message_schemas(8);
    options._defaults._ringCapacity = 8;
    return options;
  }
} // namespace primer
