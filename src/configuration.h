#pragma once

#include "angaria/descriptors.h"
#include "angaria/results.h"
#include "angaria/registry.h"

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace angaria::detail
{
  template <typename T>
  struct ConfigurationEntry
  {
    T _descriptor;
    std::string _path;
  };

  // A preflighted, owning plan. IDs reflect registration into a new Session:
  // creator first, other Process Tasks, Thread Tasks, Sources, Queues, Publishers.
  // Attachment uses a local symbolic namespace for declared and existing names;
  // that validation-only plan is never applied to the Registry. Field IDs stay
  // application-defined. Original TOML paths survive ordering for diagnostics.
  struct ConfigurationPlan
  {
    SessionOptions _options;
    std::vector<ConfigurationEntry<TaskDescriptor>> _tasks;
    std::vector<ConfigurationEntry<FieldDescriptor>> _fields;
    std::vector<ConfigurationEntry<MessageDescriptor>> _messages;
    std::vector<ConfigurationEntry<SourceQueueSetDescriptor>> _sources;
    std::vector<ConfigurationEntry<QueueDescriptor>> _queues;
    std::vector<ConfigurationEntry<PublishingQueueSetDescriptor>> _publishing;
  };

  Result<SessionOptions> load_configuration_options(const std::filesystem::path& file);
  Result<ConfigurationPlan> load_configuration(const std::filesystem::path& file,
                                                std::string_view creator = {},
                                                const Registry* existing = nullptr);
} // namespace angaria::detail
