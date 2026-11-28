#pragma once

// Private prepared-storage fixtures shared by correctness, crash and cost tests.
// They intentionally supply opaque schema/catalog identities; public Registry
// validation and runnable application configuration belong to later admission.
#include "native_atomic.h"
#include "producer_log.h"
#include "result_helpers.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>

#include <unistd.h>

namespace angaria::test_support
{
  class ProducerLogDirectory
  {
  public:
    ProducerLogDirectory()
    {
      auto pattern = (std::filesystem::temp_directory_path() / "angaria-producer-log-XXXXXX").string();
      const auto directory = ::mkdtemp(pattern.data());
      if (!directory)
      {
        throw std::runtime_error("Cannot create private producer-log test directory");
      }
      _path = directory;
    }

    ~ProducerLogDirectory()
    {
      std::error_code ignored;
      std::filesystem::remove_all(_path, ignored);
    }

    ProducerLogDirectory(const ProducerLogDirectory&) = delete;
    ProducerLogDirectory& operator=(const ProducerLogDirectory&) = delete;

    const std::filesystem::path& path() const noexcept
    {
      return _path;
    }

  private:
    std::filesystem::path _path;
  };

  inline detail::recorded::ProducerLogPaths producer_log_paths(const std::filesystem::path& directory,
                                                              const std::string& tag = "producer")
  {
    return {directory / (tag + ".log"), directory / (tag + ".owner")};
  }

  inline detail::recorded::ProducerLogOptions producer_log_options(std::uint64_t fileBytes = 1048576,
                                                                  std::uint64_t capacity = 1024)
  {
    detail::recorded::ProducerLogOptions options;
    options._header._session = {0x0102030405060708, 0x1112131415161718};
    options._header._producer = TaskId{3};
    options._header._catalogRevision = 1;
    options._header._fileBytes = fileBytes;
    options._header._publicationCapacity = capacity;
    options._header._liveAbiProfile = require_value(detail::mapped::qualify_native_profile())._liveAbiProfile;
    options._queues = {QueueId{4}, QueueId{5}};
    return options;
  }

  inline detail::recorded::ProducerAppendRequest producer_append_request(
      PublicationId publication, std::span<const std::byte> payload, QueueId queue = QueueId{4},
      UtcTimestamp publishedAt = UtcTimestamp{std::chrono::nanoseconds{-7}})
  {
    MessageHeader header;
    header._schema = {MessageType{1}, 1};
    header._format = PayloadFormat::Binary;
    header._payloadBytes = payload.size();
    header._flags = 0xa55a33ccu;
    detail::recorded::ProducerAppendRequest request;
    request._publication = publication;
    request._queue = queue;
    request._message = MessageView{header, payload};
    request._schemaReference = 4096;
    request._topologyRevision = 9;
    request._publishedAt = publishedAt;
    return request;
  }
} // namespace angaria::test_support
