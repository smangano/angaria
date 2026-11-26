// Portable topology metadata examples, independent of a live Registry. The
// fixtures below are authored from docs/mapped-format.md. Recognizing policy
// codes or active definitions does not implement their native execution paths.

#include "wire_fixture.h"
#include "result_assertions.h"
#include "topology_metadata.h"

#include "angaria/detail/mapped_format.h"

#include <gtest/gtest.h>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace
{
  using namespace angaria;
  namespace mapped = angaria::detail::mapped;
  constexpr std::uint64_t record_offset = 8192;

  using angaria::test_support::require_value;

  using angaria::test_support::expect_error;

  using angaria::test_support::put_integer;

  using angaria::test_support::get_integer;

  using angaria::test_support::append_text;

  RegistrationMetadata registration()
  {
    return RegistrationMetadata{TaskId{7}, UtcTimestamp{std::chrono::nanoseconds{-9}}, 11};
  }

  mapped::QueueDefinition queue_definition()
  {
    mapped::QueueDefinition definition;
    definition._id = QueueId{65535};
    definition._descriptor._name = "Feed";
    definition._descriptor._producer = TaskId{3};
    definition._descriptor._sourceSet = SourceQueueSetId{12};
    definition._descriptor._priority = 255;
    definition._descriptor._delivery = DeliveryPolicy::AllowLoss;
    definition._descriptor._ringCapacity = 1024;
    definition._descriptor._doc = "q";
    definition._registration = registration();
    return definition;
  }

  mapped::SourceDefinition source_definition()
  {
    mapped::SourceDefinition definition;
    definition._id = SourceQueueSetId{12};
    definition._descriptor._name = "Inputs";
    definition._descriptor._owner = TaskId{9};
    definition._descriptor._selection = SelectionPolicy::WeightedRandom;
    definition._descriptor._wait = WaitPolicy::EventFd;
    definition._descriptor._randomSeed = 0x0102030405060708ull;
    definition._descriptor._doc = "src";
    definition._members = {QueueId{65535}, QueueId{2}};
    definition._registration = registration();
    return definition;
  }

  mapped::PublishingDefinition publishing_definition()
  {
    mapped::PublishingDefinition definition;
    definition._id = PublishingQueueSetId{13};
    definition._descriptor._name = "Outputs";
    definition._descriptor._owner = TaskId{3};
    definition._descriptor._acceptance = AcceptancePolicy::Partial;
    definition._descriptor._destinations = {QueueId{2}, QueueId{65535}};
    definition._descriptor._doc = "pub";
    definition._registration = registration();
    definition._topologyRevision = 5;
    return definition;
  }

  // These three fixtures spell out the common 64-byte prefix independently.
  // Their logical addresses are file offsets; an unaligned host buffer remains
  // valid because the codec never dereferences native integer structures.
  std::vector<std::byte> golden_queue()
  {
    std::vector<std::byte> bytes(80);
    put_integer(bytes, 0, 65535, 8);
    put_integer(bytes, 8, 7, 2);
    put_integer(bytes, 10, 1, 1);
    put_integer(bytes, 16, 0xfffffffffffffff7ull, 8);
    put_integer(bytes, 24, 11, 8);
    put_integer(bytes, 32, 8336, 8);
    put_integer(bytes, 40, 4, 8);
    put_integer(bytes, 48, 8340, 8);
    put_integer(bytes, 56, 1, 8);
    put_integer(bytes, 64, 3, 2);
    put_integer(bytes, 66, 12, 2);
    put_integer(bytes, 68, 255, 1);
    put_integer(bytes, 69, 2, 1);
    put_integer(bytes, 72, 1024, 8);
    append_text(bytes, "Feedq");
    return bytes;
  }

  std::vector<std::byte> golden_source()
  {
    std::vector<std::byte> bytes(100);
    put_integer(bytes, 0, 12, 8);
    put_integer(bytes, 8, 7, 2);
    put_integer(bytes, 10, 1, 1);
    put_integer(bytes, 16, 0xfffffffffffffff7ull, 8);
    put_integer(bytes, 24, 11, 8);
    put_integer(bytes, 32, 8356, 8);
    put_integer(bytes, 40, 6, 8);
    put_integer(bytes, 48, 8362, 8);
    put_integer(bytes, 56, 3, 8);
    put_integer(bytes, 64, 9, 2);
    put_integer(bytes, 66, 3, 2);
    put_integer(bytes, 68, 2, 2);
    put_integer(bytes, 72, 0x0102030405060708ull, 8);
    put_integer(bytes, 80, 8352, 8);
    put_integer(bytes, 88, 2, 8);
    put_integer(bytes, 96, 65535, 2);
    put_integer(bytes, 98, 2, 2);
    append_text(bytes, "Inputssrc");
    return bytes;
  }

  std::vector<std::byte> golden_publishing()
  {
    std::vector<std::byte> bytes(100);
    put_integer(bytes, 0, 13, 8);
    put_integer(bytes, 8, 7, 2);
    put_integer(bytes, 10, 1, 1);
    put_integer(bytes, 16, 0xfffffffffffffff7ull, 8);
    put_integer(bytes, 24, 11, 8);
    put_integer(bytes, 32, 8356, 8);
    put_integer(bytes, 40, 7, 8);
    put_integer(bytes, 48, 8363, 8);
    put_integer(bytes, 56, 3, 8);
    put_integer(bytes, 64, 3, 2);
    put_integer(bytes, 66, 2, 2);
    put_integer(bytes, 72, 8352, 8);
    put_integer(bytes, 80, 2, 8);
    put_integer(bytes, 88, 5, 8);
    put_integer(bytes, 96, 2, 2);
    put_integer(bytes, 98, 65535, 2);
    append_text(bytes, "Outputspub");
    return bytes;
  }

  TEST(TopologyMetadata, QueueMatchesIndependentGoldenBodyAndUnalignedDecode)
  {
    EXPECT_EQ(require_value(mapped::encode_queue_definition(queue_definition(), record_offset)), golden_queue());
    auto body = golden_queue();
    body.insert(body.begin(), std::byte{0xff});
    const auto decoded = require_value(mapped::decode_queue_definition(std::span{body}.subspan(1), record_offset));
    EXPECT_EQ(decoded._id, QueueId{65535});
    EXPECT_EQ(decoded._descriptor._name, "Feed");
    EXPECT_EQ(decoded._descriptor._doc, "q");
    EXPECT_EQ(decoded._descriptor._producer, TaskId{3});
    EXPECT_EQ(decoded._descriptor._sourceSet, SourceQueueSetId{12});
    EXPECT_EQ(decoded._descriptor._priority, 255);
    EXPECT_EQ(decoded._descriptor._delivery, DeliveryPolicy::AllowLoss);
    EXPECT_EQ(decoded._descriptor._ringCapacity, 1024u);
    EXPECT_EQ(decoded._registration._createdBy, TaskId{7});
    EXPECT_EQ(decoded._registration._createdAt.time_since_epoch().count(), -9);
    EXPECT_EQ(decoded._registration._revision, 11u);
    EXPECT_EQ(decoded._state, EntityState::Draft);
  }

  TEST(TopologyMetadata, SourceMatchesIndependentGoldenBodyWithOrderedMembershipAndSeed)
  {
    EXPECT_EQ(require_value(mapped::encode_source_definition(source_definition(), record_offset)), golden_source());
    auto body = golden_source();
    body.insert(body.begin(), std::byte{0xff});
    const auto decoded = require_value(mapped::decode_source_definition(std::span{body}.subspan(1), record_offset));
    EXPECT_EQ(decoded._id, SourceQueueSetId{12});
    EXPECT_EQ(decoded._descriptor._name, "Inputs");
    EXPECT_EQ(decoded._descriptor._doc, "src");
    EXPECT_EQ(decoded._descriptor._owner, TaskId{9});
    EXPECT_EQ(decoded._descriptor._selection, SelectionPolicy::WeightedRandom);
    EXPECT_EQ(decoded._descriptor._wait, WaitPolicy::EventFd);
    EXPECT_EQ(decoded._descriptor._randomSeed, 0x0102030405060708ull);
    EXPECT_EQ(decoded._members, source_definition()._members);
    EXPECT_EQ(decoded._registration._createdAt.time_since_epoch().count(), -9);
    EXPECT_EQ(decoded._registration._revision, 11u);
  }

  TEST(TopologyMetadata, PublishingMatchesIndependentGoldenBodyAndTopologyRevision)
  {
    EXPECT_EQ(require_value(mapped::encode_publishing_definition(publishing_definition(), record_offset)), golden_publishing());
    auto body = golden_publishing();
    body.insert(body.begin(), std::byte{0xff});
    const auto decoded = require_value(mapped::decode_publishing_definition(std::span{body}.subspan(1), record_offset));
    EXPECT_EQ(decoded._id, PublishingQueueSetId{13});
    EXPECT_EQ(decoded._descriptor._name, "Outputs");
    EXPECT_EQ(decoded._descriptor._doc, "pub");
    EXPECT_EQ(decoded._descriptor._owner, TaskId{3});
    EXPECT_EQ(decoded._descriptor._acceptance, AcceptancePolicy::Partial);
    EXPECT_EQ(decoded._descriptor._destinations, publishing_definition()._descriptor._destinations);
    EXPECT_EQ(decoded._topologyRevision, 5u);
    EXPECT_EQ(decoded._registration._createdBy, TaskId{7});
  }

  // The body retains resolved policy choices, including future native policies.
  // A zero ring denotes segmented storage here; selecting a supported storage
  // implementation and validating priorities remain runtime responsibilities.
  TEST(TopologyMetadata, PortablePoliciesRetainExplicitCodesAndWideResolvedValues)
  {
    auto queue = queue_definition();
    for (const auto delivery : {DeliveryPolicy::RecoverFromLog, DeliveryPolicy::AllowLoss})
    {
      queue._descriptor._delivery = delivery;
      for (const auto capacity : {std::uint64_t{0}, std::numeric_limits<std::uint64_t>::max()})
      {
        queue._descriptor._ringCapacity = capacity;
        const auto body = require_value(mapped::encode_queue_definition(queue, record_offset));
        EXPECT_EQ(get_integer(body, 69, 1), delivery == DeliveryPolicy::RecoverFromLog ? 1u : 2u);
        const auto decoded = require_value(mapped::decode_queue_definition(body, record_offset));
        EXPECT_EQ(decoded._descriptor._ringCapacity, capacity);
        EXPECT_EQ(decoded._descriptor._delivery, delivery);
      }
    }
    auto source = source_definition();
    const std::array selections{SelectionPolicy::Strict, SelectionPolicy::Fair, SelectionPolicy::WeightedRandom};
    for (std::size_t index = 0; index < selections.size(); ++index)
    {
      source._descriptor._selection = selections[index];
      source._descriptor._randomSeed = selections[index] == SelectionPolicy::WeightedRandom ?
          std::optional<std::uint64_t>{0} : std::nullopt;
      for (const auto wait : {WaitPolicy::Spin, WaitPolicy::EventFd})
      {
        source._descriptor._wait = wait;
        const auto body = require_value(mapped::encode_source_definition(source, record_offset));
        EXPECT_EQ(get_integer(body, 66, 2), index + 1);
        EXPECT_EQ(get_integer(body, 68, 2), wait == WaitPolicy::Spin ? 1u : 2u);
        const auto decoded = require_value(mapped::decode_source_definition(body, record_offset));
        EXPECT_EQ(decoded._descriptor._selection, selections[index]);
        EXPECT_EQ(decoded._descriptor._randomSeed, source._descriptor._randomSeed);
      }
    }
    auto publishing = publishing_definition();
    for (const auto acceptance : {AcceptancePolicy::AllDestinations, AcceptancePolicy::Partial})
    {
      publishing._descriptor._acceptance = acceptance;
      const auto body = require_value(mapped::encode_publishing_definition(publishing, record_offset));
      EXPECT_EQ(get_integer(body, 66, 2), acceptance == AcceptancePolicy::AllDestinations ? 1u : 2u);
      EXPECT_EQ(require_value(mapped::decode_publishing_definition(body, record_offset))._descriptor._acceptance,
                acceptance);
    }
  }

  // Incomplete ownership is meaningful for drafts. Activation is a distinct
  // transition and may not persist an ownerless entity or empty active set.
  TEST(TopologyMetadata, DraftOwnershipCanBeIncompleteWhileActiveDefinitionsMustBeComplete)
  {
    auto queue = queue_definition();
    queue._descriptor._producer = TaskId::Invalid;
    queue._descriptor._sourceSet = SourceQueueSetId::Invalid;
    auto body = require_value(mapped::encode_queue_definition(queue, record_offset));
    EXPECT_EQ(require_value(mapped::decode_queue_definition(body, record_offset))._descriptor._producer, TaskId::Invalid);
    queue._state = EntityState::Active;
    expect_error(mapped::encode_queue_definition(queue, record_offset), ErrorCode::InvalidArgument);
    put_integer(body, 10, 2, 1);
    expect_error(mapped::decode_queue_definition(body, record_offset), ErrorCode::IoError);

    auto source = source_definition();
    source._descriptor._owner = TaskId::Invalid;
    source._members.clear();
    body = require_value(mapped::encode_source_definition(source, record_offset));
    EXPECT_TRUE(require_value(mapped::decode_source_definition(body, record_offset))._members.empty());
    source._state = EntityState::Active;
    expect_error(mapped::encode_source_definition(source, record_offset), ErrorCode::InvalidArgument);
    put_integer(body, 10, 2, 1);
    expect_error(mapped::decode_source_definition(body, record_offset), ErrorCode::IoError);

    auto publishing = publishing_definition();
    publishing._descriptor._owner = TaskId::Invalid;
    publishing._descriptor._destinations.clear();
    body = require_value(mapped::encode_publishing_definition(publishing, record_offset));
    EXPECT_TRUE(require_value(mapped::decode_publishing_definition(body, record_offset))._descriptor._destinations.empty());
    publishing._state = EntityState::Active;
    expect_error(mapped::encode_publishing_definition(publishing, record_offset), ErrorCode::InvalidArgument);
    put_integer(body, 10, 2, 1);
    expect_error(mapped::decode_publishing_definition(body, record_offset), ErrorCode::IoError);
  }

  // Shape validation accepts omitted overrides for runtime inheritance. Immutable
  // records, however, must carry the resulting effective choices explicitly.
  TEST(TopologyMetadata, InputDefaultsAndStoredResolvedPoliciesHaveDifferentBoundaries)
  {
    auto queue = queue_definition();
    queue._descriptor._delivery.reset();
    queue._descriptor._ringCapacity.reset();
    EXPECT_TRUE(std::holds_alternative<std::monostate>(mapped::validate_queue_descriptor(queue._descriptor)));
    expect_error(mapped::encode_queue_definition(queue, record_offset), ErrorCode::InvalidArgument);
    queue._descriptor._ringCapacity = 0;
    expect_error(mapped::validate_queue_descriptor(queue._descriptor), ErrorCode::InvalidArgument);

    auto source = source_definition();
    source._descriptor._selection.reset();
    source._descriptor._wait.reset();
    EXPECT_TRUE(std::holds_alternative<std::monostate>(mapped::validate_source_set_descriptor(source._descriptor)));
    expect_error(mapped::encode_source_definition(source, record_offset), ErrorCode::InvalidArgument);
    source._descriptor._selection = SelectionPolicy::WeightedRandom;
    source._descriptor._wait = WaitPolicy::Spin;
    source._descriptor._randomSeed.reset();
    expect_error(mapped::encode_source_definition(source, record_offset), ErrorCode::InvalidArgument);

    auto publishing = publishing_definition();
    publishing._descriptor._acceptance.reset();
    EXPECT_TRUE(std::holds_alternative<std::monostate>(mapped::validate_publishing_set_descriptor(publishing._descriptor)));
    expect_error(mapped::encode_publishing_definition(publishing, record_offset), ErrorCode::InvalidArgument);
  }

  TEST(TopologyMetadata, CompleteActiveDefinitionsUseStableStateCode)
  {
    auto queue = queue_definition();
    queue._state = EntityState::Active;
    const auto queueBody = require_value(mapped::encode_queue_definition(queue, record_offset));
    EXPECT_EQ(get_integer(queueBody, 10, 1), 2u);
    EXPECT_EQ(require_value(mapped::decode_queue_definition(queueBody, record_offset))._state, EntityState::Active);
    auto source = source_definition();
    source._state = EntityState::Active;
    const auto sourceBody = require_value(mapped::encode_source_definition(source, record_offset));
    EXPECT_EQ(require_value(mapped::decode_source_definition(sourceBody, record_offset))._state, EntityState::Active);
    auto publishing = publishing_definition();
    publishing._state = EntityState::Active;
    const auto publishingBody = require_value(mapped::encode_publishing_definition(publishing, record_offset));
    EXPECT_EQ(require_value(mapped::decode_publishing_definition(publishingBody, record_offset))._state, EntityState::Active);
  }

  // Membership is a set of unique Queue identities with retained iteration
  // order. Unlike a schema Field list, repeated members are never meaningful.
  TEST(TopologyMetadata, MembershipRejectsZeroAndRepeatedQueueReferences)
  {
    for (const auto& ids : {std::vector<QueueId>{QueueId{2}, QueueId{2}},
                           std::vector<QueueId>{QueueId::Invalid}})
    {
      auto source = source_definition();
      source._members = ids;
      expect_error(mapped::encode_source_definition(source, record_offset), ErrorCode::InvalidArgument);
      auto publishing = publishing_definition();
      publishing._descriptor._destinations = ids;
      expect_error(mapped::validate_publishing_set_descriptor(publishing._descriptor), ErrorCode::InvalidArgument);
      expect_error(mapped::encode_publishing_definition(publishing, record_offset), ErrorCode::InvalidArgument);
    }
    for (const auto value : {0u, 65535u})
    {
      auto body = golden_source();
      put_integer(body, 98, value, 2);
      expect_error(mapped::decode_source_definition(body, record_offset), ErrorCode::IoError);
      body = golden_publishing();
      put_integer(body, 98, value == 0 ? 0 : 2, 2);
      expect_error(mapped::decode_publishing_definition(body, record_offset), ErrorCode::IoError);
    }
  }

  // The namespace bound, rather than an accidental byte-sized count, determines
  // the portable maximum. Runtime capacities can impose a smaller session bound.
  TEST(TopologyMetadata, MembershipCanUseEveryNonzeroQueueIdentity)
  {
    auto source = source_definition();
    source._members.clear();
    for (std::uint32_t id = 1; id <= 65535; ++id)
    {
      source._members.push_back(static_cast<QueueId>(id));
    }
    const auto body = require_value(mapped::encode_source_definition(source, record_offset));
    EXPECT_EQ(get_integer(body, 88, 8), 65535u);
    EXPECT_EQ(require_value(mapped::decode_source_definition(body, record_offset))._members, source._members);
  }

  // Empty optional slices encode both offset and count as zero. Allocation may
  // add fewer than 64 zero bytes, but cannot smuggle in a second hidden body.
  TEST(TopologyMetadata, EmptySlicesAndShortZeroPaddingAreCanonical)
  {
    auto source = source_definition();
    source._members.clear();
    source._descriptor._doc.clear();
    auto body = require_value(mapped::encode_source_definition(source, record_offset));
    EXPECT_EQ(get_integer(body, 80, 8), 0u);
    EXPECT_EQ(get_integer(body, 88, 8), 0u);
    EXPECT_EQ(get_integer(body, 48, 8), 0u);
    EXPECT_EQ(get_integer(body, 56, 8), 0u);
    body.resize(body.size() + 63);
    EXPECT_TRUE(require_value(mapped::decode_source_definition(body, record_offset))._members.empty());
    body.push_back(std::byte{});
    expect_error(mapped::decode_source_definition(body, record_offset), ErrorCode::IoError);

    auto publishing = publishing_definition();
    publishing._descriptor._destinations.clear();
    publishing._descriptor._doc.clear();
    body = require_value(mapped::encode_publishing_definition(publishing, record_offset));
    EXPECT_EQ(get_integer(body, 72, 8), 0u);
    EXPECT_EQ(get_integer(body, 80, 8), 0u);
    put_integer(body, 72, 8352, 8);
    expect_error(mapped::decode_publishing_definition(body, record_offset), ErrorCode::IoError);
  }

  TEST(TopologyMetadata, UnknownPoliciesAndIrrelevantRandomSeedsAreRejected)
  {
    auto queue = queue_definition();
    queue._descriptor._delivery = static_cast<DeliveryPolicy>(99);
    expect_error(mapped::validate_queue_descriptor(queue._descriptor), ErrorCode::InvalidArgument);
    auto source = source_definition();
    source._descriptor._selection = static_cast<SelectionPolicy>(99);
    expect_error(mapped::validate_source_set_descriptor(source._descriptor), ErrorCode::InvalidArgument);
    source._descriptor._selection = SelectionPolicy::Strict;
    expect_error(mapped::validate_source_set_descriptor(source._descriptor), ErrorCode::InvalidArgument);
    source._descriptor._randomSeed.reset();
    source._descriptor._wait = static_cast<WaitPolicy>(99);
    expect_error(mapped::validate_source_set_descriptor(source._descriptor), ErrorCode::InvalidArgument);
    auto publishing = publishing_definition();
    publishing._descriptor._acceptance = static_cast<AcceptancePolicy>(99);
    expect_error(mapped::validate_publishing_set_descriptor(publishing._descriptor), ErrorCode::InvalidArgument);

    auto body = golden_queue();
    put_integer(body, 69, 3, 1);
    expect_error(mapped::decode_queue_definition(body, record_offset), ErrorCode::IoError);
    for (const auto offset : {66u, 68u})
    {
      body = golden_source();
      put_integer(body, offset, 99, 2);
      expect_error(mapped::decode_source_definition(body, record_offset), ErrorCode::IoError);
    }
    body = golden_source();
    put_integer(body, 66, 1, 2);
    expect_error(mapped::decode_source_definition(body, record_offset), ErrorCode::IoError);
    body = golden_publishing();
    put_integer(body, 66, 3, 2);
    expect_error(mapped::decode_publishing_definition(body, record_offset), ErrorCode::IoError);
  }

  // Corruption diagnostics differ from caller-input errors. Reserved fields,
  // full-width keys and attribution are validated before exposing a descriptor.
  TEST(TopologyMetadata, CommonIdentityAttributionStateAndReservedBytesAreValidated)
  {
    const std::array<std::pair<std::size_t, std::uint64_t>, 8> corruptions{
        std::pair{0u, 0ull}, {0u, 65536ull}, {8u, 0ull}, {10u, 0ull},
        {10u, 3ull}, {11u, 1ull}, {15u, 1ull}, {24u, 0ull}};
    for (const auto& [offset, value] : corruptions)
    {
      const auto width = offset == 0 || offset == 24 ? 8u : offset == 8 ? 2u : 1u;
      auto body = golden_queue();
      put_integer(body, offset, value, width);
      expect_error(mapped::decode_queue_definition(body, record_offset), ErrorCode::IoError);
      body = golden_source();
      put_integer(body, offset, value, width);
      expect_error(mapped::decode_source_definition(body, record_offset), ErrorCode::IoError);
      body = golden_publishing();
      put_integer(body, offset, value, width);
      expect_error(mapped::decode_publishing_definition(body, record_offset), ErrorCode::IoError);
    }
    auto queue = queue_definition();
    queue._id = QueueId::Invalid;
    expect_error(mapped::encode_queue_definition(queue, record_offset), ErrorCode::InvalidArgument);
    auto source = source_definition();
    source._registration._createdBy = TaskId::Invalid;
    expect_error(mapped::encode_source_definition(source, record_offset), ErrorCode::InvalidArgument);
    auto publishing = publishing_definition();
    publishing._state = static_cast<EntityState>(99);
    expect_error(mapped::encode_publishing_definition(publishing, record_offset), ErrorCode::InvalidArgument);
  }

  TEST(TopologyMetadata, KindSpecificReservedBytesAndTopologyBoundsAreValidated)
  {
    for (const auto offset : {70u, 71u})
    {
      auto body = golden_queue();
      put_integer(body, offset, 1, 1);
      expect_error(mapped::decode_queue_definition(body, record_offset), ErrorCode::IoError);
      body = golden_source();
      put_integer(body, offset, 1, 1);
      expect_error(mapped::decode_source_definition(body, record_offset), ErrorCode::IoError);
    }
    for (const auto offset : {68u, 69u, 70u, 71u})
    {
      auto body = golden_publishing();
      put_integer(body, offset, 1, 1);
      expect_error(mapped::decode_publishing_definition(body, record_offset), ErrorCode::IoError);
    }
    for (const auto revision : {0ull, 12ull})
    {
      auto definition = publishing_definition();
      definition._topologyRevision = revision;
      expect_error(mapped::encode_publishing_definition(definition, record_offset), ErrorCode::InvalidArgument);
      auto body = golden_publishing();
      put_integer(body, 88, revision, 8);
      expect_error(mapped::decode_publishing_definition(body, record_offset), ErrorCode::IoError);
    }
  }

  // Corrupt arrays must fail before count-to-byte arithmetic, allocation, or
  // reads can escape their owning immutable body. Empty offsets are canonical.
  TEST(TopologyMetadata, MembershipSlicesRejectOverflowForeignOffsetsAndTruncation)
  {
    const std::array<std::pair<std::uint64_t, std::uint64_t>, 5> corruptions{
        std::pair{8352ull, std::numeric_limits<std::uint64_t>::max()},
        {8352ull, 65536ull}, {8350ull, 2ull}, {0ull, 2ull}, {8352ull, 0ull}};
    for (const auto& [reference, count] : corruptions)
    {
      auto body = golden_source();
      put_integer(body, 80, reference, 8);
      put_integer(body, 88, count, 8);
      expect_error(mapped::decode_source_definition(body, record_offset), ErrorCode::IoError);
      body = golden_publishing();
      put_integer(body, 72, reference, 8);
      put_integer(body, 80, count, 8);
      expect_error(mapped::decode_publishing_definition(body, record_offset), ErrorCode::IoError);
    }
    for (std::size_t length = 0; length < 100; ++length)
    {
      const auto source = golden_source();
      expect_error(mapped::decode_source_definition(std::span{source}.first(length), record_offset), ErrorCode::IoError);
      const auto publishing = golden_publishing();
      expect_error(mapped::decode_publishing_definition(std::span{publishing}.first(length), record_offset), ErrorCode::IoError);
    }
    const auto queue = golden_queue();
    for (std::size_t length = 0; length < queue.size(); ++length)
    {
      expect_error(mapped::decode_queue_definition(std::span{queue}.first(length), record_offset), ErrorCode::IoError);
    }
  }

  TEST(TopologyMetadata, StringSlicesRemainCanonicalAndWithinTheOwningBody)
  {
    for (const auto [offset, value] : std::array<std::pair<std::size_t, std::uint64_t>, 5>{
             std::pair{32u, 1ull}, {40u, 0ull}, {40u, std::numeric_limits<std::uint64_t>::max()},
             {48u, 8336ull}, {56u, 0ull}})
    {
      auto body = golden_queue();
      put_integer(body, offset, value, 8);
      expect_error(mapped::decode_queue_definition(body, record_offset), ErrorCode::IoError);
    }
    auto body = golden_source();
    body.back() = std::byte{0xff};
    expect_error(mapped::decode_source_definition(body, record_offset), ErrorCode::IoError);
    body = golden_publishing();
    body.push_back(std::byte{1});
    expect_error(mapped::decode_publishing_definition(body, record_offset), ErrorCode::IoError);
  }

  // Names are nonempty UTF-8 without NUL. Documentation is length-delimited
  // UTF-8 and may contain NUL; multibyte names are measured in encoded bytes.
  TEST(TopologyMetadata, Utf8AndLengthDelimitedDocumentationArePreserved)
  {
    auto queue = queue_definition();
    queue._descriptor._name = "Prix €";
    queue._descriptor._doc = std::string{"a\0b", 3};
    auto body = require_value(mapped::encode_queue_definition(queue, record_offset));
    const auto decoded = require_value(mapped::decode_queue_definition(body, record_offset));
    EXPECT_EQ(decoded._descriptor._name, queue._descriptor._name);
    EXPECT_EQ(decoded._descriptor._doc, queue._descriptor._doc);
    for (const auto& name : {std::string{}, std::string{"x\0y", 3}, std::string{"\xc0\xaf", 2}})
    {
      queue._descriptor._name = name;
      expect_error(mapped::validate_queue_descriptor(queue._descriptor), ErrorCode::InvalidArgument);
      auto source = source_definition();
      source._descriptor._name = name;
      expect_error(mapped::validate_source_set_descriptor(source._descriptor), ErrorCode::InvalidArgument);
      auto publishing = publishing_definition();
      publishing._descriptor._name = name;
      expect_error(mapped::validate_publishing_set_descriptor(publishing._descriptor), ErrorCode::InvalidArgument);
    }
    body = golden_queue();
    body[80] = std::byte{};
    expect_error(mapped::decode_queue_definition(body, record_offset), ErrorCode::IoError);
  }

  TEST(TopologyMetadata, RecordOffsetsRejectInvalidAlignmentAndFormatOverflow)
  {
    for (const auto offset : {std::uint64_t{0}, std::uint64_t{8193}, mapped::max_log_bytes - 7,
                              std::numeric_limits<std::uint64_t>::max()})
    {
      expect_error(mapped::encode_queue_definition(queue_definition(), offset), ErrorCode::InvalidArgument);
      expect_error(mapped::decode_queue_definition(golden_queue(), offset), ErrorCode::IoError);
      expect_error(mapped::encode_source_definition(source_definition(), offset), ErrorCode::InvalidArgument);
      expect_error(mapped::decode_source_definition(golden_source(), offset), ErrorCode::IoError);
      expect_error(mapped::encode_publishing_definition(publishing_definition(), offset), ErrorCode::InvalidArgument);
      expect_error(mapped::decode_publishing_definition(golden_publishing(), offset), ErrorCode::IoError);
    }
  }
} // namespace
