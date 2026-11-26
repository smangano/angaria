#include "topology_metadata.h"

#include "angaria/detail/byte_operations.h"
#include "angaria/detail/checked_arithmetic.h"
#include "angaria/detail/error_support.h"
#include "angaria/detail/wire_codes.h"

#include "definition_metadata.h"

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <limits>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

namespace angaria::detail::mapped
{
  namespace
  {
    using definition::check_text;
    using definition::check_registration;
    using definition::body_begin;
    using definition::host_size;
    using definition::encode_text;
    using definition::decode_text;

    namespace wire = angaria::detail::wire;

    using angaria::detail::CodecFailure;
    using angaria::detail::all_zero;
    using angaria::detail::codec_require;
    using angaria::detail::read_unsigned_le;
    using angaria::detail::write_unsigned_le;

    constexpr std::size_t queue_fixed_bytes = 80;
    constexpr std::size_t set_fixed_bytes = 96;

    std::uint8_t state_code(EntityState state, ErrorCode code)
    {
      const auto encoded = wire::encode(state, wire::entity_states);
      codec_require(encoded.has_value(), code, "definition.state", "Unknown entity state");
      return static_cast<std::uint8_t>(*encoded);
    }

    EntityState decode_state(std::uint8_t code)
    {
      const auto decoded = wire::decode(code, wire::entity_states);
      codec_require(decoded.has_value(), ErrorCode::IoError, "definition.state", "Unknown stored entity state");
      return *decoded;
    }

    std::uint16_t delivery_code(DeliveryPolicy policy, ErrorCode code)
    {
      const auto encoded = wire::encode(policy, wire::delivery_policies);
      codec_require(encoded.has_value(), code, "queue.delivery", "Unknown delivery policy");
      return *encoded;
    }

    DeliveryPolicy decode_delivery(std::uint8_t code)
    {
      const auto decoded = wire::decode(code, wire::delivery_policies);
      codec_require(decoded.has_value(), ErrorCode::IoError, "queue.delivery", "Unknown stored delivery policy");
      return *decoded;
    }

    std::uint16_t selection_code(SelectionPolicy policy, ErrorCode code)
    {
      const auto encoded = wire::encode(policy, wire::selection_policies);
      codec_require(encoded.has_value(), code, "source_set.selection", "Unknown selection policy");
      return *encoded;
    }

    SelectionPolicy decode_selection(std::uint16_t code)
    {
      const auto decoded = wire::decode(code, wire::selection_policies);
      codec_require(decoded.has_value(), ErrorCode::IoError, "source_set.selection", "Unknown stored selection policy");
      return *decoded;
    }

    std::uint16_t wait_code(WaitPolicy policy, ErrorCode code)
    {
      const auto encoded = wire::encode(policy, wire::wait_policies);
      codec_require(encoded.has_value(), code, "source_set.wait", "Unknown wait policy");
      return *encoded;
    }

    WaitPolicy decode_wait(std::uint16_t code)
    {
      const auto decoded = wire::decode(code, wire::wait_policies);
      codec_require(decoded.has_value(), ErrorCode::IoError, "source_set.wait", "Unknown stored wait policy");
      return *decoded;
    }

    std::uint16_t acceptance_code(AcceptancePolicy policy, ErrorCode code)
    {
      const auto encoded = wire::encode(policy, wire::acceptance_policies);
      codec_require(encoded.has_value(), code, "publishing_set.acceptance", "Unknown acceptance policy");
      return *encoded;
    }

    AcceptancePolicy decode_acceptance(std::uint16_t code)
    {
      const auto decoded = wire::decode(code, wire::acceptance_policies);
      codec_require(decoded.has_value(), ErrorCode::IoError, "publishing_set.acceptance", "Unknown stored acceptance policy");
      return *decoded;
    }

    void check_queue(const QueueDescriptor& descriptor, ErrorCode code, bool resolved = false)
    {
      check_text(descriptor._name, descriptor._doc, "queue.name", "queue.doc", code);
      if (descriptor._delivery)
      {
        static_cast<void>(delivery_code(*descriptor._delivery, code));
      }
      codec_require(resolved || !descriptor._ringCapacity || *descriptor._ringCapacity != 0,
              code, "queue.ring_capacity", "An explicit Queue ring-capacity override must be positive");
    }

    void check_source(const SourceQueueSetDescriptor& descriptor, ErrorCode code)
    {
      check_text(descriptor._name, descriptor._doc, "source_set.name", "source_set.doc", code);
      if (descriptor._selection)
      {
        static_cast<void>(selection_code(*descriptor._selection, code));
        codec_require(*descriptor._selection == SelectionPolicy::WeightedRandom || !descriptor._randomSeed,
                code, "source_set.random_seed", "A random seed requires weighted-random selection");
      }
      if (descriptor._wait)
      {
        static_cast<void>(wait_code(*descriptor._wait, code));
      }
    }

    void check_queue_ids(std::span<const QueueId> ids, ErrorCode code, const char* path)
    {
      codec_require(ids.size() <= std::numeric_limits<std::uint16_t>::max(), code, path,
              "Queue membership exceeds the identity namespace");
      std::set<QueueId> unique;
      for (const auto id : ids)
      {
        codec_require(id != QueueId::Invalid && unique.insert(id).second, code, path,
                "Queue references must be nonzero and unique");
      }
    }

    void check_publishing(const PublishingQueueSetDescriptor& descriptor, ErrorCode code)
    {
      check_text(descriptor._name, descriptor._doc, "publishing_set.name", "publishing_set.doc", code);
      check_queue_ids(descriptor._destinations, code, "publishing_set.destinations");
      if (descriptor._acceptance)
      {
        static_cast<void>(acceptance_code(*descriptor._acceptance, code));
      }
    }

    void check_queue_definition(const QueueDefinition& definition, ErrorCode code)
    {
      check_queue(definition._descriptor, code, true);
      check_registration(definition._registration, code);
      static_cast<void>(state_code(definition._state, code));
      codec_require(definition._id != QueueId::Invalid, code, "queue.id", "Queue identity must be nonzero");
      codec_require(definition._descriptor._ringCapacity.has_value() && definition._descriptor._delivery.has_value(),
              code, "queue.policy", "Stored Queue policies must be resolved");
      codec_require(definition._state != EntityState::Active ||
                  (definition._descriptor._producer != TaskId::Invalid &&
                   definition._descriptor._sourceSet != SourceQueueSetId::Invalid),
              code, "queue.owner", "An active Queue needs a producer and Source Queue Set");
    }

    void check_source_definition(const SourceDefinition& definition, ErrorCode code)
    {
      check_source(definition._descriptor, code);
      check_registration(definition._registration, code);
      static_cast<void>(state_code(definition._state, code));
      codec_require(definition._id != SourceQueueSetId::Invalid, code, "source_set.id", "Source identity must be nonzero");
      codec_require(definition._descriptor._selection.has_value() && definition._descriptor._wait.has_value(),
              code, "source_set.policy", "Stored Source policies must be resolved");
      codec_require(*definition._descriptor._selection != SelectionPolicy::WeightedRandom ||
                  definition._descriptor._randomSeed.has_value(),
              code, "source_set.random_seed", "Stored weighted-random selection needs its effective seed");
      check_queue_ids(definition._members, code, "source_set.members");
      codec_require(definition._state != EntityState::Active ||
                  (definition._descriptor._owner != TaskId::Invalid && !definition._members.empty()),
              code, "source_set.owner", "An active Source needs an owner and member Queue");
    }

    void check_publishing_definition(const PublishingDefinition& definition, ErrorCode code)
    {
      check_publishing(definition._descriptor, code);
      check_registration(definition._registration, code);
      static_cast<void>(state_code(definition._state, code));
      codec_require(definition._id != PublishingQueueSetId::Invalid, code, "publishing_set.id", "Publishing identity must be nonzero");
      codec_require(definition._descriptor._acceptance.has_value(), code, "publishing_set.acceptance",
              "Stored Publishing acceptance must be resolved");
      codec_require(definition._topologyRevision != 0 && definition._topologyRevision <= definition._registration._revision,
              code, "publishing_set.topology_revision", "Publishing topology revision is outside its definition history");
      codec_require(definition._state != EntityState::Active ||
                  (definition._descriptor._owner != TaskId::Invalid && !definition._descriptor._destinations.empty()),
              code, "publishing_set.owner", "An active Publishing set needs an owner and destination Queue");
    }

    void write_common(std::span<std::byte> bytes, std::uint16_t id,
                        const RegistrationMetadata& registration, EntityState state)
    {
      write_unsigned_le<std::uint64_t>(bytes, 0, id);
      write_unsigned_le<std::uint16_t>(bytes, 8, static_cast<std::uint16_t>(registration._createdBy));
      write_unsigned_le<std::uint8_t>(bytes, 10, state_code(state, ErrorCode::InvalidArgument));
      write_unsigned_le<std::uint64_t>(bytes, 16,
          static_cast<std::uint64_t>(registration._createdAt.time_since_epoch().count()));
      write_unsigned_le<std::uint64_t>(bytes, 24, registration._revision);
    }

    std::uint16_t read_common(std::span<const std::byte> bytes,
                               RegistrationMetadata& registration, EntityState& state)
    {
      const auto key = read_unsigned_le<std::uint64_t>(bytes, 0);
      codec_require(key != 0 && key <= std::numeric_limits<std::uint16_t>::max(),
              ErrorCode::IoError, "definition.id", "Stored entity identity is not a nonzero u16");
      codec_require(all_zero(bytes.subspan(11, 5)), ErrorCode::IoError, "definition.reserved",
              "Reserved definition bytes must be zero");
      registration._createdBy = static_cast<TaskId>(read_unsigned_le<std::uint16_t>(bytes, 8));
      registration._createdAt = UtcTimestamp{std::chrono::nanoseconds{
          std::bit_cast<std::int64_t>(read_unsigned_le<std::uint64_t>(bytes, 16))}};
      registration._revision = read_unsigned_le<std::uint64_t>(bytes, 24);
      check_registration(registration, ErrorCode::IoError);
      state = decode_state(read_unsigned_le<std::uint8_t>(bytes, 10));
      return static_cast<std::uint16_t>(key);
    }

    void write_members(std::span<std::byte> bytes, std::span<const QueueId> ids,
                         std::size_t sliceOffset, std::uint64_t begin)
    {
      if (!ids.empty())
      {
        write_unsigned_le<std::uint64_t>(bytes, sliceOffset, begin + set_fixed_bytes);
        write_unsigned_le<std::uint64_t>(bytes, sliceOffset + 8, ids.size());
        for (std::size_t index = 0; index < ids.size(); ++index)
        {
          write_unsigned_le<std::uint16_t>(bytes, set_fixed_bytes + index * 2, static_cast<std::uint16_t>(ids[index]));
        }
      }
    }

    std::vector<QueueId> read_members(std::span<const std::byte> bytes, std::size_t sliceOffset,
                                      std::uint64_t begin, const char* path)
    {
      const auto reference = read_unsigned_le<std::uint64_t>(bytes, sliceOffset);
      const auto count = read_unsigned_le<std::uint64_t>(bytes, sliceOffset + 8);
      const auto arrayBytes = checked_multiply(count, 2);
      codec_require(count <= std::numeric_limits<std::uint16_t>::max() && arrayBytes &&
                  *arrayBytes <= bytes.size() - set_fixed_bytes,
              ErrorCode::IoError, path, "Queue array exceeds its owning body or identity namespace");
      codec_require(reference == (count == 0 ? 0 : begin + set_fixed_bytes), ErrorCode::IoError,
              path, "Queue array is outside its canonical owning body");
      std::vector<QueueId> ids(host_size(count, ErrorCode::IoError));
      for (std::size_t index = 0; index < ids.size(); ++index)
      {
        ids[index] = static_cast<QueueId>(read_unsigned_le<std::uint16_t>(bytes, set_fixed_bytes + index * 2));
      }
      check_queue_ids(ids, ErrorCode::IoError, path);
      return ids;
    }
  } // namespace

  OperationResult validate_queue_descriptor(const QueueDescriptor& descriptor)
  {
    try
    {
      check_queue(descriptor, ErrorCode::InvalidArgument);
      return std::monostate{};
    }
    catch (const CodecFailure& failure)
    {
      return failure._error;
    }
  }

  OperationResult validate_source_set_descriptor(const SourceQueueSetDescriptor& descriptor)
  {
    try
    {
      check_source(descriptor, ErrorCode::InvalidArgument);
      return std::monostate{};
    }
    catch (const CodecFailure& failure)
    {
      return failure._error;
    }
  }

  OperationResult validate_publishing_set_descriptor(const PublishingQueueSetDescriptor& descriptor)
  {
    try
    {
      check_publishing(descriptor, ErrorCode::InvalidArgument);
      return std::monostate{};
    }
    catch (const CodecFailure& failure)
    {
      return failure._error;
    }
  }

  Result<std::vector<std::byte>> encode_queue_definition(const QueueDefinition& definition,
                                                         std::uint64_t recordOffset)
  {
    try
    {
      check_queue_definition(definition, ErrorCode::InvalidArgument);
      const auto& descriptor = definition._descriptor;
      auto bytes = encode_text(queue_fixed_bytes, descriptor._name, descriptor._doc, recordOffset);
      write_common(bytes, static_cast<std::uint16_t>(definition._id), definition._registration, definition._state);
      write_unsigned_le<std::uint16_t>(bytes, 64, static_cast<std::uint16_t>(descriptor._producer));
      write_unsigned_le<std::uint16_t>(bytes, 66, static_cast<std::uint16_t>(descriptor._sourceSet));
      write_unsigned_le<std::uint8_t>(bytes, 68, descriptor._priority);
      write_unsigned_le<std::uint8_t>(bytes, 69, static_cast<std::uint8_t>(delivery_code(*descriptor._delivery, ErrorCode::InvalidArgument)));
      write_unsigned_le<std::uint64_t>(bytes, 72, *descriptor._ringCapacity);
      return bytes;
    }
    catch (const CodecFailure& failure)
    {
      return failure._error;
    }
  }

  Result<QueueDefinition> decode_queue_definition(std::span<const std::byte> immutableBody,
                                                   std::uint64_t recordOffset)
  {
    try
    {
      codec_require(immutableBody.size() >= queue_fixed_bytes, ErrorCode::IoError, "queue.definition", "Truncated Queue body");
      const auto begin = body_begin(recordOffset, immutableBody.size(), ErrorCode::IoError);
      codec_require(all_zero(immutableBody.subspan(70, 2)), ErrorCode::IoError, "queue.definition", "Reserved Queue bytes are nonzero");
      QueueDefinition definition;
      definition._id = static_cast<QueueId>(read_common(immutableBody, definition._registration, definition._state));
      auto& descriptor = definition._descriptor;
      descriptor._producer = static_cast<TaskId>(read_unsigned_le<std::uint16_t>(immutableBody, 64));
      descriptor._sourceSet = static_cast<SourceQueueSetId>(read_unsigned_le<std::uint16_t>(immutableBody, 66));
      descriptor._priority = read_unsigned_le<std::uint8_t>(immutableBody, 68);
      descriptor._delivery = decode_delivery(read_unsigned_le<std::uint8_t>(immutableBody, 69));
      descriptor._ringCapacity = read_unsigned_le<std::uint64_t>(immutableBody, 72);
      auto strings = decode_text(immutableBody, queue_fixed_bytes, begin, "queue.name", "queue.doc");
      descriptor._name = std::move(strings[0]);
      descriptor._doc = std::move(strings[1]);
      check_queue_definition(definition, ErrorCode::IoError);
      return definition;
    }
    catch (const CodecFailure& failure)
    {
      return failure._error;
    }
  }

  Result<std::vector<std::byte>> encode_source_definition(const SourceDefinition& definition,
                                                          std::uint64_t recordOffset)
  {
    try
    {
      check_source_definition(definition, ErrorCode::InvalidArgument);
      const auto& descriptor = definition._descriptor;
      const auto arrayBytes = checked_multiply(definition._members.size(), 2);
      const auto prefix = arrayBytes ? checked_add(set_fixed_bytes, *arrayBytes) : std::nullopt;
      codec_require(prefix.has_value(), ErrorCode::InvalidArgument, "source_set.members", "Queue array length overflows");
      auto bytes = encode_text(*prefix, descriptor._name, descriptor._doc, recordOffset);
      write_common(bytes, static_cast<std::uint16_t>(definition._id), definition._registration, definition._state);
      write_unsigned_le<std::uint16_t>(bytes, 64, static_cast<std::uint16_t>(descriptor._owner));
      write_unsigned_le<std::uint16_t>(bytes, 66, selection_code(*descriptor._selection, ErrorCode::InvalidArgument));
      write_unsigned_le<std::uint16_t>(bytes, 68, wait_code(*descriptor._wait, ErrorCode::InvalidArgument));
      write_unsigned_le<std::uint64_t>(bytes, 72, descriptor._randomSeed.value_or(0));
      write_members(bytes, definition._members, 80, recordOffset + 64);
      return bytes;
    }
    catch (const CodecFailure& failure)
    {
      return failure._error;
    }
  }

  Result<SourceDefinition> decode_source_definition(std::span<const std::byte> immutableBody,
                                                     std::uint64_t recordOffset)
  {
    try
    {
      codec_require(immutableBody.size() >= set_fixed_bytes, ErrorCode::IoError, "source_set.definition", "Truncated Source body");
      const auto begin = body_begin(recordOffset, immutableBody.size(), ErrorCode::IoError);
      codec_require(all_zero(immutableBody.subspan(70, 2)), ErrorCode::IoError, "source_set.definition", "Reserved Source bytes are nonzero");
      SourceDefinition definition;
      definition._id = static_cast<SourceQueueSetId>(read_common(immutableBody, definition._registration, definition._state));
      auto& descriptor = definition._descriptor;
      descriptor._owner = static_cast<TaskId>(read_unsigned_le<std::uint16_t>(immutableBody, 64));
      descriptor._selection = decode_selection(read_unsigned_le<std::uint16_t>(immutableBody, 66));
      descriptor._wait = decode_wait(read_unsigned_le<std::uint16_t>(immutableBody, 68));
      const auto seed = read_unsigned_le<std::uint64_t>(immutableBody, 72);
      if (*descriptor._selection == SelectionPolicy::WeightedRandom)
      {
        descriptor._randomSeed = seed;
      }
      else
      {
        codec_require(seed == 0, ErrorCode::IoError, "source_set.random_seed", "Nonrandom Source seed must be zero");
      }
      definition._members = read_members(immutableBody, 80, begin, "source_set.members");
      const auto cursor = set_fixed_bytes + definition._members.size() * 2;
      auto strings = decode_text(immutableBody, cursor, begin, "source_set.name", "source_set.doc");
      descriptor._name = std::move(strings[0]);
      descriptor._doc = std::move(strings[1]);
      check_source_definition(definition, ErrorCode::IoError);
      return definition;
    }
    catch (const CodecFailure& failure)
    {
      return failure._error;
    }
  }

  Result<std::vector<std::byte>> encode_publishing_definition(const PublishingDefinition& definition,
                                                              std::uint64_t recordOffset)
  {
    try
    {
      check_publishing_definition(definition, ErrorCode::InvalidArgument);
      const auto& descriptor = definition._descriptor;
      const auto arrayBytes = checked_multiply(descriptor._destinations.size(), 2);
      const auto prefix = arrayBytes ? checked_add(set_fixed_bytes, *arrayBytes) : std::nullopt;
      codec_require(prefix.has_value(), ErrorCode::InvalidArgument, "publishing_set.destinations", "Queue array length overflows");
      auto bytes = encode_text(*prefix, descriptor._name, descriptor._doc, recordOffset);
      write_common(bytes, static_cast<std::uint16_t>(definition._id), definition._registration, definition._state);
      write_unsigned_le<std::uint16_t>(bytes, 64, static_cast<std::uint16_t>(descriptor._owner));
      write_unsigned_le<std::uint16_t>(bytes, 66, acceptance_code(*descriptor._acceptance, ErrorCode::InvalidArgument));
      write_members(bytes, descriptor._destinations, 72, recordOffset + 64);
      write_unsigned_le<std::uint64_t>(bytes, 88, definition._topologyRevision);
      return bytes;
    }
    catch (const CodecFailure& failure)
    {
      return failure._error;
    }
  }

  Result<PublishingDefinition> decode_publishing_definition(std::span<const std::byte> immutableBody,
                                                             std::uint64_t recordOffset)
  {
    try
    {
      codec_require(immutableBody.size() >= set_fixed_bytes, ErrorCode::IoError, "publishing_set.definition", "Truncated Publishing body");
      const auto begin = body_begin(recordOffset, immutableBody.size(), ErrorCode::IoError);
      codec_require(all_zero(immutableBody.subspan(68, 4)), ErrorCode::IoError, "publishing_set.definition", "Reserved Publishing bytes are nonzero");
      PublishingDefinition definition;
      definition._id = static_cast<PublishingQueueSetId>(read_common(immutableBody, definition._registration, definition._state));
      auto& descriptor = definition._descriptor;
      descriptor._owner = static_cast<TaskId>(read_unsigned_le<std::uint16_t>(immutableBody, 64));
      descriptor._acceptance = decode_acceptance(read_unsigned_le<std::uint16_t>(immutableBody, 66));
      descriptor._destinations = read_members(immutableBody, 72, begin, "publishing_set.destinations");
      definition._topologyRevision = read_unsigned_le<std::uint64_t>(immutableBody, 88);
      const auto cursor = set_fixed_bytes + descriptor._destinations.size() * 2;
      auto strings = decode_text(immutableBody, cursor, begin, "publishing_set.name", "publishing_set.doc");
      descriptor._name = std::move(strings[0]);
      descriptor._doc = std::move(strings[1]);
      check_publishing_definition(definition, ErrorCode::IoError);
      return definition;
    }
    catch (const CodecFailure& failure)
    {
      return failure._error;
    }
  }
} // namespace angaria::detail::mapped
