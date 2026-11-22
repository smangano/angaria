#include "session_metadata.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

namespace angaria::detail::mapped
{
  namespace
  {
    struct MetadataFailure
    {
      Error _error;
    };

    [[noreturn]] void fail(ErrorCode code, std::string path, std::string message)
    {
      throw MetadataFailure{Error{code, std::move(message), std::move(path)}};
    }

    void require(bool condition, ErrorCode code, const char* path, const char* message)
    {
      if (!condition)
      {
        fail(code, path, message);
      }
    }

    template <typename T>
    T read_integer(std::span<const std::byte> bytes, std::size_t offset)
    {
      static_assert(std::is_unsigned_v<T> && sizeof(T) <= sizeof(std::uint64_t));
      std::uint64_t value{};
      for (std::size_t index = 0; index < sizeof(T); ++index)
      {
        value |= std::uint64_t{std::to_integer<unsigned char>(bytes[offset + index])} << (8 * index);
      }
      return static_cast<T>(value);
    }

    template <typename T>
    void write_integer(std::span<std::byte> bytes, std::size_t offset, T value)
    {
      static_assert(std::is_unsigned_v<T> && sizeof(T) <= sizeof(std::uint64_t));
      for (std::size_t index = 0; index < sizeof(T); ++index)
      {
        bytes[offset + index] = static_cast<std::byte>((std::uint64_t{value} >> (8 * index)) & 0xff);
      }
    }

    bool zero(std::span<const std::byte> bytes)
    {
      return std::all_of(bytes.begin(), bytes.end(),
                         [](std::byte value)
                         {
                           return value == std::byte{};
                         });
    }

    bool valid_utf8(std::string_view text)
    {
      std::size_t cursor{};
      while (cursor < text.size())
      {
        const auto first = static_cast<unsigned char>(text[cursor++]);
        if (first < 0x80)
        {
          continue;
        }
        unsigned continuations{};
        std::uint32_t value{};
        std::uint32_t minimum{};
        if (first >= 0xc2 && first <= 0xdf)
        {
          continuations = 1;
          value = first & 0x1f;
          minimum = 0x80;
        }
        else if (first >= 0xe0 && first <= 0xef)
        {
          continuations = 2;
          value = first & 0x0f;
          minimum = 0x800;
        }
        else if (first >= 0xf0 && first <= 0xf4)
        {
          continuations = 3;
          value = first & 0x07;
          minimum = 0x10000;
        }
        else
        {
          return false;
        }
        if (continuations > text.size() - cursor)
        {
          return false;
        }
        for (unsigned index = 0; index != continuations; ++index)
        {
          const auto next = static_cast<unsigned char>(text[cursor++]);
          if ((next & 0xc0) != 0x80)
          {
            return false;
          }
          value = (value << 6) | (next & 0x3f);
        }
        if (value < minimum || value > 0x10ffff || (value >= 0xd800 && value <= 0xdfff))
        {
          return false;
        }
      }
      return true;
    }

    void check_text(std::string_view text, bool required, bool noNull, ErrorCode code, const char* path)
    {
      require(!required || !text.empty(), code, path, "Required metadata string is empty");
      require(valid_utf8(text), code, path, "Metadata string is not valid UTF-8");
      require(!noNull || text.find('\0') == text.npos, code, path, "Metadata string contains a null character");
    }

    std::uint64_t body_begin(std::uint64_t offset, std::uint64_t length, ErrorCode code, const char* path)
    {
      require(offset >= superblock_bytes && offset % record_alignment == 0 &&
                  contains_range(max_log_bytes, offset, 64), code, path, "Invalid record offset");
      const auto begin = offset + 64;
      require(contains_range(max_log_bytes, begin, length), code, path, "Metadata body exceeds format bounds");
      return begin;
    }

    template <std::size_t N>
    std::vector<std::byte> encode_strings(std::size_t fixedBytes, std::size_t sliceStart,
                                          const std::array<std::string_view, N>& strings,
                                          std::uint64_t offset, const char* path)
    {
      std::uint64_t length = fixedBytes;
      for (const auto text : strings)
      {
        const auto next = checked_add(length, text.size());
        require(next && *next <= std::numeric_limits<std::size_t>::max() &&
                    *next <= static_cast<std::uint64_t>(std::numeric_limits<std::ptrdiff_t>::max()),
                ErrorCode::InvalidArgument, path, "Metadata string lengths exceed host bounds");
        length = *next;
      }
      const auto begin = body_begin(offset, length, ErrorCode::InvalidArgument, path);
      std::vector<std::byte> bytes(static_cast<std::size_t>(length));
      std::size_t cursor = fixedBytes;
      for (std::size_t index = 0; index != strings.size(); ++index)
      {
        const auto text = strings[index];
        if (text.empty())
        {
          continue;
        }
        write_integer<std::uint64_t>(bytes, sliceStart + index * 16, begin + cursor);
        write_integer<std::uint64_t>(bytes, sliceStart + index * 16 + 8, text.size());
        for (const auto character : text)
        {
          bytes[cursor++] = static_cast<std::byte>(static_cast<unsigned char>(character));
        }
      }
      return bytes;
    }

    template <std::size_t N>
    std::array<std::string, N> decode_strings(std::span<const std::byte> bytes, std::size_t fixedBytes,
                                              std::size_t sliceStart, std::uint64_t offset,
                                              const char* path)
    {
      require(bytes.size() >= fixedBytes, ErrorCode::IoError, path, "Truncated metadata fixed body");
      const auto begin = body_begin(offset, bytes.size(), ErrorCode::IoError, path);
      std::array<std::string, N> strings;
      std::size_t cursor = fixedBytes;
      for (std::size_t index = 0; index != N; ++index)
      {
        const auto reference = read_integer<std::uint64_t>(bytes, sliceStart + index * 16);
        const auto length = read_integer<std::uint64_t>(bytes, sliceStart + index * 16 + 8);
        if (length == 0)
        {
          require(reference == 0, ErrorCode::IoError, path, "Empty metadata slice must have a zero offset");
          continue;
        }
        require(reference == begin + cursor && length <= bytes.size() - cursor,
                ErrorCode::IoError, path, "Metadata slice is outside its canonical owning body");
        strings[index].assign(reinterpret_cast<const char*>(bytes.data() + cursor),
                               static_cast<std::size_t>(length));
        cursor += static_cast<std::size_t>(length);
      }
      require(bytes.size() - cursor < section_alignment && zero(bytes.subspan(cursor)),
              ErrorCode::IoError, path, "Unexpected metadata allocation padding");
      return strings;
    }

    std::uint16_t storage_code(StorageMapping storage, ErrorCode code)
    {
      switch (storage)
      {
      case StorageMapping::TwoPlane:
        return 1;
      case StorageMapping::Segmented:
        return 2;
      }
      fail(code, "session.storage", "Unknown storage mapping");
    }

    std::uint16_t delivery_code(DeliveryPolicy policy, ErrorCode code)
    {
      switch (policy)
      {
      case DeliveryPolicy::RecoverFromLog:
        return 1;
      case DeliveryPolicy::AllowLoss:
        return 2;
      }
      fail(code, "session.defaults.delivery", "Unknown delivery policy");
    }

    std::uint16_t selection_code(SelectionPolicy policy, ErrorCode code)
    {
      switch (policy)
      {
      case SelectionPolicy::Strict:
        return 1;
      case SelectionPolicy::Fair:
        return 2;
      case SelectionPolicy::WeightedRandom:
        return 3;
      }
      fail(code, "session.defaults.selection", "Unknown selection policy");
    }

    std::uint16_t wait_code(WaitPolicy policy, ErrorCode code)
    {
      switch (policy)
      {
      case WaitPolicy::Spin:
        return 1;
      case WaitPolicy::EventFd:
        return 2;
      }
      fail(code, "session.defaults.wait", "Unknown wait policy");
    }

    std::uint16_t acceptance_code(AcceptancePolicy policy, ErrorCode code)
    {
      switch (policy)
      {
      case AcceptancePolicy::AllDestinations:
        return 1;
      case AcceptancePolicy::Partial:
        return 2;
      }
      fail(code, "session.defaults.acceptance", "Unknown acceptance policy");
    }

    template <typename Policy, std::size_t N>
    Policy decode_policy(std::uint16_t code, const std::array<Policy, N>& policies, const char* path)
    {
      require(code > 0 && code <= N, ErrorCode::IoError, path, "Unknown policy code");
      return policies[code - 1];
    }

    void check_session(const SessionDefinition& definition, ErrorCode code)
    {
      const auto& options = definition._options;
      const auto& metadata = definition._metadata;
      require(metadata._session != SessionId{}, code, "session.identity", "Session identity is zero");
      require(metadata._createdBy != TaskId::Invalid &&
                  static_cast<std::uint16_t>(metadata._createdBy) <= options._limits._tasks,
              code, "session.creator", "Creator Task is outside the recorded capacity");
      const auto layout = make_fixed_layout(options._limits);
      require(layout && valid_log_limit(options._maxLogBytes) && layout->_arenaBegin <= options._maxLogBytes,
              code, "session.limits", "Invalid session capacities or byte limit");
      const auto ringBytes = checked_multiply(options._defaults._ringCapacity, ring_slot_bytes);
      const auto ringRecord = ringBytes ? checked_add(64, *ringBytes) : std::nullopt;
      const auto ringAllocation = ringRecord ? checked_align(*ringRecord, section_alignment) : std::nullopt;
      require(options._defaults._ringCapacity != 0 && ringAllocation && *ringAllocation <= max_log_bytes,
              code, "session.defaults.ring_capacity", "Ring capacity exceeds format bounds");
      switch (options._storage)
      {
      case StorageMapping::TwoPlane:
        require(!options._segmentBytes, code, "session.segment_bytes", "Two-plane storage cannot specify a segment quantum");
        break;
      case StorageMapping::Segmented:
        require(options._segmentBytes && valid_segment_bytes(*options._segmentBytes) &&
                    *options._segmentBytes <= options._maxLogBytes,
                code, "session.segment_bytes", "Invalid or unbudgeted segment quantum");
        break;
      default:
        fail(code, "session.storage", "Unknown storage mapping");
      }
      require(options._applicationVersion == metadata._applicationVersion, code, "session.application_version",
              "Session options and metadata disagree on application version");
      check_text(options._name, true, true, code, "session.name");
      check_text(metadata._angariaVersion, true, false, code, "session.angaria_version");
      check_text(metadata._applicationVersion, false, false, code, "session.application_version");
      check_text(metadata._creatorUser, true, true, code, "session.creator_user");
    }

    void check_task_descriptor(const TaskDescriptor& descriptor, ErrorCode code)
    {
      check_text(descriptor._name, true, true, code, "task.name");
      check_text(descriptor._doc, false, false, code, "task.doc");
      switch (descriptor._kind)
      {
      case TaskKind::Process:
        require(!descriptor._process, code, "task.process", "A process Task cannot have a parent");
        break;
      case TaskKind::Thread:
        require(descriptor._process && *descriptor._process != TaskId::Invalid,
                code, "task.process", "A thread Task needs a nonzero parent Process Task");
        break;
      default:
        fail(code, "task.kind", "Unknown Task kind");
      }
    }

    void check_task(const TaskDefinition& definition, ErrorCode code)
    {
      require(definition._id != TaskId::Invalid, code, "task.id", "Task identity is zero");
      require(definition._registration._createdBy != TaskId::Invalid && definition._registration._revision != 0,
              code, "task.registration", "Task creator and registration revision must be nonzero");
      check_task_descriptor(definition._descriptor, code);
      require(definition._descriptor._kind != TaskKind::Thread ||
                  definition._descriptor._process != definition._id,
              code, "task.process", "A thread Task needs a distinct parent Process Task");
    }
  } // namespace

  OperationResult validate_metadata_text(std::string_view text, bool required, bool noNull,
                                         const char* path, ErrorCode code)
  {
    try
    {
      check_text(text, required, noNull, code, path);
      return std::monostate{};
    }
    catch (const MetadataFailure& failure)
    {
      return failure._error;
    }
  }

  OperationResult validate_task_descriptor(const TaskDescriptor& descriptor)
  {
    try
    {
      check_task_descriptor(descriptor, ErrorCode::InvalidArgument);
      return std::monostate{};
    }
    catch (const MetadataFailure& failure)
    {
      return failure._error;
    }
  }

  Result<std::vector<std::byte>> encode_session_definition(const SessionDefinition& definition,
                                                           std::uint64_t recordOffset)
  {
    try
    {
      check_session(definition, ErrorCode::InvalidArgument);
      const auto directory8 = definition._options._directory.generic_u8string();
      const std::string directory(reinterpret_cast<const char*>(directory8.data()), directory8.size());
      check_text(directory, true, true, ErrorCode::InvalidArgument, "session.original_directory");
      const std::array<std::string_view, 5> strings{
          definition._options._name, definition._metadata._angariaVersion,
          definition._metadata._applicationVersion, definition._metadata._creatorUser, directory};
      auto bytes = encode_strings(128, 48, strings, recordOffset, "session.definition");
      write_integer<std::uint16_t>(bytes, 0, static_cast<std::uint16_t>(definition._metadata._createdBy));
      write_integer<std::uint16_t>(bytes, 2, storage_code(definition._options._storage, ErrorCode::InvalidArgument));
      write_integer<std::uint16_t>(bytes, 4, delivery_code(definition._options._defaults._delivery, ErrorCode::InvalidArgument));
      write_integer<std::uint16_t>(bytes, 6, selection_code(definition._options._defaults._selection, ErrorCode::InvalidArgument));
      write_integer<std::uint16_t>(bytes, 8, wait_code(definition._options._defaults._wait, ErrorCode::InvalidArgument));
      write_integer<std::uint16_t>(bytes, 10, acceptance_code(definition._options._defaults._acceptance, ErrorCode::InvalidArgument));
      write_integer<std::uint64_t>(bytes, 16, static_cast<std::uint64_t>(definition._metadata._createdAt.time_since_epoch().count()));
      write_integer<std::uint64_t>(bytes, 24, definition._options._defaults._ringCapacity);
      write_integer<std::uint64_t>(bytes, 32, definition._options._segmentBytes.value_or(0));
      write_integer<std::uint64_t>(bytes, 40, definition._policyFeatures);
      return bytes;
    }
    catch (const MetadataFailure& failure)
    {
      return failure._error;
    }
    catch (const std::filesystem::filesystem_error& error)
    {
      return Error{ErrorCode::InvalidArgument, error.what(), "session.original_directory"};
    }
  }

  Result<SessionDefinition> decode_session_definition(std::span<const std::byte> immutableBody,
                                                       std::uint64_t recordOffset,
                                                       const SuperblockConfiguration& configuration)
  {
    try
    {
      const auto strings = decode_strings<5>(immutableBody, 128, 48, recordOffset, "session.definition");
      require(zero(immutableBody.subspan(12, 4)), ErrorCode::IoError, "session.definition", "Reserved Session bytes are nonzero");
      SessionDefinition definition;
      definition._metadata._session = configuration._session;
      definition._metadata._createdBy = static_cast<TaskId>(read_integer<std::uint16_t>(immutableBody, 0));
      definition._metadata._createdAt = UtcTimestamp{std::chrono::nanoseconds{
          std::bit_cast<std::int64_t>(read_integer<std::uint64_t>(immutableBody, 16))}};
      definition._metadata._angariaVersion = strings[1];
      definition._metadata._applicationVersion = strings[2];
      definition._metadata._creatorUser = strings[3];
      definition._options._name = strings[0];
      definition._options._applicationVersion = strings[2];
      check_text(strings[4], true, true, ErrorCode::IoError, "session.original_directory");
      definition._options._directory = std::filesystem::path{std::u8string{
          reinterpret_cast<const char8_t*>(strings[4].data()), strings[4].size()}};
      definition._options._maxLogBytes = configuration._maxLogBytes;
      definition._options._limits = configuration._limits;
      definition._options._storage = decode_policy(read_integer<std::uint16_t>(immutableBody, 2),
          std::array{StorageMapping::TwoPlane, StorageMapping::Segmented}, "session.storage");
      require(definition._options._storage == configuration._storage, ErrorCode::IoError,
              "session.storage", "Session definition disagrees with superblock storage");
      const auto segment = read_integer<std::uint64_t>(immutableBody, 32);
      require(segment == configuration._segmentBytes, ErrorCode::IoError,
              "session.segment_bytes", "Session definition disagrees with superblock segment quantum");
      if (definition._options._storage == StorageMapping::Segmented)
      {
        definition._options._segmentBytes = segment;
      }
      else
      {
        require(segment == 0, ErrorCode::IoError, "session.segment_bytes", "Two-plane storage has a segment quantum");
      }
      definition._options._defaults._delivery = decode_policy(read_integer<std::uint16_t>(immutableBody, 4),
          std::array{DeliveryPolicy::RecoverFromLog, DeliveryPolicy::AllowLoss}, "session.defaults.delivery");
      definition._options._defaults._selection = decode_policy(read_integer<std::uint16_t>(immutableBody, 6),
          std::array{SelectionPolicy::Strict, SelectionPolicy::Fair, SelectionPolicy::WeightedRandom}, "session.defaults.selection");
      definition._options._defaults._wait = decode_policy(read_integer<std::uint16_t>(immutableBody, 8),
          std::array{WaitPolicy::Spin, WaitPolicy::EventFd}, "session.defaults.wait");
      definition._options._defaults._acceptance = decode_policy(read_integer<std::uint16_t>(immutableBody, 10),
          std::array{AcceptancePolicy::AllDestinations, AcceptancePolicy::Partial}, "session.defaults.acceptance");
      definition._options._defaults._ringCapacity = read_integer<std::uint64_t>(immutableBody, 24);
      definition._policyFeatures = read_integer<std::uint64_t>(immutableBody, 40);
      check_session(definition, ErrorCode::IoError);
      return definition;
    }
    catch (const MetadataFailure& failure)
    {
      return failure._error;
    }
    catch (const std::filesystem::filesystem_error& error)
    {
      return Error{ErrorCode::IoError, error.what(), "session.original_directory"};
    }
  }

  Result<std::vector<std::byte>> encode_task_definition(const TaskDefinition& definition,
                                                        std::uint64_t recordOffset)
  {
    try
    {
      check_task(definition, ErrorCode::InvalidArgument);
      const std::array<std::string_view, 2> strings{definition._descriptor._name, definition._descriptor._doc};
      auto bytes = encode_strings(72, 32, strings, recordOffset, "task.definition");
      write_integer<std::uint64_t>(bytes, 0, static_cast<std::uint16_t>(definition._id));
      write_integer<std::uint16_t>(bytes, 8, static_cast<std::uint16_t>(definition._registration._createdBy));
      write_integer<std::uint64_t>(bytes, 16, static_cast<std::uint64_t>(definition._registration._createdAt.time_since_epoch().count()));
      write_integer<std::uint64_t>(bytes, 24, definition._registration._revision);
      write_integer<std::uint16_t>(bytes, 64, definition._descriptor._kind == TaskKind::Process ? 1 : 2);
      write_integer<std::uint16_t>(bytes, 66, static_cast<std::uint16_t>(definition._descriptor._process.value_or(TaskId::Invalid)));
      return bytes;
    }
    catch (const MetadataFailure& failure)
    {
      return failure._error;
    }
  }

  Result<TaskDefinition> decode_task_definition(std::span<const std::byte> immutableBody,
                                                 std::uint64_t recordOffset)
  {
    try
    {
      const auto strings = decode_strings<2>(immutableBody, 72, 32, recordOffset, "task.definition");
      require(zero(immutableBody.subspan(10, 6)) && zero(immutableBody.subspan(68, 4)),
              ErrorCode::IoError, "task.definition", "Task state/reserved bytes are nonzero");
      const auto id = read_integer<std::uint64_t>(immutableBody, 0);
      require(id != 0 && id <= max_entity_id, ErrorCode::IoError, "task.id", "Task key exceeds its 16-bit namespace");
      TaskDefinition definition;
      definition._id = static_cast<TaskId>(id);
      definition._descriptor._name = strings[0];
      definition._descriptor._doc = strings[1];
      definition._descriptor._kind = decode_policy(read_integer<std::uint16_t>(immutableBody, 64),
          std::array{TaskKind::Process, TaskKind::Thread}, "task.kind");
      const auto parent = read_integer<std::uint16_t>(immutableBody, 66);
      if (parent != 0)
      {
        definition._descriptor._process = static_cast<TaskId>(parent);
      }
      definition._registration._createdBy = static_cast<TaskId>(read_integer<std::uint16_t>(immutableBody, 8));
      definition._registration._createdAt = UtcTimestamp{std::chrono::nanoseconds{
          std::bit_cast<std::int64_t>(read_integer<std::uint64_t>(immutableBody, 16))}};
      definition._registration._revision = read_integer<std::uint64_t>(immutableBody, 24);
      check_task(definition, ErrorCode::IoError);
      return definition;
    }
    catch (const MetadataFailure& failure)
    {
      return failure._error;
    }
  }
} // namespace angaria::detail::mapped
