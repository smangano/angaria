#pragma once

#include <cstdint>

namespace angaria
{
  // Entity identities are scoped to one session. Zero denotes an invalid identity.
  enum class TaskId : std::uint16_t
  {
    Invalid = 0
  };

  enum class QueueId : std::uint16_t
  {
    Invalid = 0
  };

  // These distinct C++ types share the Registry's Queue Set allocation namespace.
  enum class SourceQueueSetId : std::uint16_t
  {
    Invalid = 0
  };

  enum class PublishingQueueSetId : std::uint16_t
  {
    Invalid = 0
  };

  enum class MessageType : std::uint16_t
  {
    Invalid = 0
  };

  // Registration validates the full 0xEE-prefixed Field identity.
  enum class FieldId : std::uint32_t
  {
    Invalid = 0
  };

  struct SchemaId
  {
    MessageType _type{};
    std::uint32_t _version{};

    bool operator==(const SchemaId&) const = default;
  };

  struct SessionId
  {
    std::uint64_t _high{};
    std::uint64_t _low{};

    bool operator==(const SessionId&) const = default;
  };

  struct TaskClaim
  {
    SessionId _session{};
    TaskId _task{};
    std::uint64_t _incarnation{};

    bool operator==(const TaskClaim&) const = default;
  };

  struct PublicationId
  {
    TaskClaim _producer{};
    std::uint64_t _sequence{};

    bool operator==(const PublicationId&) const = default;
  };

  struct DeliveryToken
  {
    TaskClaim _consumer{};
    SourceQueueSetId _sourceSet{};
    QueueId _queue{};
    std::uint64_t _sequence{};
    std::uint64_t _issuance{};

    bool operator==(const DeliveryToken&) const = default;
  };
} // namespace angaria
