#include "angaria/field.h"

#include <gtest/gtest.h>
#include <sstream>

namespace angaria
{
  enum class EFieldType : uint16_t
  {
    TestField1,
    TestField2,
    TestField3
  };

  TEST(Field, Int32)
  {
    using TestField = Int32Field<EFieldType::TestField1>;
    TestField field(100);

    EXPECT_EQ(field.get(), 100);
    field.set(-42);
    EXPECT_EQ(field.to_string(), "-42");
    EXPECT_EQ(field, TestField(-42));
  }

  TEST(Field, Int32StreamRoundTrip)
  {
    using TestField = Int32Field<EFieldType::TestField1>;
    const TestField original(-42);
    TestField decoded(0);
    std::stringstream stream;

    original.write(stream);
    decoded.read(stream);

    ASSERT_TRUE(stream.good());
    EXPECT_EQ(decoded, original);
  }

  TEST(Field, Int32TruncatedRead)
  {
    const Int32Field<EFieldType::TestField2> original(-42);
    Int32Field<EFieldType::TestField1> decoded(100);
    std::stringstream stream;

    original.write(stream);
    auto bytes = stream.str();
    ASSERT_FALSE(bytes.empty());
    bytes.pop_back();
    std::istringstream truncated(bytes);

    decoded.read(truncated);

    EXPECT_TRUE(truncated.fail());
    EXPECT_EQ(decoded._fieldType, EFieldType::TestField1);
    EXPECT_EQ(decoded.get(), 100);
  }
} // namespace angaria
