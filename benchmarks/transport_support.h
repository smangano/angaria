#pragma once

// Shared benchmark setup and checks. These are tool helpers, not primer APIs;
// application lessons continue to spell out configuration and Result checks.
#include "angaria/detail/error_support.h"
#include "angaria/runtime.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <exception>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace angaria::benchmarks
{
  inline constexpr std::size_t channel_count = 4;
  inline constexpr std::uint64_t ring_capacity = 8;

  inline std::uint64_t elapsed_ns(std::chrono::steady_clock::time_point first,
                                  std::chrono::steady_clock::time_point last)
  {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(last - first).count());
  }

  struct BenchmarkFailure
  {
    Error _error;
  };

  template <typename T>
  T value(Result<T> result)
  {
    return detail::value_or_throw<BenchmarkFailure>(std::move(result));
  }

  inline void require(bool condition, std::string_view message)
  {
    if (!condition)
    {
      throw BenchmarkFailure{Error{ErrorCode::InvalidState, std::string{message}, "benchmark"}};
    }
  }

  inline std::uint64_t parse_count(std::string_view text, std::string_view name)
  {
    std::uint64_t result{};
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), result);
    if (text.empty() || parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size())
    {
      throw BenchmarkFailure{Error{ErrorCode::InvalidArgument,
          "Expected an unsigned decimal integer", std::string{name}}};
    }
    return result;
  }

  inline void check_report(const PublicationReport& report, PublicationId publication, QueueId queue)
  {
    if (report._error != ErrorCode::None)
    {
      throw BenchmarkFailure{Error{report._error, "Publication returned an error", "benchmark.send"}};
    }
    require(report._publication == publication && report._status == PublicationStatus::Accepted &&
                report._destinations.size() == 1 && report._destinations[0]._queue == queue &&
                report._destinations[0]._status == DestinationStatus::Accepted &&
                report._destinations[0]._reason == ErrorCode::None,
            "Publication was not accepted by its sole destination");
  }

  inline CreatedSession create_session(const std::filesystem::path& directory, std::string name,
                                         std::uint32_t capacityScale = 1)
  {
    SessionOptions options;
    options._name = std::move(name);
    options._applicationVersion = "1";
    options._directory = directory;
    options._maxLogBytes = 64ULL * 1024 * 1024;
    options._limits = CapacityLimits::empty().tasks(9 * capacityScale).queues(4 * capacityScale)
                          .source_sets(4 * capacityScale).memberships_per_set(1)
                          .fields(capacityScale).message_schemas(capacityScale);
    options._defaults._ringCapacity = ring_capacity;
    TaskDescriptor setup;
    setup._name = "Setup";
    return value(Session::create(options, setup));
  }

  struct ChannelEndpoints
  {
    ChannelEndpoints(Task producer, Task consumer, QueueWriter writer, SourceQueueSet source)
        : _producer(std::move(producer)), _consumer(std::move(consumer)), _writer(std::move(writer)),
          _source(std::move(source)), _producerClaim(_producer.claim()), _consumerClaim(_consumer.claim()),
          _queue(_writer.id()), _sourceSet(_source.id())
    {
    }

    Task _producer;
    Task _consumer;
    QueueWriter _writer;
    SourceQueueSet _source;
    TaskClaim _producerClaim;
    TaskClaim _consumerClaim;
    QueueId _queue;
    SourceQueueSetId _sourceSet;
  };

  // Geometry stays constant even when fewer channels actively exchange data.
  inline std::vector<ChannelEndpoints> register_channels(CreatedSession& created)
  {
    auto registry = created._creator.registry();
    std::vector<ChannelEndpoints> channels;
    channels.reserve(channel_count);
    for (std::size_t index = 0; index < channel_count; ++index)
    {
      const auto suffix = std::to_string(index + 1);
      TaskDescriptor producerDescriptor;
      producerDescriptor._name = "Producer" + suffix;
      const auto producer = value(registry.register_task(producerDescriptor));
      TaskDescriptor consumerDescriptor;
      consumerDescriptor._name = "Consumer" + suffix;
      const auto consumer = value(registry.register_task(consumerDescriptor));
      SourceQueueSetDescriptor sourceDescriptor;
      sourceDescriptor._name = "Inputs" + suffix;
      sourceDescriptor._owner = consumer;
      const auto source = value(registry.register_source_set(sourceDescriptor));
      QueueDescriptor queueDescriptor;
      queueDescriptor._name = "Queue" + suffix;
      queueDescriptor._producer = producer;
      queueDescriptor._sourceSet = source;
      const auto queue = value(registry.register_queue(queueDescriptor));
      value(registry.activate_source_set(source));
      value(registry.activate_queue(queue));
      auto producerTask = value(created._session.attach_task(producer));
      auto consumerTask = value(created._session.attach_task(consumer));
      auto writer = value(producerTask.open_writer(queue));
      auto reader = value(consumerTask.open_source_set(source));
      channels.emplace_back(std::move(producerTask), std::move(consumerTask), std::move(writer), std::move(reader));
    }
    return channels;
  }

  // Worker construction precedes the common start barrier. Every failure path
  // joins started threads; phase timing stops before join overhead.
  template <typename Function>
  std::uint64_t run_workers(std::size_t count, Function function,
                            std::chrono::steady_clock::time_point* phaseStart = nullptr)
  {
    using Clock = std::chrono::steady_clock;
    require(count > 0 && count <= channel_count + 1, "Invalid benchmark worker count");
    std::mutex mutex;
    std::condition_variable condition;
    std::size_t ready{};
    bool released{};
    bool cancelled{};
    std::array<std::exception_ptr, channel_count + 1> failures{};
    std::array<Clock::time_point, channel_count + 1> finished{};
    std::vector<std::thread> workers;
    workers.reserve(count);
    try
    {
      for (std::size_t index = 0; index < count; ++index)
      {
        workers.emplace_back([&, index]
        {
          {
            std::unique_lock lock{mutex};
            ++ready;
            condition.notify_all();
            condition.wait(lock, [&]
            {
              return released;
            });
            if (cancelled)
            {
              return;
            }
          }
          try
          {
            function(index);
          }
          catch (...)
          {
            failures[index] = std::current_exception();
          }
          finished[index] = Clock::now();
        });
      }
    }
    catch (...)
    {
      {
        std::lock_guard lock{mutex};
        cancelled = true;
        released = true;
      }
      condition.notify_all();
      for (auto& worker : workers)
      {
        worker.join();
      }
      throw;
    }
    Clock::time_point start;
    {
      std::unique_lock lock{mutex};
      condition.wait(lock, [&]
      {
        return ready == count;
      });
      start = Clock::now();
      if (phaseStart)
      {
        *phaseStart = start;
      }
      released = true;
    }
    condition.notify_all();
    for (auto& worker : workers)
    {
      worker.join();
    }
    for (std::size_t index = 0; index < count; ++index)
    {
      if (failures[index])
      {
        std::rethrow_exception(failures[index]);
      }
    }
    return elapsed_ns(start, *std::max_element(finished.begin(), finished.begin() + count));
  }
} // namespace angaria::benchmarks
