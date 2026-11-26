#pragma once

#include "angaria/runtime.h"
#include "angaria/detail/error_support.h"

#include <filesystem>
#include <stdexcept>
#include <string>
#include <utility>
#include <variant>

namespace primer
{
  // Shared diagnostic names keep example and inspection-tool errors consistent.
  using angaria::detail::error_name;

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
