// Private helper contracts, independent of native Session delivery. Fixed byte
// expectations and boundary values protect the shared implementation used by
// otherwise separate codecs; format-level golden fixtures remain independent.

#include "angaria/detail/byte_operations.h"
#include "angaria/detail/checked_arithmetic.h"
#include "angaria/detail/error_support.h"
#include "angaria/detail/field_representation.h"
#include "angaria/detail/identity_helpers.h"
#include "angaria/detail/text_escaping.h"
#include "angaria/detail/wire_codes.h"

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <span>
#include <sstream>
#include <string>
#include <utility>
#include <variant>

namespace
{
  namespace helpers = angaria::detail;
  using namespace angaria;

  TEST(SharedHelpersTest, UnalignedLittleEndianFieldsPreserveSurroundingBytes)
  {
    std::array<std::byte, 13> bytes;
    bytes.fill(std::byte{0xa5});
    helpers::write_unsigned_le(bytes, 1, std::uint64_t{0x8877665544332211});
    const std::array expected{
        std::byte{0xa5}, std::byte{0x11}, std::byte{0x22}, std::byte{0x33},
        std::byte{0x44}, std::byte{0x55}, std::byte{0x66}, std::byte{0x77},
        std::byte{0x88}, std::byte{0xa5}, std::byte{0xa5}, std::byte{0xa5}, std::byte{0xa5}};
    EXPECT_EQ(bytes, expected);
    EXPECT_EQ(helpers::read_unsigned_le<std::uint16_t>(bytes, 3), 0x4433u);
    EXPECT_EQ(helpers::read_unsigned_le<std::uint32_t>(bytes, 5), 0x88776655u);
    EXPECT_EQ(helpers::read_unsigned_le<std::uint64_t>(bytes, 1), 0x8877665544332211ull);
    // Variable-width logical bit fields share the same byte order and retain
    // only the requested low bytes; this 24-bit example has no native C++ type.
    helpers::write_unsigned_le(bytes, 9, std::uint64_t{0xffabcdef}, 3);
    EXPECT_EQ(bytes[9], std::byte{0xef});
    EXPECT_EQ(bytes[10], std::byte{0xcd});
    EXPECT_EQ(bytes[11], std::byte{0xab});
    EXPECT_EQ(bytes[12], std::byte{0xa5});
    EXPECT_EQ(helpers::read_unsigned_le(bytes, 9, 3), 0xabcdefu);
    std::array<std::uint8_t, 3> prototype{0xa5, 0, 0};
    helpers::write_unsigned_le(std::span{prototype}, 1, std::uint16_t{0x1234});
    EXPECT_EQ(prototype, (std::array<std::uint8_t, 3>{0xa5, 0x34, 0x12}));
    EXPECT_EQ(helpers::read_unsigned_le<std::uint16_t>(std::span<const std::uint8_t>{prototype}, 1), 0x1234u);
  }

  TEST(SharedHelpersTest, SignedBitsCoverEveryInt8ValueAndWiderBoundaries)
  {
    for (unsigned bits = 0; bits < 256; ++bits)
    {
      const auto expected = bits < 128 ? static_cast<int>(bits) : static_cast<int>(bits) - 256;
      EXPECT_EQ(helpers::signed_from_bits<std::int8_t>(static_cast<std::uint8_t>(bits)), expected);
    }
    EXPECT_EQ(helpers::signed_from_bits<std::int16_t>(0x8000u), std::numeric_limits<std::int16_t>::min());
    EXPECT_EQ(helpers::signed_from_bits<std::int32_t>(0xffffffffu), -1);
    EXPECT_EQ(helpers::signed_from_bits<std::int64_t>(0x8000000000000000ull), std::numeric_limits<std::int64_t>::min());
    EXPECT_EQ(helpers::signed_from_bits<std::int64_t>(0x7fffffffffffffffull), std::numeric_limits<std::int64_t>::max());
  }

  TEST(SharedHelpersTest, EmptyAndNonemptyZeroRangesHaveTheSameContract)
  {
    EXPECT_TRUE(helpers::all_zero(std::span<const std::byte>{}));
    EXPECT_TRUE(helpers::all_zero(std::span<const std::uint64_t>{}));
    std::array<std::byte, 3> bytes{};
    std::array<std::uint64_t, 3> words{};
    EXPECT_TRUE(helpers::all_zero(bytes));
    EXPECT_TRUE(helpers::all_zero(words));
    for (std::size_t index = 0; index < bytes.size(); ++index)
    {
      bytes[index] = std::byte{0x80};
      words[index] = std::uint64_t{1} << 63;
      EXPECT_FALSE(helpers::all_zero(bytes));
      EXPECT_FALSE(helpers::all_zero(words));
      bytes[index] = std::byte{};
      words[index] = 0;
    }
  }

  TEST(SharedHelpersTest, ArithmeticAndHostBoundsStayIndependentOfFormatPolicy)
  {
    constexpr auto maximum = std::numeric_limits<std::uint64_t>::max();
    static_assert(helpers::checked_add(maximum, 0) == maximum);
    static_assert(!helpers::checked_add(maximum, 1));
    static_assert(helpers::checked_multiply(maximum, 0) == 0);
    static_assert(!helpers::checked_multiply(maximum, 2));
    static_assert(helpers::checked_align(4, 3) == 6);
    static_assert(!helpers::checked_align(maximum, 64));
    static_assert(!helpers::checked_align(0, 0));
    static_assert(helpers::contains_range(maximum, maximum, 0));
    static_assert(!helpers::contains_range(maximum, maximum, 1));
    EXPECT_TRUE(helpers::fits_host_size(std::numeric_limits<std::ptrdiff_t>::max()));
    EXPECT_FALSE(helpers::fits_host_size(maximum));
    EXPECT_TRUE(helpers::fits_size_t(std::numeric_limits<std::size_t>::max()));
  }

  TEST(SharedHelpersTest, ErrorUnwrappingPreservesTheCallerBoundaryAndMoveOnlyValues)
  {
    struct ExampleFailure
    {
      Error _error;
    };
    auto value = helpers::value_or_throw<ExampleFailure>(Result<std::unique_ptr<int>>{std::make_unique<int>(7)});
    ASSERT_NE(value, nullptr);
    EXPECT_EQ(*value, 7);
    try
    {
      helpers::value_or_throw<ExampleFailure>(Result<int>{Error{ErrorCode::Conflict, "retained message", "example.path"}});
      FAIL() << "Expected the operation's own exception type";
    }
    catch (const ExampleFailure& failure)
    {
      EXPECT_EQ(failure._error._code, ErrorCode::Conflict);
      EXPECT_EQ(failure._error._message, "retained message");
      EXPECT_EQ(failure._error._path, "example.path");
    }
    EXPECT_STREQ(helpers::error_name(ErrorCode::OutcomeUnknown), "OutcomeUnknown");
    EXPECT_STREQ(helpers::error_name(static_cast<ErrorCode>(999)), "UnknownError");
  }

  TEST(SharedHelpersTest, ExplicitStoredCodesAndFieldShapesKeepTheirMeanings)
  {
    namespace wire = helpers::wire;
    EXPECT_EQ(wire::encode(EntityState::Draft, wire::entity_states), 1u);
    EXPECT_EQ(wire::decode(2, wire::entity_states), EntityState::Active);
    EXPECT_EQ(wire::encode(PayloadFormat::Binary, wire::payload_formats), 1u);
    EXPECT_EQ(wire::decode(5, wire::payload_formats), PayloadFormat::Text);
    EXPECT_EQ(wire::encode(ErrorCode::None, wire::error_codes, 0), 0u);
    EXPECT_EQ(wire::decode(19, wire::error_codes, 0), ErrorCode::OutcomeUnknown);
    EXPECT_FALSE(wire::decode(0, wire::payload_formats));
    EXPECT_FALSE(wire::decode(65535, wire::entity_states));
    EXPECT_FALSE(wire::encode(static_cast<PayloadFormat>(999), wire::payload_formats));
    EXPECT_FALSE(helpers::integer_shape(EFieldBase::BASE_CHAR));
    const auto integer = helpers::integer_shape(EFieldBase::BASE_INT8);
    ASSERT_TRUE(integer);
    EXPECT_TRUE(integer->_signed);
    EXPECT_EQ(integer->_bytes, 1u);
    EXPECT_EQ(helpers::scalar_width(EFieldBase::BASE_FLOAT64), 8u);
    EXPECT_FALSE(helpers::scalar_width(EFieldBase::BASE_CHARS));
    EXPECT_EQ(helpers::schema_key(SchemaId{MessageType{65535}, 0xffffffffu}), 0xffffffffffffull);
    EXPECT_TRUE(helpers::valid_field_id(0xeeffffffu));
    EXPECT_FALSE(helpers::valid_field_id(0xedffffffu));
  }

  TEST(SharedHelpersTest, EscapingPreservesQuotePoliciesAndStreamingBoundaries)
  {
    const auto render = [](unsigned char byte, bool quoted = false)
    {
      const auto encoded = helpers::escape_ascii_byte(byte, quoted);
      return std::string(encoded._bytes.data(), encoded._size);
    };
    EXPECT_EQ(render('"'), "\"");
    EXPECT_EQ(render('"', true), "\\\"");
    EXPECT_EQ(render('\\'), "\\\\");
    EXPECT_EQ(render(0), "\\x00");
    EXPECT_EQ(render(0x1f), "\\x1f");
    EXPECT_EQ(render(0x7f), "\\x7f");
    EXPECT_EQ(render(0xff), "\\xff");
    EXPECT_EQ(render('~'), "~");
    const auto prefix = std::string(1023, 'x');
    const auto suffix = std::string(4097, 'y');
    std::ostringstream output;
    helpers::write_diagnostic_text(output, prefix + "\n\\" + suffix);
    EXPECT_EQ(output.str(), prefix + "\\x0a\\\\" + suffix);
  }
} // namespace
