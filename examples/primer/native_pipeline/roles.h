#pragma once

#include "angaria/results.h"

#include <cstdint>
#include <filesystem>
#include <string_view>

// Application roles, not wrappers around Angaria configuration or Results.
// Each implementation shows its public Session/Task/endpoint calls directly.
namespace native_pipeline
{
  inline constexpr std::uint64_t alert_threshold = 10004;
  inline constexpr std::uint64_t max_payload_bytes = 128;
  inline constexpr std::uint32_t max_quote_number = 1000;

  angaria::OperationResult initialize(const std::filesystem::path& configuration);
  angaria::OperationResult publish_quotes(const std::filesystem::path& configuration,
      std::uint32_t first, std::uint32_t count, bool hold);
  angaria::OperationResult process_quotes(const std::filesystem::path& configuration,
      std::uint32_t count, bool hold_first);
  angaria::OperationResult audit_alerts(const std::filesystem::path& configuration,
      std::uint32_t count);
  angaria::OperationResult publication_status(const std::filesystem::path& configuration,
      std::string_view task_name, std::uint64_t incarnation, std::uint64_t sequence);
}
