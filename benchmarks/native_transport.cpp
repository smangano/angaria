#include "angaria/detail/error_support.h"
#include "angaria/runtime.h"

#include "publication_runtime.h"
#include "runtime_state.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <iostream>
#include <memory>
#include <mutex>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

namespace
{
  using Clock = std::chrono::steady_clock;
  using TimePoint = Clock::time_point;
  using namespace angaria;

  constexpr std::size_t channel_count = 4;
  constexpr std::uint64_t ring_capacity = 8;

  struct BenchmarkFailure
  {
    Error _error;
  };

  template <typename T>
  T value(Result<T> result)
  {
    return detail::value_or_throw<BenchmarkFailure>(std::move(result));
  }

  void require(bool condition, std::string_view message)
  {
    if (!condition)
    {
      throw BenchmarkFailure{Error{ErrorCode::InvalidState, std::string{message}, "benchmark"}};
    }
  }

  std::uint64_t elapsed_ns(TimePoint first, TimePoint last)
  {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(last - first).count());
  }

  enum class Metric : std::size_t
  {
    Issue,
    Send,
    Receive,
    Acknowledge,
    Cycle,
    GateWait,
    GateHeld,
    Prepare,
    FileGrowth,
    Materialize,
    Apply,
    ProfileSend,
    Count
  };

  constexpr std::array<std::string_view, static_cast<std::size_t>(Metric::Count)> metric_names{
      "issue", "send", "receive", "acknowledge", "cycle", "gate_wait", "gate_held", "prepare",
      "file_growth", "materialize", "apply", "profile_send"};

  struct Options
  {
    std::filesystem::path _directory;
    std::uint64_t _messages{};
    std::uint64_t _warmupMessages{};
    std::size_t _producers{};
    std::size_t _payloadBytes{};
    std::string_view _mode;
  };

  std::uint64_t parse_count(std::string_view text, std::string_view name)
  {
    std::uint64_t result{};
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), result);
    if (text.empty() || parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size())
    {
      throw BenchmarkFailure{Error{ErrorCode::InvalidArgument, "Expected an unsigned decimal integer", std::string{name}}};
    }
    return result;
  }

  Options parse_options(int argc, char** argv)
  {
    if (argc != 7)
    {
      throw BenchmarkFailure{Error{ErrorCode::InvalidArgument,
          "Usage: angaria_transport_benchmark DIRECTORY TOTAL WARMUP PRODUCERS PAYLOAD_BYTES MODE", "arguments"}};
    }
    const auto total = parse_count(argv[2], "total");
    const auto warmup = parse_count(argv[3], "warmup");
    const auto producers = parse_count(argv[4], "producers");
    const auto bytes = parse_count(argv[5], "payload_bytes");
    const std::string_view mode{argv[6]};
    if (std::string_view{argv[1]}.empty() || total == 0 || total > 4096 || warmup > 256 ||
        (producers != 1 && producers != 2 && producers != 4) ||
        bytes < 8 || bytes > 4096 || bytes % 8 != 0 ||
        (mode != "serial" && mode != "phased" && mode != "profile") ||
        (mode == "serial" && producers != 1))
    {
      throw BenchmarkFailure{Error{ErrorCode::InvalidArgument,
          "Require TOTAL 1..4096, WARMUP 0..256, PRODUCERS 1/2/4, PAYLOAD_BYTES 8..4096 divisible by 8, "
          "and MODE serial/phased/profile; serial requires one producer", "arguments"}};
    }
    if (total % producers != 0 || warmup % producers != 0)
    {
      throw BenchmarkFailure{Error{ErrorCode::InvalidArgument,
          "TOTAL and WARMUP must be divisible by PRODUCERS", "arguments"}};
    }
    return Options{argv[1], total, warmup, static_cast<std::size_t>(producers), static_cast<std::size_t>(bytes), mode};
  }

  struct Channel
  {
    Channel(Task producer, Task consumer, QueueWriter writer, SourceQueueSet source, std::size_t samples)
        : _producer(std::move(producer)), _consumer(std::move(consumer)), _writer(std::move(writer)),
          _source(std::move(source)), _producerClaim(_producer.claim()), _consumerClaim(_consumer.claim()),
          _queue(_writer.id()), _sourceSet(_source.id())
    {
      for (auto& metric : _samples)
      {
        metric.reserve(samples);
      }
    }

    void record(Metric metric, std::uint64_t duration)
    {
      _samples[static_cast<std::size_t>(metric)].push_back(duration);
    }

    Task _producer;
    Task _consumer;
    QueueWriter _writer;
    SourceQueueSet _source;
    TaskClaim _producerClaim;
    TaskClaim _consumerClaim;
    QueueId _queue;
    SourceQueueSetId _sourceSet;
    std::array<std::vector<std::uint64_t>, static_cast<std::size_t>(Metric::Count)> _samples;
    std::uint64_t _completed{};
    std::uint64_t _checksum{};
  };

  // The private profile observes journal engine boundaries. Its virtual calls
  // and four clock reads add overhead; it is reported separately from public API timing.
  class JournalProbe final : public detail::mapped::JournalFaultInjector
  {
  public:
    OperationResult checkpoint(detail::mapped::JournalCheckpoint point, std::uint64_t) override
    {
      using Checkpoint = detail::mapped::JournalCheckpoint;
      std::size_t selected{};
      switch (point)
      {
      case Checkpoint::Prepared: selected = 0; break;
      case Checkpoint::FileGrowth: selected = 1; break;
      case Checkpoint::Committed: selected = 2; break;
      case Checkpoint::Idle: selected = 3; break;
      default: return std::monostate{};
      }
      _times[selected] = Clock::now();
      ++_counts[selected];
      return std::monostate{};
    }

    std::array<TimePoint, 4> _times{};
    std::array<std::uint64_t, 4> _counts{};
  };

  void check_report(const PublicationReport& report, PublicationId publication, QueueId queue)
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

  void send_one(Channel& channel, MessageView message, std::uint64_t sequence, bool measured,
                const std::shared_ptr<detail::SessionState>& profileState)
  {
    const auto issueStart = Clock::now();
    auto issued = channel._producer.next_publication_id();
    const auto issueEnd = Clock::now();
    const auto publication = value(std::move(issued));
    require(publication._producer == channel._producerClaim && publication._sequence == sequence,
            "Publication identity sequence changed");
    if (measured)
    {
      channel.record(Metric::Issue, elapsed_ns(issueStart, issueEnd));
    }

    std::array<DestinationOutcome, 1> destinations;
    PublicationReport report;
    if (!profileState)
    {
      const auto sendStart = Clock::now();
      report = channel._writer.send(publication, message, destinations);
      const auto sendEnd = Clock::now();
      if (measured)
      {
        channel.record(Metric::Send, elapsed_ns(sendStart, sendEnd));
      }
    }
    else
    {
      JournalProbe probe;
      const auto request = Clock::now();
      auto acquired = profileState->_controller->acquire_mutation();
      const auto acquisition = Clock::now();
      auto guard = value(std::move(acquired));
      {
        detail::RuntimeAccess access{profileState, std::move(guard), true, &probe};
        report = detail::runtime_send(access, channel._producerClaim, channel._queue,
                                      publication, message, destinations);
      }
      const auto released = Clock::now();
      require(probe._counts == std::array<std::uint64_t, 4>{1, 1, 1, 1},
              "Profile did not observe exactly one complete append transaction");
      require(acquisition <= probe._times[0] && probe._times[0] <= probe._times[1] &&
              probe._times[1] <= probe._times[2] && probe._times[2] <= probe._times[3] &&
              probe._times[3] <= released, "Profile boundaries were not ordered");
      channel.record(Metric::GateWait, elapsed_ns(request, acquisition));
      channel.record(Metric::GateHeld, elapsed_ns(acquisition, released));
      channel.record(Metric::Prepare, elapsed_ns(acquisition, probe._times[0]));
      channel.record(Metric::FileGrowth, elapsed_ns(probe._times[0], probe._times[1]));
      channel.record(Metric::Materialize, elapsed_ns(probe._times[1], probe._times[2]));
      channel.record(Metric::Apply, elapsed_ns(probe._times[2], probe._times[3]));
      channel.record(Metric::ProfileSend, elapsed_ns(request, released));
    }
    check_report(report, publication, channel._queue);
  }

  void receive_one(Channel& channel, SchemaId schema, std::span<const std::byte> payload,
                   std::uint64_t sequence, bool measured)
  {
    const auto receiveStart = Clock::now();
    auto received = channel._source.receive(std::chrono::seconds{5});
    const auto receiveEnd = Clock::now();
    if (const auto* error = std::get_if<Error>(&received))
    {
      throw BenchmarkFailure{*error};
    }
    const auto* delivery = std::get_if<Delivery>(&received);
    require(delivery != nullptr, "Expected delivery; received a timeout or delivery gap");
    const auto& header = delivery->_message._header;
    const auto& token = delivery->_token;
    // Validate the borrowed payload before acknowledgment ends its lifetime.
    // Wall-clock timestamps may be epoch zero or move backward; only presence
    // and the direct Queue's recorded enqueue/publication identity are required.
    require(header._schema == schema && header._format == PayloadFormat::Binary &&
            header._publication == PublicationId{channel._producerClaim, sequence} &&
            header._payloadBytes == payload.size() && header._flags == 0 &&
            header._queue == channel._queue && header._queueSequence == sequence &&
            header._consumer == channel._consumerClaim && header._enqueuedAt.has_value() &&
            header._dequeuedAt.has_value() && *header._enqueuedAt == header._publishedAt &&
            token._consumer == channel._consumerClaim && token._sourceSet == channel._sourceSet &&
            token._queue == channel._queue && token._sequence == sequence && token._issuance != 0,
            "Delivery header or acknowledgment token changed");
    require(delivery->_message._payload.size() == payload.size() &&
            std::equal(payload.begin(), payload.end(), delivery->_message._payload.begin()),
            "Delivery payload changed");
    std::uint64_t checksum{};
    for (const auto byte : delivery->_message._payload)
    {
      checksum += std::to_integer<std::uint8_t>(byte);
    }
    const auto acknowledgeStart = Clock::now();
    auto acknowledged = channel._source.acknowledge(token);
    const auto acknowledgeEnd = Clock::now();
    value(std::move(acknowledged));
    if (measured)
    {
      channel.record(Metric::Receive, elapsed_ns(receiveStart, receiveEnd));
      channel.record(Metric::Acknowledge, elapsed_ns(acknowledgeStart, acknowledgeEnd));
      ++channel._completed;
      channel._checksum += checksum;
    }
  }

  // Every worker reaches the start barrier after its thread has been created.
  // Stop the phase at the last worker's completion, excluding join overhead.
  template <typename Function>
  std::uint64_t run_workers(std::size_t count, Function function)
  {
    std::mutex mutex;
    std::condition_variable condition;
    std::size_t ready{};
    bool released{};
    bool cancelled{};
    std::array<TimePoint, channel_count> finished{};
    std::array<std::exception_ptr, channel_count> failures{};
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
    TimePoint start;
    {
      std::unique_lock lock{mutex};
      condition.wait(lock, [&]
      {
        return ready == count;
      });
      start = Clock::now();
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

  void write_metrics(std::ostream& output, const std::vector<Channel>& channels)
  {
    bool first = true;
    output << '[';
    for (std::size_t metric = 0; metric < metric_names.size(); ++metric)
    {
      std::vector<std::uint64_t> samples;
      for (const auto& channel : channels)
      {
        samples.insert(samples.end(), channel._samples[metric].begin(), channel._samples[metric].end());
      }
      if (samples.empty())
      {
        continue;
      }
      std::uint64_t total{};
      for (const auto duration : samples)
      {
        total += duration;
      }
      std::sort(samples.begin(), samples.end());
      const auto percentile = [&](std::size_t percent)
      {
        return samples[(percent * samples.size() + 99) / 100 - 1];
      };
      if (!first)
      {
        output << ',';
      }
      first = false;
      output << "{\"name\":\"" << metric_names[metric] << "\",\"count\":" << samples.size()
                << ",\"min_ns\":" << samples.front() << ",\"p50_ns\":" << percentile(50)
                << ",\"p95_ns\":" << percentile(95) << ",\"p99_ns\":" << percentile(99)
                << ",\"max_ns\":" << samples.back() << ",\"total_ns\":" << total << '}';
    }
    output << ']';
  }

  void run(const Options& arguments)
  {
    SessionOptions options;
    options._name = "Native transport benchmark";
    options._applicationVersion = "1";
    options._directory = arguments._directory;
    options._maxLogBytes = 64ULL * 1024 * 1024;
    options._limits = CapacityLimits::empty().tasks(9).queues(4).source_sets(4).memberships_per_set(1)
                          .fields(1).message_schemas(1);
    options._defaults._ringCapacity = ring_capacity;
    TaskDescriptor setup;
    setup._name = "Setup";
    auto created = value(Session::create(options, setup));
    auto registry = created._creator.registry();

    FieldDescriptor field;
    field._id = FieldId{0xee000001};
    field._name = "Word";
    field._base = EFieldBase::BASE_UINT64;
    field._size = 8;
    const auto fieldId = value(registry.register_field(field));
    MessageDescriptor message;
    message._schema = SchemaId{MessageType{1}, 1};
    message._name = "Payload";
    message._fields.assign(arguments._payloadBytes / 8, fieldId);
    const auto schema = value(registry.register_message(message));
    auto codec = value(BinaryPayloadCodec::from_registry(registry, schema, arguments._payloadBytes));
    const std::vector<PayloadValue> values(arguments._payloadBytes / 8, std::uint64_t{1});
    const auto payload = value(codec.encode(values));
    require(payload.size() == arguments._payloadBytes, "Encoded payload size changed");
    MessageHeader outgoingHeader;
    outgoingHeader._schema = schema;
    outgoingHeader._payloadBytes = payload.size();
    const MessageView outgoing{outgoingHeader, payload};

    // Four complete channels are always configured. Producer count changes
    // contention without changing catalog geometry or the number of claims.
    std::vector<Channel> channels;
    channels.reserve(channel_count);
    for (std::size_t index = 0; index < channel_count; ++index)
    {
      const auto suffix = std::to_string(index + 1);
      TaskDescriptor producerDescriptor;
      producerDescriptor._name = "Producer" + suffix;
      const auto producerId = value(registry.register_task(producerDescriptor));
      TaskDescriptor consumerDescriptor;
      consumerDescriptor._name = "Consumer" + suffix;
      const auto consumerId = value(registry.register_task(consumerDescriptor));
      SourceQueueSetDescriptor sourceDescriptor;
      sourceDescriptor._name = "Inputs" + suffix;
      sourceDescriptor._owner = consumerId;
      const auto sourceId = value(registry.register_source_set(sourceDescriptor));
      QueueDescriptor queueDescriptor;
      queueDescriptor._name = "Queue" + suffix;
      queueDescriptor._producer = producerId;
      queueDescriptor._sourceSet = sourceId;
      const auto queueId = value(registry.register_queue(queueDescriptor));
      value(registry.activate_source_set(sourceId));
      value(registry.activate_queue(queueId));
      auto producer = value(created._session.attach_task(producerId));
      auto consumer = value(created._session.attach_task(consumerId));
      auto writer = value(producer.open_writer(queueId));
      auto source = value(consumer.open_source_set(sourceId));
      channels.emplace_back(std::move(producer), std::move(consumer), std::move(writer), std::move(source),
                            arguments._messages / arguments._producers);
    }

    const auto warmupPerChannel = arguments._warmupMessages / arguments._producers;
    const auto measuredPerChannel = arguments._messages / arguments._producers;
    for (std::size_t index = 0; index < arguments._producers; ++index)
    {
      for (std::uint64_t sequence = 1; sequence <= warmupPerChannel; ++sequence)
      {
        send_one(channels[index], outgoing, sequence, false, {});
        receive_one(channels[index], schema, payload, sequence, false);
      }
    }

    std::shared_ptr<detail::SessionState> profileState;
    if (arguments._mode == "profile")
    {
      profileState = std::make_shared<detail::SessionState>();
      profileState->_directory = std::filesystem::canonical(arguments._directory);
      profileState->_controller = value(detail::mapped::SessionController::open_existing(profileState->_directory));
    }
    const auto file = arguments._directory / "session.ang";
    const auto before = std::filesystem::file_size(file);
    std::uint64_t afterPublish{};
    std::uint64_t afterDrain{};
    std::uint64_t publishPhase{};
    std::uint64_t drainPhase{};
    std::uint64_t cyclePhase{};
    if (arguments._mode == "serial")
    {
      const auto start = Clock::now();
      for (std::uint64_t index = 0; index < measuredPerChannel; ++index)
      {
        const auto cycleStart = Clock::now();
        const auto sequence = warmupPerChannel + index + 1;
        send_one(channels[0], outgoing, sequence, true, {});
        receive_one(channels[0], schema, payload, sequence, true);
        channels[0].record(Metric::Cycle, elapsed_ns(cycleStart, Clock::now()));
      }
      cyclePhase = elapsed_ns(start, Clock::now());
      // Serial publication and draining are interleaved; both sizes denote
      // the final state rather than a nonexistent separate publish boundary.
      afterPublish = std::filesystem::file_size(file);
      afterDrain = afterPublish;
    }
    else
    {
      publishPhase = run_workers(arguments._producers, [&](std::size_t channel)
      {
        for (std::uint64_t index = 0; index < measuredPerChannel; ++index)
        {
          send_one(channels[channel], outgoing, warmupPerChannel + index + 1, true, profileState);
        }
      });
      afterPublish = std::filesystem::file_size(file);
      drainPhase = run_workers(arguments._producers, [&](std::size_t channel)
      {
        for (std::uint64_t index = 0; index < measuredPerChannel; ++index)
        {
          receive_one(channels[channel], schema, payload, warmupPerChannel + index + 1, true);
        }
      });
      afterDrain = std::filesystem::file_size(file);
    }
    require(afterPublish >= before && afterDrain == afterPublish, "Draining unexpectedly grew the Session file");
    std::uint64_t completed{};
    std::uint64_t checksum{};
    for (auto& channel : channels)
    {
      auto extra = channel._source.receive();
      if (const auto* error = std::get_if<Error>(&extra))
      {
        throw BenchmarkFailure{*error};
      }
      require(std::holds_alternative<ReceiveTimeout>(extra), "Unexpected delivery remained after draining");
      value(channel._source.close());
      completed += channel._completed;
      checksum += channel._checksum;
    }
    require(completed == arguments._messages && checksum == arguments._messages * arguments._payloadBytes / 8,
            "Completed delivery count or payload checksum changed");
    const auto version = value(created._session.metadata())._angariaVersion;
    require(!version.empty() && version.find_first_not_of("0123456789.") == std::string::npos,
            "Framework version is not a dotted numeric version");
    // Build the complete result before writing stdout, so failed validation or
    // statistics allocation cannot leave a partial success document behind.
    std::ostringstream output;
    output.exceptions(std::ios::badbit | std::ios::failbit);
    output << "{\"framework_version\":\"" << version << "\",\"mode\":\"" << arguments._mode
              << "\",\"api_kind\":\"" << (profileState ? "instrumented_private_engine" : "buffered_public")
              << "\",\"messages\":" << arguments._messages << ",\"warmup_messages\":" << arguments._warmupMessages
              << ",\"producers\":" << arguments._producers << ",\"channels\":" << channel_count
              << ",\"payload_bytes\":" << arguments._payloadBytes << ",\"ring_capacity\":" << ring_capacity
              << ",\"history_before\":" << arguments._warmupMessages
              << ",\"history_after\":" << arguments._warmupMessages + arguments._messages
              << ",\"file_bytes_before\":" << before << ",\"file_bytes_after_publish\":" << afterPublish
              << ",\"file_bytes_after_drain\":" << afterDrain << ",\"append_bytes\":" << afterPublish - before
              << ",\"publish_phase_ns\":" << publishPhase << ",\"drain_phase_ns\":" << drainPhase
              << ",\"cycle_phase_ns\":" << cyclePhase << ",\"completed\":" << completed
              << ",\"checksum\":" << checksum << ",\"metrics\":";
    write_metrics(output, channels);
    output << "}\n";
    std::cout << output.str();
  }

  void write_error(const Error& error)
  {
    std::cerr << detail::error_name(error._code) << " [" << error._path << "]: " << error._message << '\n';
  }
} // namespace

int main(int argc, char** argv)
{
  try
  {
    run(parse_options(argc, argv));
    return 0;
  }
  catch (const BenchmarkFailure& failure)
  {
    write_error(failure._error);
  }
  catch (const angaria::detail::RuntimeFailure& failure)
  {
    write_error(failure._error);
  }
  catch (const std::exception& failure)
  {
    std::cerr << "benchmark: " << failure.what() << '\n';
  }
  return 1;
}
