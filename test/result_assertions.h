#pragma once

#include "result_helpers.h"

#include <gtest/gtest.h>

#include <variant>

namespace angaria::test_support
{
  // Kept separate from result_helpers.h so standalone process workers can
  // unwrap Results without taking a GoogleTest header/link dependency.
  template <typename T>
  void expect_error(const Result<T>& result, ErrorCode code)
  {
    ASSERT_TRUE(std::holds_alternative<Error>(result));
    const auto& error = std::get<Error>(result);
    EXPECT_EQ(error._code, code) << error._path << ": " << error._message;
    EXPECT_FALSE(error._path.empty());
    EXPECT_FALSE(error._message.empty());
  }
} // namespace angaria::test_support
