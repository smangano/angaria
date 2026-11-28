#pragma once

#include "angaria/descriptors.h"
#include "angaria/results.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>

namespace angaria::detail::wire
{
  // Array positions describe explicit, contiguous stored codes; they never
  // depend on the C++ enum's underlying ordinal. Callers retain format-specific
  // admission rules and diagnostics for an unknown or unsupported code.
  template <typename Enum, std::size_t N>
  constexpr std::optional<std::uint16_t> encode(Enum value, const std::array<Enum, N>& values,
                                               std::uint16_t first = 1)
  {
    for (std::size_t index = 0; index < N; ++index)
    {
      if (values[index] == value)
      {
        if (index > std::size_t{std::numeric_limits<std::uint16_t>::max()} - first)
        {
          return std::nullopt;
        }
        return static_cast<std::uint16_t>(first + index);
      }
    }
    return std::nullopt;
  }

  template <typename Enum, std::size_t N>
  constexpr std::optional<Enum> decode(std::uint16_t code, const std::array<Enum, N>& values,
                                       std::uint16_t first = 1)
  {
    if (code < first || static_cast<std::size_t>(code - first) >= N)
    {
      return std::nullopt;
    }
    return values[code - first];
  }

  inline constexpr std::array storage_mappings{StorageMapping::TwoPlane, StorageMapping::Segmented};
  inline constexpr std::array delivery_policies{DeliveryPolicy::RecoverFromLog, DeliveryPolicy::AllowLoss};
  inline constexpr std::array selection_policies{SelectionPolicy::Strict, SelectionPolicy::Fair,
                                                 SelectionPolicy::WeightedRandom};
  inline constexpr std::array wait_policies{WaitPolicy::Spin, WaitPolicy::EventFd};
  inline constexpr std::array acceptance_policies{AcceptancePolicy::AllDestinations, AcceptancePolicy::Partial};
  inline constexpr std::array acknowledgment_modes{AcknowledgmentMode::Required, AcknowledgmentMode::None};
  inline constexpr std::array payload_formats{PayloadFormat::Binary, PayloadFormat::Json, PayloadFormat::Xml,
                                              PayloadFormat::Csv, PayloadFormat::Text};
  inline constexpr std::array task_kinds{TaskKind::Process, TaskKind::Thread};
  inline constexpr std::array entity_states{EntityState::Draft, EntityState::Active};
  inline constexpr std::array field_bases{
      EFieldBase::BASE_NONE, EFieldBase::BASE_CHAR, EFieldBase::BASE_BYTE,
      EFieldBase::BASE_INT16, EFieldBase::BASE_UINT16, EFieldBase::BASE_INT32,
      EFieldBase::BASE_UINT32, EFieldBase::BASE_INT64, EFieldBase::BASE_UINT64,
      EFieldBase::BASE_FLOAT32, EFieldBase::BASE_FLOAT64, EFieldBase::BASE_DATE,
      EFieldBase::BASE_TIMESTAMP, EFieldBase::BASE_YEARS, EFieldBase::BASE_MONTHS,
      EFieldBase::BASE_DAYS, EFieldBase::BASE_HOURS, EFieldBase::BASE_MINS,
      EFieldBase::BASE_SECS, EFieldBase::BASE_MILLISECS, EFieldBase::BASE_MICROSECS,
      EFieldBase::BASE_NANOSECS, EFieldBase::BASE_CHARS, EFieldBase::BASE_BYTES,
      EFieldBase::BASE_SYMBOL, EFieldBase::BASE_STRING, EFieldBase::BASE_INT8};
  inline constexpr std::array logical_kinds{FieldLogicalKind::None, FieldLogicalKind::Boolean,
                                            FieldLogicalKind::Bits, FieldLogicalKind::Enumeration,
                                            FieldLogicalKind::FixedAscii};

  // Rejection reasons start at zero, unlike the policy families above.
  inline constexpr std::array error_codes{
      ErrorCode::None, ErrorCode::InvalidArgument, ErrorCode::InvalidState,
      ErrorCode::InvalidConfiguration, ErrorCode::ConfigurationMismatch, ErrorCode::NotFound,
      ErrorCode::Conflict, ErrorCode::CapacityExceeded, ErrorCode::OwnershipBusy,
      ErrorCode::WrongOwner, ErrorCode::ReadOnlyRegistry, ErrorCode::NotActive,
      ErrorCode::UnsupportedPolicy, ErrorCode::SchemaConflict, ErrorCode::LogFull,
      ErrorCode::StaleDelivery, ErrorCode::OutputBufferTooSmall, ErrorCode::HistoryUnavailable,
      ErrorCode::IoError, ErrorCode::OutcomeUnknown, ErrorCode::QueueFull};
} // namespace angaria::detail::wire
