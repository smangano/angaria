#pragma once

// Private acknowledged endpoints, not a public Session/profile configuration.
// Reuse the recorded Queue's real immutable schema/topology fixture and switch
// only its explicit processing contract; message recording is unchanged.
#include "recorded_queue_fixture.h"
#include "native_claim.h"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <string>

namespace angaria::test_support
{
  inline detail::recorded::RecordedQueueOptions recorded_acknowledged_options(
      const std::filesystem::path& directory, std::uint64_t publications = 128)
  {
    auto options = recorded_queue_options(directory, 2, publications);
    options._acknowledged = true;
    return options;
  }

  inline UtcTimestamp recorded_test_time(std::int64_t value)
  {
    return UtcTimestamp{std::chrono::nanoseconds{value}};
  }

  inline std::filesystem::path recorded_progress_path(const detail::recorded::RecordedQueueOptions& options)
  {
    return options._progressPath.empty() ? std::filesystem::path{options._connectionPath.string() + ".progress"}
                                         : options._progressPath;
  }

  inline detail::recorded::ProgressOptions recorded_progress_options(
      const detail::recorded::RecordedQueueOptions& options)
  {
    auto connection = require_value(detail::mapped::NativeFile::open_existing(
        options._connectionPath, detail::mapped::FileAccess::ReadOnly));
    detail::recorded::ProgressOptions progress;
    progress._path = recorded_progress_path(options);
    progress._session = options._log._header._session;
    progress._consumer = options._consumer;
    progress._source = options._source;
    progress._queue = options._queue;
    progress._connectionGeneration = options._generation;
    progress._connectionIdentity = require_value(connection.identity());
    progress._publicationCapacity = options._log._header._publicationCapacity;
    progress._liveAbiProfile = options._log._header._liveAbiProfile;
    return progress;
  }

  inline std::uint64_t recorded_consumer_incarnation(const detail::recorded::RecordedQueueOptions& options)
  {
    auto file = require_value(detail::mapped::NativeFile::open_existing(
        options._connectionPath, detail::mapped::FileAccess::ReadOnly));
    auto mapping = require_value(file.map_read_only(512, 8));
    return require_value(detail::mapped::AtomicReadWord::bind(require_value(mapping.bytes()), 0)).load_acquire();
  }
} // namespace angaria::test_support
