#include "result_assertions.h"
#include "mapped_superblock.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <utility>
#include <variant>
#include <vector>

// The superblock is the first 4096 bytes of session.ang; it describes format,
// compatibility, and fixed-section geometry (docs/mapped-format.md). These
// portable codec tests neither recover the journal nor create a usable Session.
namespace
{
  using namespace angaria;
  namespace mapped = angaria::detail::mapped;

  constexpr std::uint32_t fixture_profile = 0x00010002;
  constexpr std::uint64_t fixture_arena = 5760;
  constexpr auto max_unsigned = std::numeric_limits<std::uint64_t>::max();

  void put_integer(mapped::SuperblockBytes& bytes, std::size_t offset, std::uint64_t value,
                   std::size_t width)
  {
    for (std::size_t i = 0; i < width; ++i)
    {
      bytes[offset + i] = static_cast<std::byte>((value >> (8 * i)) & 0xff);
    }
  }

  std::uint64_t get_integer(const mapped::SuperblockBytes& bytes, std::size_t offset,
                            std::size_t width)
  {
    std::uint64_t value{};
    for (std::size_t i = 0; i < width; ++i)
    {
      value |= std::uint64_t{std::to_integer<std::uint8_t>(bytes[offset + i])} << (8 * i);
    }
    return value;
  }

  mapped::SuperblockConfiguration fixture_configuration()
  {
    mapped::SuperblockConfiguration configuration;
    // Keep the independently authored 2.0 fixture unchanged. Framing defaults
    // remain 2.1; public factories explicitly select the 2.2 core profile.
    configuration._formatMinor = 0;
    configuration._requiredFeatures = mapped::journal_feature;
    configuration._session._high = 0x0807060504030201;
    configuration._session._low = 0x8877665544332211;
    configuration._maxLogBytes = 0x0000000203040506;
    configuration._liveAbiProfile = fixture_profile;
    configuration._optionalFeatures = 0x1020304050607080;
    configuration._limits = CapacityLimits::empty()
                                .tasks(1)
                                .queues(2)
                                .source_sets(1)
                                .memberships_per_set(3)
                                .fields(2)
                                .message_schemas(1)
                                .string_pool_bytes(3);
    return configuration;
  }

  mapped::SuperblockBytes golden_superblock()
  {
    // This fixture is authored directly from the format table. It never calls
    // the production encoder or section planner to obtain expected bytes.
    mapped::SuperblockBytes bytes{};
    const std::array<std::uint8_t, 8> magic{0x41, 0x4e, 0x47, 0x41, 0x52, 0x49, 0x41, 0};
    for (std::size_t i = 0; i < magic.size(); ++i)
    {
      bytes[i] = static_cast<std::byte>(magic[i]);
    }
    put_integer(bytes, 8, 2, 2);
    put_integer(bytes, 12, 4096, 4);
    put_integer(bytes, 16, 0x0807060504030201, 8);
    put_integer(bytes, 24, 0x8877665544332211, 8);
    put_integer(bytes, 32, 0x0000000203040506, 8);
    put_integer(bytes, 40, fixture_arena, 8);
    put_integer(bytes, 48, 1, 2);
    put_integer(bytes, 50, 1, 2);
    put_integer(bytes, 52, fixture_profile, 4);
    put_integer(bytes, 56, 1, 8);
    put_integer(bytes, 64, 0x1020304050607080, 8);
    put_integer(bytes, 88, fixture_arena, 8);
    put_integer(bytes, 104, 13, 4);
    put_integer(bytes, 108, 64, 1);
    put_integer(bytes, 109, 1, 1);
    put_integer(bytes, 110, 8, 1);
    const std::array<std::uint32_t, 8> limits{1, 2, 1, 0, 3, 2, 1, 0};
    for (std::size_t i = 0; i < limits.size(); ++i)
    {
      put_integer(bytes, 128 + 4 * i, limits[i], 4);
    }
    put_integer(bytes, 160, 3, 8);
    put_integer(bytes, 176, fixture_arena, 8);

    const std::array<std::uint64_t, 13> offsets{
        4096, 4160, 4288, 0, 4352, 4480, 4544, 4800, 5312, 0, 5568, 0, 5696,
    };
    const std::array<std::uint64_t, 13> counts{1, 2, 1, 0, 2, 1, 1, 2, 1, 0, 3, 0, 3};
    const std::array<std::uint32_t, 13> strides{
        64, 64, 64, 64, 64, 64, 256, 256, 256, 128, 32, 32, 1,
    };
    const std::array<std::uint64_t, 13> lengths{
        64, 128, 64, 0, 128, 64, 256, 512, 256, 0, 96, 0, 3,
    };
    for (std::size_t i = 0; i < offsets.size(); ++i)
    {
      const auto entry = 256 + i * 32;
      put_integer(bytes, entry, i + 1, 2);
      put_integer(bytes, entry + 4, strides[i], 4);
      put_integer(bytes, entry + 8, offsets[i], 8);
      put_integer(bytes, entry + 16, counts[i], 8);
      put_integer(bytes, entry + 24, lengths[i], 8);
    }
    return bytes;
  }

  using angaria::test_support::expect_error;

  mapped::LiveCompatibility fixture_support()
  {
    mapped::LiveCompatibility supported;
    supported._liveAbiProfile = fixture_profile;
    return supported;
  }

  // Compare against bytes authored from the format table, rather than decoding
  // the encoder's own output. A matching encoder/decoder bug cannot hide here.
  TEST(MappedSuperblock, EncoderMatchesIndependentGoldenBytes)
  {
    const auto encoded = mapped::encode_initial_superblock(fixture_configuration());
    ASSERT_TRUE(std::holds_alternative<mapped::SuperblockBytes>(encoded));
    EXPECT_EQ(std::get<mapped::SuperblockBytes>(encoded), golden_superblock());
  }

  // Decode known little-endian bytes into owning configuration values. The
  // asymmetric numbers expose byte-order and field-width mistakes on any host.
  TEST(MappedSuperblock, DecoderReadsIndependentLittleEndianFixture)
  {
    const auto bytes = golden_superblock();
    const auto decoded = mapped::decode_superblock(bytes);
    ASSERT_TRUE(std::holds_alternative<mapped::SuperblockConfiguration>(decoded));
    const auto& configuration = std::get<mapped::SuperblockConfiguration>(decoded);
    EXPECT_EQ(configuration._session._high, 0x0807060504030201u);
    EXPECT_EQ(configuration._session._low, 0x8877665544332211u);
    EXPECT_EQ(configuration._maxLogBytes, 0x0000000203040506u);
    EXPECT_EQ(configuration._storage, StorageMapping::TwoPlane);
    EXPECT_EQ(configuration._synchronizationVersion, 1u);
    EXPECT_EQ(configuration._liveAbiProfile, fixture_profile);
    EXPECT_EQ(configuration._requiredFeatures, 1u);
    EXPECT_EQ(configuration._optionalFeatures, 0x1020304050607080u);
    EXPECT_EQ(configuration._limits._tasks, 1u);
    EXPECT_EQ(configuration._limits._queues, 2u);
    EXPECT_EQ(configuration._limits._sourceSets, 1u);
    EXPECT_EQ(configuration._limits._publishingSets, 0u);
    EXPECT_EQ(configuration._limits._membershipsPerSet, 3u);
    EXPECT_EQ(configuration._limits._fields, 2u);
    EXPECT_EQ(configuration._limits._messageSchemas, 1u);
    EXPECT_EQ(configuration._limits._symbols, 0u);
    EXPECT_EQ(configuration._limits._stringPoolBytes, 3u);
    EXPECT_EQ(configuration._segmentBytes, 0u);
  }

  // Encoding reserves geometry and starts with empty roots/journal; it does not
  // publish Session readiness. An empty capacity profile is valid for this
  // low-level codec even though public creation must reserve its creator Task.
  TEST(MappedSuperblock, DefaultAndEmptyProfilesInitializeOnlyTheProvisionalPrefix)
  {
    auto configuration = fixture_configuration();
    configuration._limits = CapacityLimits{};
    auto encoded = mapped::encode_initial_superblock(configuration);
    ASSERT_TRUE(std::holds_alternative<mapped::SuperblockBytes>(encoded));
    auto bytes = std::get<mapped::SuperblockBytes>(encoded);
    EXPECT_EQ(get_integer(bytes, 40, 8), 1197056u);
    EXPECT_EQ(get_integer(bytes, 88, 8), 1197056u);
    EXPECT_EQ(get_integer(bytes, 176, 8), 1197056u);
    for (const auto offset : {72u, 80u, 96u, 112u, 120u})
    {
      EXPECT_EQ(get_integer(bytes, offset, 8), 0u);
    }
    EXPECT_TRUE(std::all_of(bytes.begin() + 672, bytes.begin() + 2720,
                            [](std::byte value)
                            {
                              return value == std::byte{};
                            }));

    configuration._limits = CapacityLimits::empty();
    configuration._maxLogBytes = 4096;
    encoded = mapped::encode_initial_superblock(configuration);
    ASSERT_TRUE(std::holds_alternative<mapped::SuperblockBytes>(encoded));
    bytes = std::get<mapped::SuperblockBytes>(encoded);
    EXPECT_EQ(get_integer(bytes, 40, 8), 4096u);
    EXPECT_EQ(get_integer(bytes, 88, 8), 4096u);
    EXPECT_EQ(get_integer(bytes, 176, 8), 4096u);
    for (std::size_t i = 0; i < 13; ++i)
    {
      const auto entry = 256 + i * 32;
      EXPECT_EQ(get_integer(bytes, entry, 2), i + 1);
      EXPECT_NE(get_integer(bytes, entry + 4, 4), 0u);
      EXPECT_EQ(get_integer(bytes, entry + 8, 8), 0u);
      EXPECT_EQ(get_integer(bytes, entry + 16, 8), 0u);
      EXPECT_EQ(get_integer(bytes, entry + 24, 8), 0u);
    }
    EXPECT_TRUE(std::holds_alternative<mapped::SuperblockConfiguration>(mapped::decode_superblock(bytes)));
  }

  // A Session ID is an opaque 128-bit value. Only the all-zero value is invalid;
  // applications must not impose separate nonzero requirements on its halves.
  TEST(MappedSuperblock, SessionIdentityNeedsOnlyOneNonzeroWord)
  {
    for (const auto zero_high : {false, true})
    {
      auto configuration = fixture_configuration();
      auto bytes = golden_superblock();
      if (zero_high)
      {
        configuration._session._high = 0;
        put_integer(bytes, 16, 0, 8);
      }
      else
      {
        configuration._session._low = 0;
        put_integer(bytes, 24, 0, 8);
      }
      EXPECT_TRUE(std::holds_alternative<mapped::SuperblockBytes>(
          mapped::encode_initial_superblock(configuration)));
      const auto decoded = mapped::decode_superblock(bytes);
      ASSERT_TRUE(std::holds_alternative<mapped::SuperblockConfiguration>(decoded));
      EXPECT_EQ(std::get<mapped::SuperblockConfiguration>(decoded)._session, configuration._session);
    }
  }

  // Exercise capacities above 16 bits so metadata serialization cannot silently
  // apply the narrower Task/Queue ID width to every reservation count.
  TEST(MappedSuperblock, WideCapacityCountsRetainTheirFullWidth)
  {
    auto configuration = fixture_configuration();
    configuration._limits.fields(0x01000000).message_schemas(65536).symbols(65537);
    const auto encoded = mapped::encode_initial_superblock(configuration);
    ASSERT_TRUE(std::holds_alternative<mapped::SuperblockBytes>(encoded));
    const auto& bytes = std::get<mapped::SuperblockBytes>(encoded);
    EXPECT_EQ(get_integer(bytes, 148, 4), 0x01000000u);
    EXPECT_EQ(get_integer(bytes, 152, 4), 65536u);
    EXPECT_EQ(get_integer(bytes, 156, 4), 65537u);
    const auto decoded = mapped::decode_superblock(bytes);
    ASSERT_TRUE(std::holds_alternative<mapped::SuperblockConfiguration>(decoded));
    const auto& limits = std::get<mapped::SuperblockConfiguration>(decoded)._limits;
    EXPECT_EQ(limits._fields, 0x01000000u);
    EXPECT_EQ(limits._messageSchemas, 65536u);
    EXPECT_EQ(limits._symbols, 65537u);
  }

  // A recognizable header fragment is insufficient: the complete control area
  // must be available before its directory or reserved bytes can be validated.
  TEST(MappedSuperblock, TruncatedPrefixesAreRejected)
  {
    const auto bytes = golden_superblock();
    for (const auto length : {0u, 7u, 15u, 255u, 671u, 4095u})
    {
      SCOPED_TRACE(length);
      expect_error(mapped::decode_superblock(std::span<const std::byte>{bytes}.first(length)),
                   ErrorCode::IoError);
    }
  }

  // Mutate one immutable field at a time in an otherwise valid fixture. These
  // invalid markers, sizes, and capacities must fail before runtime references form.
  TEST(MappedSuperblock, InvalidImmutableHeaderMetadataIsRejected)
  {
    struct Mutation
    {
      std::size_t _offset;
      std::uint64_t _value;
      std::size_t _width;
    };
    const std::array<Mutation, 17> mutations{{
        {0, 0, 1}, {8, 1, 2}, {8, 3, 2}, {10, 1, 2}, {12, 4095, 4},
        {32, 0, 8}, {32, max_unsigned, 8}, {32, fixture_arena - 1, 8},
        {40, fixture_arena + 64, 8}, {48, 3, 2}, {50, 0, 2}, {52, 0, 4},
        {104, 12, 4}, {108, 32, 1}, {109, 2, 1}, {110, 4, 1}, {160, max_unsigned, 8},
    }};
    for (const auto& mutation : mutations)
    {
      SCOPED_TRACE(mutation._offset);
      auto bytes = golden_superblock();
      put_integer(bytes, mutation._offset, mutation._value, mutation._width);
      expect_error(mapped::decode_superblock(bytes), ErrorCode::IoError);
    }
    auto bytes = golden_superblock();
    put_integer(bytes, 16, 0, 8);
    put_integer(bytes, 24, 0, 8);
    expect_error(mapped::decode_superblock(bytes), ErrorCode::IoError);
    bytes = golden_superblock();
    put_integer(bytes, 128, 65536, 4);
    expect_error(mapped::decode_superblock(bytes), ErrorCode::IoError);
  }

  // Reserved bytes are part of the compatibility contract, not spare application
  // storage. Unexpected values cannot be interpreted safely by this format reader.
  TEST(MappedSuperblock, ReservedHeaderAndDirectoryFlagsMustBeZero)
  {
    for (const auto offset : {111u, 184u, 255u, 258u, 2720u, 4095u})
    {
      SCOPED_TRACE(offset);
      auto bytes = golden_superblock();
      bytes[offset] = std::byte{1};
      expect_error(mapped::decode_superblock(bytes), ErrorCode::IoError);
    }
  }

  // Recompute section geometry from recorded capacities instead of trusting
  // on-disk offsets/counts. Forged overlaps, strides, ordering, or wrapped ranges
  // must not become mapped references into unrelated bytes.
  TEST(MappedSuperblock, DirectoryRangesAndGeometryCannotBeForged)
  {
    struct Mutation
    {
      std::size_t _offset;
      std::uint64_t _value;
      std::size_t _width;
    };
    const std::array<Mutation, 10> mutations{{
        {256, 14, 2}, {260, 0, 4}, {260, 0xffffffff, 4},
        {264, max_unsigned, 8}, {272, max_unsigned, 8}, {280, max_unsigned, 8},
        {272, 2, 8}, {280, 128, 8}, {296, 4096, 8}, {360, 64, 8},
    }};
    for (const auto& mutation : mutations)
    {
      SCOPED_TRACE(mutation._offset);
      auto bytes = golden_superblock();
      put_integer(bytes, mutation._offset, mutation._value, mutation._width);
      expect_error(mapped::decode_superblock(bytes), ErrorCode::IoError);
    }
    auto bytes = golden_superblock();
    std::swap_ranges(bytes.begin() + 256, bytes.begin() + 288, bytes.begin() + 288);
    expect_error(mapped::decode_superblock(bytes), ErrorCode::IoError);
  }

  // Caller mistakes are InvalidConfiguration, whereas malformed stored bytes
  // are IoError. Reject impossible profiles before emitting a provisional prefix.
  TEST(MappedSuperblock, InvalidEncoderConfigurationFailsBeforeProducingBytes)
  {
    std::vector<mapped::SuperblockConfiguration> invalid;
    auto configuration = fixture_configuration();
    configuration._session = {};
    invalid.push_back(configuration);
    configuration = fixture_configuration();
    configuration._maxLogBytes = 0;
    invalid.push_back(configuration);
    configuration._maxLogBytes = max_unsigned;
    invalid.push_back(configuration);
    configuration._maxLogBytes = fixture_arena - 1;
    invalid.push_back(configuration);
    configuration = fixture_configuration();
    configuration._liveAbiProfile = 0;
    invalid.push_back(configuration);
    configuration = fixture_configuration();
    configuration._synchronizationVersion = 0;
    invalid.push_back(configuration);
    configuration = fixture_configuration();
    configuration._storage = static_cast<StorageMapping>(99);
    invalid.push_back(configuration);
    configuration = fixture_configuration();
    configuration._segmentBytes = 4160;
    invalid.push_back(configuration);
    configuration = fixture_configuration();
    configuration._limits.tasks(65536);
    invalid.push_back(configuration);
    configuration = fixture_configuration();
    configuration._storage = StorageMapping::Segmented;
    for (const auto quantum : {0u, 4096u, 4161u})
    {
      configuration._segmentBytes = quantum;
      invalid.push_back(configuration);
    }
    configuration._maxLogBytes = 1048576;
    configuration._segmentBytes = 2097152;
    invalid.push_back(configuration);
    for (std::size_t i = 0; i < invalid.size(); ++i)
    {
      SCOPED_TRACE(i);
      expect_error(mapped::encode_initial_superblock(invalid[i]), ErrorCode::InvalidConfiguration);
    }
  }

  // Deliberately nonsensical mutable bytes show this decoder's boundary: it
  // validates immutable metadata only. Successful decoding is not permission to
  // use those roots; Session attachment must acquire the gate and recover first.
  TEST(MappedSuperblock, MutableRootsAndJournalAreOpaqueToStructuralDecoding)
  {
    auto bytes = golden_superblock();
    for (const auto offset : {72u, 80u, 88u, 96u, 112u, 120u, 176u})
    {
      put_integer(bytes, offset, max_unsigned, 8);
    }
    std::fill(bytes.begin() + 672, bytes.begin() + 2720, std::byte{0xa5});
    for (const auto control : {0u, 1u, 2u, 3u, 99u})
    {
      put_integer(bytes, 672, control, 8);
      const auto decoded = mapped::decode_superblock(bytes);
      ASSERT_TRUE(std::holds_alternative<mapped::SuperblockConfiguration>(decoded));
      EXPECT_EQ(std::get<mapped::SuperblockConfiguration>(decoded)._session,
                fixture_configuration()._session);
    }
    std::vector<std::byte> mapped_window(bytes.begin(), bytes.end());
    mapped_window.resize(8192, std::byte{0xff});
    EXPECT_TRUE(std::holds_alternative<mapped::SuperblockConfiguration>(
        mapped::decode_superblock(mapped_window)));
  }

  // Inspection can retain a foreign native profile/protocol or unknown features
  // even when this process cannot attach live. Decoding and live compatibility
  // are separate decisions.
  TEST(MappedSuperblock, PortableDecodePreservesForeignCompatibilityMetadata)
  {
    auto bytes = golden_superblock();
    put_integer(bytes, 50, 9, 2);
    put_integer(bytes, 52, 0xfedcba98, 4);
    put_integer(bytes, 56, 0x8000000000000001, 8);
    const auto decoded = mapped::decode_superblock(bytes);
    ASSERT_TRUE(std::holds_alternative<mapped::SuperblockConfiguration>(decoded));
    const auto& configuration = std::get<mapped::SuperblockConfiguration>(decoded);
    EXPECT_EQ(configuration._synchronizationVersion, 9u);
    EXPECT_EQ(configuration._liveAbiProfile, 0xfedcba98u);
    EXPECT_EQ(configuration._requiredFeatures, 0x8000000000000001u);
    EXPECT_EQ(configuration._optionalFeatures, 0x1020304050607080u);
    expect_error(mapped::check_live_compatibility(configuration, fixture_support()), ErrorCode::IoError);
  }

  // A live adapter must explicitly support the recorded ABI, synchronization
  // protocol, and required features. Supplying capability values here tests the
  // comparison logic; native host qualification is tested separately.
  TEST(MappedSuperblock, LiveCompatibilityRequiresProfileProtocolAndRequiredFeatures)
  {
    auto configuration = fixture_configuration();
    auto supported = fixture_support();
    EXPECT_TRUE(std::holds_alternative<std::monostate>(
        mapped::check_live_compatibility(configuration, supported)));
    supported._liveAbiProfile ^= 1;
    expect_error(mapped::check_live_compatibility(configuration, supported), ErrorCode::IoError);
    supported = fixture_support();
    supported._synchronizationVersion = 2;
    expect_error(mapped::check_live_compatibility(configuration, supported), ErrorCode::IoError);
    supported = fixture_support();
    configuration._requiredFeatures |= std::uint64_t{1} << 63;
    expect_error(mapped::check_live_compatibility(configuration, supported), ErrorCode::IoError);
    supported._supportedRequiredFeatures = configuration._requiredFeatures;
    EXPECT_TRUE(std::holds_alternative<std::monostate>(
        mapped::check_live_compatibility(configuration, supported)));
    supported._liveAbiProfile = 0;
    expect_error(mapped::check_live_compatibility(configuration, supported), ErrorCode::InvalidArgument);
  }

  // A valid segmented description remains portable metadata. Setting a test
  // capability flag demonstrates negotiation, not an implemented segmented backend.
  TEST(MappedSuperblock, SegmentedMetadataRequiresExplicitLiveStorageSupport)
  {
    auto configuration = fixture_configuration();
    configuration._storage = StorageMapping::Segmented;
    configuration._segmentBytes = 4160;
    const auto encoded = mapped::encode_initial_superblock(configuration);
    ASSERT_TRUE(std::holds_alternative<mapped::SuperblockBytes>(encoded));
    const auto decoded = mapped::decode_superblock(std::get<mapped::SuperblockBytes>(encoded));
    ASSERT_TRUE(std::holds_alternative<mapped::SuperblockConfiguration>(decoded));
    const auto& stored = std::get<mapped::SuperblockConfiguration>(decoded);
    EXPECT_EQ(stored._storage, StorageMapping::Segmented);
    EXPECT_EQ(stored._segmentBytes, 4160u);
    auto supported = fixture_support();
    expect_error(mapped::check_live_compatibility(stored, supported), ErrorCode::UnsupportedPolicy);
    supported._segmentedStorage = true;
    EXPECT_TRUE(std::holds_alternative<std::monostate>(
        mapped::check_live_compatibility(stored, supported)));
  }

  // An adapter may impose build-time ceilings below the file format's limits.
  // Attachment must check every recorded reservation, including the string pool,
  // rather than accepting a Session it cannot represent locally.
  TEST(MappedSuperblock, LiveCapacityCeilingsApplyToEveryRecordedReservation)
  {
    auto configuration = fixture_configuration();
    configuration._limits = CapacityLimits{};
    auto supported = fixture_support();
    supported._capacityCeilings = configuration._limits;
    EXPECT_TRUE(std::holds_alternative<std::monostate>(
        mapped::check_live_compatibility(configuration, supported)));

    const std::array<std::uint32_t CapacityLimits::*, 8> members{
        &CapacityLimits::_tasks, &CapacityLimits::_queues, &CapacityLimits::_sourceSets,
        &CapacityLimits::_publishingSets, &CapacityLimits::_membershipsPerSet, &CapacityLimits::_fields,
        &CapacityLimits::_messageSchemas, &CapacityLimits::_symbols,
    };
    for (std::size_t i = 0; i < members.size(); ++i)
    {
      SCOPED_TRACE(i);
      auto ceilings = configuration._limits;
      --(ceilings.*members[i]);
      supported._capacityCeilings = ceilings;
      expect_error(mapped::check_live_compatibility(configuration, supported), ErrorCode::IoError);
    }
    auto ceilings = configuration._limits;
    --ceilings._stringPoolBytes;
    supported._capacityCeilings = ceilings;
    expect_error(mapped::check_live_compatibility(configuration, supported), ErrorCode::IoError);
  }

  // The new profile advertises logical metadata before its first Field exists.
  // A reader limited to 2.0 or lacking the feature must reject that Session.
  TEST(MappedSuperblock, BasicFieldsProfileIsExplicitAndNegotiated)
  {
    auto current = fixture_configuration();
    current._formatMinor = 1;
    current._requiredFeatures = 3;
    const auto encoded = mapped::encode_initial_superblock(current);
    ASSERT_TRUE(std::holds_alternative<mapped::SuperblockBytes>(encoded));
    const auto& bytes = std::get<mapped::SuperblockBytes>(encoded);
    EXPECT_EQ(get_integer(bytes, 10, 2), 1u);
    EXPECT_EQ(get_integer(bytes, 56, 8), 3u);
    const auto decoded = mapped::decode_superblock(bytes);
    ASSERT_TRUE(std::holds_alternative<mapped::SuperblockConfiguration>(decoded));
    EXPECT_EQ(std::get<mapped::SuperblockConfiguration>(decoded)._formatMinor, 1u);
    EXPECT_TRUE(std::holds_alternative<std::monostate>(
        mapped::check_live_compatibility(current, fixture_support())));

    auto legacy = fixture_support();
    legacy._maxFormatMinor = 0;
    legacy._supportedRequiredFeatures = 1;
    expect_error(mapped::check_live_compatibility(current, legacy), ErrorCode::IoError);
    EXPECT_TRUE(std::holds_alternative<std::monostate>(
        mapped::check_live_compatibility(fixture_configuration(), legacy)));
    legacy._maxFormatMinor = 1;
    expect_error(mapped::check_live_compatibility(current, legacy), ErrorCode::IoError);
  }

  // A feature flag alone cannot reinterpret 2.0, and a 2.1 header cannot omit
  // required logical semantics. These are malformed profiles, not optional data.
  TEST(MappedSuperblock, FormatAndBasicFieldFeatureCannotDisagree)
  {
    auto bytes = golden_superblock();
    for (const auto [minor, features] :
         {std::pair{0u, 3u}, std::pair{1u, 1u}, std::pair{1u, 2u}, std::pair{2u, 3u}})
    {
      SCOPED_TRACE(minor);
      SCOPED_TRACE(features);
      auto changed = bytes;
      put_integer(changed, 10, minor, 2);
      put_integer(changed, 56, features, 8);
      expect_error(mapped::decode_superblock(changed), ErrorCode::IoError);
    }
    auto configuration = fixture_configuration();
    configuration._requiredFeatures = 3;
    expect_error(mapped::encode_initial_superblock(configuration), ErrorCode::InvalidConfiguration);
    configuration._formatMinor = 1;
    configuration._requiredFeatures = 1;
    expect_error(mapped::encode_initial_superblock(configuration), ErrorCode::InvalidConfiguration);
  }
  // Read support can advance before public factories create the new profile.
  // The two 2.2 representations share geometry, but loss needs a separate bit.
  TEST(MappedSuperblock, DeliveryProfilesMatchIndependentHeaderBytes)
  {
    for (const auto features : {std::uint64_t{7}, std::uint64_t{15}})
    {
      auto configuration = fixture_configuration();
      configuration._formatMinor = 2;
      configuration._requiredFeatures = features;
      auto golden = golden_superblock();
      put_integer(golden, 10, 2, 2);
      put_integer(golden, 56, features, 8);
      const auto encoded = mapped::encode_initial_superblock(configuration);
      ASSERT_TRUE(std::holds_alternative<mapped::SuperblockBytes>(encoded));
      EXPECT_EQ(std::get<mapped::SuperblockBytes>(encoded), golden);
      const auto decoded = mapped::decode_superblock(golden);
      ASSERT_TRUE(std::holds_alternative<mapped::SuperblockConfiguration>(decoded));
      EXPECT_EQ(std::get<mapped::SuperblockConfiguration>(decoded)._formatMinor, 2u);
      EXPECT_EQ(std::get<mapped::SuperblockConfiguration>(decoded)._requiredFeatures, features);
    }
  }

  TEST(MappedSuperblock, EveryKnownRequiredFeatureCombinationHasOneCanonicalMinor)
  {
    // A known bit cannot be disguised as an optional interpretation of an older
    // minor. Loss without core delivery is invalid even if a reader knows loss.
    for (std::uint16_t minor = 0; minor <= 2; ++minor)
    {
      for (std::uint64_t features = 0; features < 16; ++features)
      {
        SCOPED_TRACE(minor);
        SCOPED_TRACE(features);
        const auto valid = (minor == 0 && features == 1) ||
                           (minor == 1 && features == 3) ||
                           (minor == 2 && (features == 7 || features == 15));
        auto configuration = fixture_configuration();
        configuration._formatMinor = minor;
        configuration._requiredFeatures = features;
        auto bytes = golden_superblock();
        put_integer(bytes, 10, minor, 2);
        put_integer(bytes, 56, features, 8);
        if (valid)
        {
          EXPECT_TRUE(std::holds_alternative<mapped::SuperblockBytes>(
              mapped::encode_initial_superblock(configuration)));
          EXPECT_TRUE(std::holds_alternative<mapped::SuperblockConfiguration>(mapped::decode_superblock(bytes)));
        }
        else
        {
          expect_error(mapped::encode_initial_superblock(configuration), ErrorCode::InvalidConfiguration);
          expect_error(mapped::decode_superblock(bytes), ErrorCode::IoError);
        }
      }
    }
  }

  TEST(MappedSuperblock, DeliveryRecognitionDoesNotChangeProvisionalCreationDefaults)
  {
    auto configuration = fixture_configuration();
    const mapped::SuperblockConfiguration defaults;
    configuration._formatMinor = defaults._formatMinor;
    configuration._requiredFeatures = defaults._requiredFeatures;
    const auto encoded = mapped::encode_initial_superblock(configuration);
    ASSERT_TRUE(std::holds_alternative<mapped::SuperblockBytes>(encoded));
    const auto& bytes = std::get<mapped::SuperblockBytes>(encoded);
    EXPECT_EQ(get_integer(bytes, 10, 2), 1u);
    EXPECT_EQ(get_integer(bytes, 56, 8), 3u);
  }

  TEST(MappedSuperblock, AllowedLossReaderRetainsCoreCompatibilityAndOlderReadersRejectLoss)
  {
    auto core = fixture_configuration();
    core._formatMinor = 2;
    core._requiredFeatures = 7;
    const auto current = fixture_support();
    EXPECT_TRUE(std::holds_alternative<std::monostate>(mapped::check_live_compatibility(core, current)));

    auto older = current;
    older._maxFormatMinor = 1;
    older._supportedRequiredFeatures = 3;
    expect_error(mapped::check_live_compatibility(core, older), ErrorCode::IoError);
    EXPECT_TRUE(std::holds_alternative<std::monostate>(
        mapped::check_live_compatibility(fixture_configuration(), older)));

    auto loss = core;
    loss._requiredFeatures = 15;
    EXPECT_TRUE(std::holds_alternative<std::monostate>(mapped::check_live_compatibility(loss, current)));
    auto coreReader = current;
    coreReader._supportedRequiredFeatures = 7;
    EXPECT_TRUE(std::holds_alternative<std::monostate>(mapped::check_live_compatibility(core, coreReader)));
    expect_error(mapped::check_live_compatibility(loss, coreReader), ErrorCode::IoError);
  }

  TEST(MappedSuperblock, UnknownDeliveryRequirementsRemainInspectableButCannotAttachLive)
  {
    auto bytes = golden_superblock();
    put_integer(bytes, 10, 2, 2);
    put_integer(bytes, 56, (std::uint64_t{1} << 63) | 7, 8);
    const auto decoded = mapped::decode_superblock(bytes);
    ASSERT_TRUE(std::holds_alternative<mapped::SuperblockConfiguration>(decoded));
    const auto& configuration = std::get<mapped::SuperblockConfiguration>(decoded);
    EXPECT_EQ(configuration._requiredFeatures, (std::uint64_t{1} << 63) | 7);
    expect_error(mapped::check_live_compatibility(configuration, fixture_support()), ErrorCode::IoError);
    put_integer(bytes, 10, 3, 2);
    expect_error(mapped::decode_superblock(bytes), ErrorCode::IoError);
  }
} // namespace
