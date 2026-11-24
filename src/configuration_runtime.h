#pragma once

#include "configuration.h"
#include "angaria/runtime.h"

#include <cstdint>

namespace angaria::detail
{
  // Preflight includes every retained registration/membership/activation and
  // its ring allocation before bootstrap creates any files or initial updates.
  void preflight_configuration(const ConfigurationPlan& configuration, std::uint64_t bootstrapTail);
  void apply_configuration(CreatedSession& session, const ConfigurationPlan& configuration);
  void match_configuration_options(const SessionOptions& requested, const SessionOptions& recorded);
} // namespace angaria::detail
