#pragma once

#include "angaria/results.h"

#include <stdexcept>
#include <utility>
#include <variant>

namespace angaria::test_support
{
  // Test plumbing only: preserve detailed failures without borrowing production
  // exception policies or its independent wire encoders for golden fixtures.
  template <typename T>
  T require_value(Result<T> result)
  {
    if (const auto* error = std::get_if<Error>(&result))
    {
      throw std::runtime_error(error->_path + ": " + error->_message);
    }
    return std::get<T>(std::move(result));
  }

  template <typename T>
  bool has_error(const Result<T>& result, ErrorCode code)
  {
    const auto* error = std::get_if<Error>(&result);
    return error != nullptr && error->_code == code;
  }
} // namespace angaria::test_support
