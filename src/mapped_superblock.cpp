#include "mapped_superblock.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

namespace angaria::detail::mapped
{
  namespace
  {
    constexpr std::array<std::byte, 8> MAGIC{
        std::byte{0x41}, std::byte{0x4E}, std::byte{0x47}, std::byte{0x41},
        std::byte{0x52}, std::byte{0x49}, std::byte{0x41}, std::byte{0},
    };

    // Callers validate the complete prefix before using fixed offsets. These
    // byte operations work on unaligned buffers and never alias native structs.
    template <typename T>
    T read_integer(std::span<const std::byte> bytes, std::size_t offset)
    {
      static_assert(std::is_unsigned_v<T> && sizeof(T) <= sizeof(std::uint64_t));
      std::uint64_t value{};
      for (std::size_t index = 0; index < sizeof(T); ++index)
      {
        value |= std::uint64_t{std::to_integer<std::uint8_t>(bytes[offset + index])} << (index * 8);
      }
      return static_cast<T>(value);
    }

    template <typename T>
    void write_integer(std::span<std::byte> bytes, std::size_t offset, T value)
    {
      static_assert(std::is_unsigned_v<T> && sizeof(T) <= sizeof(std::uint64_t));
      for (std::size_t index = 0; index < sizeof(T); ++index)
      {
        bytes[offset + index] = static_cast<std::byte>((std::uint64_t{value} >> (index * 8)) & 0xFF);
      }
    }

    Error failure(ErrorCode code, std::string_view path, std::string message)
    {
      return Error{code, std::move(message), std::string{path}};
    }

    std::optional<Error> validate_configuration(const SuperblockConfiguration& configuration,
                                                ErrorCode code)
    {
      if (configuration._formatMinor > delivery_format_minor)
      {
        return failure(code, "superblock.format", "Unsupported format version; expected 2.0, 2.1, or 2.2");
      }
      // Unknown required bits remain available to portable metadata inspection;
      // live admission rejects them unless its adapter explicitly supports them.
      // Known bits must always retain their canonical minor-version meaning.
      constexpr auto known = journal_feature | basic_fields_feature |
                             native_delivery_feature | allow_loss_feature;
      const auto features = configuration._requiredFeatures & known;
      const auto expected = configuration._formatMinor == 0 ? journal_feature :
          configuration._formatMinor == 1 ? journal_feature | basic_fields_feature :
          journal_feature | basic_fields_feature | native_delivery_feature;
      if (features != expected &&
          !(configuration._formatMinor == delivery_format_minor && features == (expected | allow_loss_feature)))
      {
        return failure(code, "superblock.required_features",
                       "Known required features must match the 2.0, 2.1, or 2.2 delivery profile");
      }
      if (configuration._session == SessionId{})
      {
        return failure(code, "superblock.session", "Session identity must be nonzero");
      }
      if (!valid_log_limit(configuration._maxLogBytes))
      {
        return failure(code, "superblock.max_log_bytes", "Byte limit must be in 1..INT64_MAX");
      }
      const auto layout = make_fixed_layout(configuration._limits);
      if (!layout)
      {
        return failure(code, "superblock.limits", "Capacities exceed format ranges or overflow the layout");
      }
      if (layout->_arenaBegin > configuration._maxLogBytes)
      {
        return failure(code, "superblock.max_log_bytes", "Fixed reservation exceeds the byte limit");
      }
      switch (configuration._storage)
      {
      case StorageMapping::TwoPlane:
        if (configuration._segmentBytes != 0)
        {
          return failure(code, "superblock.segment_bytes", "Two-plane storage requires a zero segment quantum");
        }
        break;
      case StorageMapping::Segmented:
        if (!valid_segment_bytes(configuration._segmentBytes) ||
            configuration._segmentBytes > configuration._maxLogBytes)
        {
          return failure(code, "superblock.segment_bytes", "Invalid or unbudgeted segment quantum");
        }
        break;
      default:
        return failure(code, "superblock.storage", "Unknown storage mapping");
      }
      if (configuration._synchronizationVersion == 0)
      {
        return failure(code, "superblock.synchronization_version", "Synchronization version must be nonzero");
      }
      if (configuration._liveAbiProfile == 0)
      {
        return failure(code, "superblock.live_abi_profile", "Live ABI profile must be nonzero");
      }
      return std::nullopt;
    }

    bool zero_bytes(std::span<const std::byte> bytes, std::size_t offset, std::size_t length)
    {
      const auto region = bytes.subspan(offset, length);
      return std::all_of(region.begin(), region.end(),
                         [](std::byte value)
                         {
                           return value == std::byte{0};
                         });
    }
  } // namespace

  Result<SuperblockBytes> encode_initial_superblock(const SuperblockConfiguration& configuration)
  {
    if (const auto error = validate_configuration(configuration, ErrorCode::InvalidConfiguration))
    {
      return *error;
    }
    const auto layout = *make_fixed_layout(configuration._limits);
    SuperblockBytes bytes{};
    std::copy(MAGIC.begin(), MAGIC.end(), bytes.begin());
    write_integer<std::uint16_t>(bytes, 8, format_major);
    write_integer<std::uint16_t>(bytes, 10, configuration._formatMinor);
    write_integer<std::uint32_t>(bytes, 12, superblock_bytes);
    write_integer<std::uint64_t>(bytes, 16, configuration._session._high);
    write_integer<std::uint64_t>(bytes, 24, configuration._session._low);
    write_integer<std::uint64_t>(bytes, 32, configuration._maxLogBytes);
    write_integer<std::uint64_t>(bytes, 40, layout._arenaBegin);
    write_integer<std::uint16_t>(bytes, 48, configuration._storage == StorageMapping::TwoPlane ? 1 : 2);
    write_integer<std::uint16_t>(bytes, 50, configuration._synchronizationVersion);
    write_integer<std::uint32_t>(bytes, 52, configuration._liveAbiProfile);
    write_integer<std::uint64_t>(bytes, 56, configuration._requiredFeatures);
    write_integer<std::uint64_t>(bytes, 64, configuration._optionalFeatures);
    write_integer<std::uint64_t>(bytes, 88, layout._arenaBegin);
    write_integer<std::uint32_t>(bytes, 104, section_count);
    bytes[108] = std::byte{64};
    bytes[109] = std::byte{1};
    bytes[110] = std::byte{8};
    const auto& limits = configuration._limits;
    const std::array<std::uint32_t, 8> counts{
        limits._tasks, limits._queues, limits._sourceSets, limits._publishingSets,
        limits._membershipsPerSet, limits._fields, limits._messageSchemas, limits._symbols,
    };
    for (std::size_t index = 0; index < counts.size(); ++index)
    {
      write_integer<std::uint32_t>(bytes, 128 + index * 4, counts[index]);
    }
    write_integer<std::uint64_t>(bytes, 160, limits._stringPoolBytes);
    write_integer<std::uint64_t>(bytes, 168, configuration._segmentBytes);
    write_integer<std::uint64_t>(bytes, 176, layout._arenaBegin);
    for (std::size_t index = 0; index < layout._sections.size(); ++index)
    {
      const auto& section = layout._sections[index];
      const auto offset = 256 + index * 32;
      write_integer<std::uint16_t>(bytes, offset, static_cast<std::uint16_t>(section._kind));
      write_integer<std::uint16_t>(bytes, offset + 2, section._flags);
      write_integer<std::uint32_t>(bytes, offset + 4, section._stride);
      write_integer<std::uint64_t>(bytes, offset + 8, section._offset);
      write_integer<std::uint64_t>(bytes, offset + 16, section._count);
      write_integer<std::uint64_t>(bytes, offset + 24, section._byteLength);
    }
    return bytes;
  }

  Result<SuperblockConfiguration> decode_superblock(std::span<const std::byte> input)
  {
    if (input.size() < superblock_bytes)
    {
      return failure(ErrorCode::IoError, "superblock", "Truncated 4096-byte superblock");
    }
    const auto bytes = input.first(superblock_bytes);
    const auto malformed = [](std::string_view path, std::string message)
    {
      return failure(ErrorCode::IoError, path, std::move(message));
    };
    if (!std::equal(MAGIC.begin(), MAGIC.end(), bytes.begin()))
    {
      return malformed("superblock.magic", "Invalid ANGARIA magic");
    }
    if (read_integer<std::uint16_t>(bytes, 8) != format_major ||
        read_integer<std::uint16_t>(bytes, 10) > delivery_format_minor)
    {
      return malformed("superblock.format", "Unsupported format version; expected 2.0, 2.1, or 2.2");
    }
    if (read_integer<std::uint32_t>(bytes, 12) != superblock_bytes)
    {
      return malformed("superblock.header_bytes", "Invalid superblock size");
    }
    if (read_integer<std::uint32_t>(bytes, 104) != section_count)
    {
      return malformed("superblock.sections", "Expected thirteen fixed sections");
    }
    if (bytes[108] != std::byte{64} || bytes[109] != std::byte{1} || bytes[110] != std::byte{8})
    {
      return malformed("superblock.representation", "Unsupported address width, byte order, or atomic width");
    }
    if (bytes[111] != std::byte{0} || !zero_bytes(bytes, 184, 72) || !zero_bytes(bytes, 2720, 1376))
    {
      return malformed("superblock.reserved", "Nonzero reserved header bytes");
    }

    SuperblockConfiguration configuration;
    configuration._formatMinor = read_integer<std::uint16_t>(bytes, 10);
    configuration._session._high = read_integer<std::uint64_t>(bytes, 16);
    configuration._session._low = read_integer<std::uint64_t>(bytes, 24);
    configuration._maxLogBytes = read_integer<std::uint64_t>(bytes, 32);
    switch (read_integer<std::uint16_t>(bytes, 48))
    {
    case 1:
      configuration._storage = StorageMapping::TwoPlane;
      break;
    case 2:
      configuration._storage = StorageMapping::Segmented;
      break;
    default:
      return malformed("superblock.storage", "Unknown storage mapping code");
    }
    configuration._synchronizationVersion = read_integer<std::uint16_t>(bytes, 50);
    configuration._liveAbiProfile = read_integer<std::uint32_t>(bytes, 52);
    configuration._requiredFeatures = read_integer<std::uint64_t>(bytes, 56);
    configuration._optionalFeatures = read_integer<std::uint64_t>(bytes, 64);
    configuration._limits.tasks(read_integer<std::uint32_t>(bytes, 128))
        .queues(read_integer<std::uint32_t>(bytes, 132))
        .source_sets(read_integer<std::uint32_t>(bytes, 136))
        .publishing_sets(read_integer<std::uint32_t>(bytes, 140))
        .memberships_per_set(read_integer<std::uint32_t>(bytes, 144))
        .fields(read_integer<std::uint32_t>(bytes, 148))
        .message_schemas(read_integer<std::uint32_t>(bytes, 152))
        .symbols(read_integer<std::uint32_t>(bytes, 156))
        .string_pool_bytes(read_integer<std::uint64_t>(bytes, 160));
    configuration._segmentBytes = read_integer<std::uint64_t>(bytes, 168);
    if (const auto error = validate_configuration(configuration, ErrorCode::IoError))
    {
      return *error;
    }
    const auto layout = *make_fixed_layout(configuration._limits);
    if (read_integer<std::uint64_t>(bytes, 40) != layout._arenaBegin)
    {
      return malformed("superblock.fixed_bytes", "Fixed extent does not match recorded capacities");
    }
    for (std::size_t index = 0; index < layout._sections.size(); ++index)
    {
      const auto& expected = layout._sections[index];
      const auto offset = 256 + index * 32;
      if (read_integer<std::uint16_t>(bytes, offset) != static_cast<std::uint16_t>(expected._kind) ||
          read_integer<std::uint16_t>(bytes, offset + 2) != expected._flags ||
          read_integer<std::uint32_t>(bytes, offset + 4) != expected._stride ||
          read_integer<std::uint64_t>(bytes, offset + 8) != expected._offset ||
          read_integer<std::uint64_t>(bytes, offset + 16) != expected._count ||
          read_integer<std::uint64_t>(bytes, offset + 24) != expected._byteLength)
      {
        return malformed("superblock.sections[" + std::to_string(index) + "]",
                         "Section kind, flags, stride, offset, count, or byte length is invalid");
      }
    }
    return configuration;
  }

  OperationResult check_live_compatibility(const SuperblockConfiguration& configuration,
                                           const LiveCompatibility& supported)
  {
    if (const auto error = validate_configuration(configuration, ErrorCode::IoError))
    {
      return *error;
    }
    if (supported._liveAbiProfile == 0 || supported._synchronizationVersion == 0)
    {
      return failure(ErrorCode::InvalidArgument, "compatibility", "The native adapter must declare its profile and protocol");
    }
    if (configuration._formatMinor > supported._maxFormatMinor)
    {
      return failure(ErrorCode::IoError, "superblock.format", "Format minor exceeds the reader's supported profile");
    }
    if (configuration._liveAbiProfile != supported._liveAbiProfile)
    {
      return failure(ErrorCode::IoError, "superblock.live_abi_profile", "Live ABI profile is unsupported");
    }
    if (configuration._synchronizationVersion != supported._synchronizationVersion)
    {
      return failure(ErrorCode::IoError, "superblock.synchronization_version", "Synchronization protocol is unsupported");
    }
    if ((configuration._requiredFeatures & ~supported._supportedRequiredFeatures) != 0)
    {
      return failure(ErrorCode::IoError, "superblock.required_features", "Required features are unsupported");
    }
    if (configuration._storage == StorageMapping::Segmented && !supported._segmentedStorage)
    {
      return failure(ErrorCode::UnsupportedPolicy, "superblock.storage", "Segmented runtime storage is unsupported");
    }
    if (supported._capacityCeilings)
    {
      const auto& limits = configuration._limits;
      const auto& ceiling = *supported._capacityCeilings;
      const std::array<std::pair<std::string_view, bool>, 9> checks{{
          {"tasks", limits._tasks <= ceiling._tasks},
          {"queues", limits._queues <= ceiling._queues},
          {"source_sets", limits._sourceSets <= ceiling._sourceSets},
          {"publishing_sets", limits._publishingSets <= ceiling._publishingSets},
          {"memberships_per_set", limits._membershipsPerSet <= ceiling._membershipsPerSet},
          {"fields", limits._fields <= ceiling._fields},
          {"message_schemas", limits._messageSchemas <= ceiling._messageSchemas},
          {"symbols", limits._symbols <= ceiling._symbols},
          {"string_pool_bytes", limits._stringPoolBytes <= ceiling._stringPoolBytes},
      }};
      for (const auto& [name, fits] : checks)
      {
        if (!fits)
        {
          return failure(ErrorCode::IoError, "superblock.limits." + std::string{name},
                         "Recorded capacity exceeds the build ceiling");
        }
      }
    }
    return std::monostate{};
  }
} // namespace angaria::detail::mapped
