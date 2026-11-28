#include "transport_support.h"
#include "diagnostic_totals.h"

#include "angaria/detail/checked_arithmetic.h"
#include "performance_diagnostics.h"

#if defined(ANGARIA_ENABLE_CACHEGRIND) && ANGARIA_ENABLE_CACHEGRIND
#include <valgrind/cachegrind.h>
#endif

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <limits>
#include <mutex>
#include <new>
#include <optional>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

namespace diagnostics = angaria::detail::diagnostics;

namespace
{
  // Replacement operators live only in this measurement executable. They count
  // successful C++ allocation requests inside an active Capture, not malloc
  // traffic, outstanding bytes, or allocations made by another thread.
  void* allocate(std::size_t bytes, std::size_t alignment = 0)
  {
    if (alignment != 0)
    {
      if ((alignment & (alignment - 1)) != 0)
      {
        throw std::bad_alloc{};
      }
      alignment = std::max(alignment, alignof(void*));
    }
    for (;;)
    {
      void* result{};
      if (alignment == 0)
      {
        result = std::malloc(bytes == 0 ? 1 : bytes);
      }
      else if (::posix_memalign(&result, alignment, bytes == 0 ? 1 : bytes) != 0)
      {
        result = nullptr;
      }
      if (result)
      {
        diagnostics::record_allocation(bytes);
        return result;
      }
      const auto handler = std::get_new_handler();
      if (!handler)
      {
        throw std::bad_alloc{};
      }
      handler();
    }
  }
  void* allocate_nothrow(std::size_t bytes, std::size_t alignment = 0) noexcept
  {
    try
    {
      return allocate(bytes, alignment);
    }
    catch (...)
    {
      return nullptr;
    }
  }
}

void* operator new(std::size_t bytes)
{
  return allocate(bytes);
}
void* operator new[](std::size_t bytes)
{
  return allocate(bytes);
}
void* operator new(std::size_t bytes, std::align_val_t alignment)
{
  return allocate(bytes, static_cast<std::size_t>(alignment));
}
void* operator new[](std::size_t bytes, std::align_val_t alignment)
{
  return allocate(bytes, static_cast<std::size_t>(alignment));
}
void* operator new(std::size_t bytes, const std::nothrow_t&) noexcept
{
  return allocate_nothrow(bytes);
}
void* operator new[](std::size_t bytes, const std::nothrow_t&) noexcept
{
  return allocate_nothrow(bytes);
}
void* operator new(std::size_t bytes, std::align_val_t alignment, const std::nothrow_t&) noexcept
{
  return allocate_nothrow(bytes, static_cast<std::size_t>(alignment));
}
void* operator new[](std::size_t bytes, std::align_val_t alignment, const std::nothrow_t&) noexcept
{
  return allocate_nothrow(bytes, static_cast<std::size_t>(alignment));
}
void operator delete(void* address) noexcept
{
  std::free(address);
}
void operator delete[](void* address) noexcept
{
  std::free(address);
}
void operator delete(void* address, std::size_t) noexcept
{
  std::free(address);
}
void operator delete[](void* address, std::size_t) noexcept
{
  std::free(address);
}
void operator delete(void* address, std::align_val_t) noexcept
{
  std::free(address);
}
void operator delete[](void* address, std::align_val_t) noexcept
{
  std::free(address);
}
void operator delete(void* address, std::size_t, std::align_val_t) noexcept
{
  std::free(address);
}
void operator delete[](void* address, std::size_t, std::align_val_t) noexcept
{
  std::free(address);
}
void operator delete(void* address, const std::nothrow_t&) noexcept
{
  std::free(address);
}
void operator delete[](void* address, const std::nothrow_t&) noexcept
{
  std::free(address);
}
void operator delete(void* address, std::align_val_t, const std::nothrow_t&) noexcept
{
  std::free(address);
}
void operator delete[](void* address, std::align_val_t, const std::nothrow_t&) noexcept
{
  std::free(address);
}

namespace
{
  using namespace angaria;
  using namespace angaria::benchmarks;
  using Clock = std::chrono::steady_clock;
  using namespace std::chrono_literals;

#if defined(ANGARIA_ENABLE_CACHEGRIND) && ANGARIA_ENABLE_CACHEGRIND
  inline constexpr bool cachegrind_enabled = true;

  // Requests affect only the optional profiler, not transport state. Stop on
  // unwinding too, before Session teardown and error reporting.
  class CachegrindRegion
  {
  public:
    explicit CachegrindRegion(bool active) noexcept : _active(active)
    {
      if (_active)
      {
        CACHEGRIND_START_INSTRUMENTATION;
      }
    }

    ~CachegrindRegion()
    {
      if (_active)
      {
        CACHEGRIND_STOP_INSTRUMENTATION;
      }
    }

    CachegrindRegion(const CachegrindRegion&) = delete;
    CachegrindRegion& operator=(const CachegrindRegion&) = delete;

  private:
    bool _active{};
  };
#else
  inline constexpr bool cachegrind_enabled = false;
#endif

  struct Options
  {
    std::filesystem::path _directory;
    std::uint64_t _messages{16};
    std::uint64_t _history{};
    std::size_t _payloadBytes{8};
    std::size_t _fields{1};
    std::uint32_t _capacityScale{1};
    std::string_view _workload{"serial"};
    std::string_view _collectionName{"counts"};
    diagnostics::Collection _collection{diagnostics::Collection::Counts};
    std::string_view _historyScope{"same"};
    std::size_t _idlePollers{};
    std::string_view _role{};
    std::uint64_t _workBudgetMs{5000};
    std::string_view _cachegrindRegion{"none"};
  };

  [[noreturn]] void invalid(std::string message)
  {
    throw BenchmarkFailure{Error{ErrorCode::InvalidArgument, std::move(message), "arguments"}};
  }

  Options parse_options(int argc, char** argv)
  {
    if (argc < 2 || std::string_view{argv[1]}.empty())
    {
      invalid("Usage: angaria_transport_diagnostics DIRECTORY [--messages N --history H --payload-bytes B "
              "--fields F --capacity-scale C --workload serial|backlog|stream|process --collection none|counts|timings "
              "--history-scope same|other --idle-pollers K --role setup|producer|consumer --work-budget-ms MS "
              "--cachegrind-region none|cycle]");
    }
    Options result;
    result._directory = argv[1];
    std::vector<std::string_view> seen;
    for (int index = 2; index < argc; index += 2)
    {
      const std::string_view flag{argv[index]};
      if (index + 1 == argc || std::find(seen.begin(), seen.end(), flag) != seen.end())
      {
        invalid("Every option needs one value and may occur only once");
      }
      seen.push_back(flag);
      const std::string_view valueText{argv[index + 1]};
      if (flag == "--workload") result._workload = valueText;
      else if (flag == "--collection") result._collectionName = valueText;
      else if (flag == "--history-scope") result._historyScope = valueText;
      else if (flag == "--role") result._role = valueText;
      else if (flag == "--cachegrind-region") result._cachegrindRegion = valueText;
      else
      {
        const auto count = parse_count(valueText, flag);
        if (flag == "--messages") result._messages = count;
        else if (flag == "--history") result._history = count;
        else if (flag == "--work-budget-ms") result._workBudgetMs = count;
        else if (flag == "--payload-bytes" && count <= 4096) result._payloadBytes = static_cast<std::size_t>(count);
        else if (flag == "--fields" && count <= 512) result._fields = static_cast<std::size_t>(count);
        else if (flag == "--capacity-scale" && count <= 16) result._capacityScale = static_cast<std::uint32_t>(count);
        else if (flag == "--idle-pollers" && count <= 3) result._idlePollers = static_cast<std::size_t>(count);
        else invalid("Unknown option or value outside its supported bound");
      }
    }
    if (result._messages == 0 || result._messages > 128 || result._history > 2048 ||
        result._workBudgetMs == 0 || result._workBudgetMs > 60000 ||
        result._payloadBytes < 8 || result._fields == 0 || result._payloadBytes % result._fields != 0 ||
        (result._capacityScale != 1 && result._capacityScale != 4 && result._capacityScale != 16) ||
        (result._workload != "serial" && result._workload != "backlog" && result._workload != "stream" &&
         result._workload != "process") ||
        (result._historyScope != "same" && result._historyScope != "other") ||
        (result._idlePollers != 0 && result._workload != "stream"))
    {
      invalid("Require messages 1..128, history 0..2048, payload bytes 8..4096, Fields 1..512 dividing bytes, "
              "capacity scale 1/4/16, work budget 1..60000ms, supported workload/history scope, and idle pollers only with stream");
    }
    if ((result._workload != "process" && !result._role.empty()) ||
        (result._workload == "process" && result._role != "setup" && result._role != "producer" && result._role != "consumer"))
    {
      invalid("Process workloads require --role setup, producer, or consumer; other workloads do not use --role");
    }
    if (result._collectionName == "none") result._collection = diagnostics::Collection::None;
    else if (result._collectionName == "counts") result._collection = diagnostics::Collection::Counts;
    else if (result._collectionName == "timings") result._collection = diagnostics::Collection::Timings;
    else invalid("Unknown collection mode");
    if (!diagnostics::instrumentation_enabled && result._collection != diagnostics::Collection::None)
    {
      invalid("Counts/timings require an ANGARIA_ENABLE_DIAGNOSTICS build; use --collection none for baseline");
    }
    if (result._cachegrindRegion != "none" && result._cachegrindRegion != "cycle")
    {
      invalid("Cachegrind region must be none or cycle");
    }
    if (result._cachegrindRegion == "cycle" &&
        (!cachegrind_enabled || result._workload != "serial" ||
         result._collection != diagnostics::Collection::None || result._idlePollers != 0 || !result._role.empty()))
    {
      invalid("Cachegrind cycle requires ANGARIA_ENABLE_CACHEGRIND=ON, serial, collection none, no idle pollers or role");
    }
    return result;
  }

  struct Record
  {
    std::string_view _operation;
    std::string_view _result{"ok"};
    std::size_t _actor{};
    std::uint64_t _index{};
    std::uint64_t _startNs{};
    std::uint64_t _historyBefore{};
    std::uint64_t _historyAfter{};
    std::uint64_t _historyUpperBound{};
    diagnostics::Sample _sample;
  };

  struct Records
  {
    Records(std::size_t capacity, std::size_t actor) : _records(capacity), _actor(actor)
    {
    }

    Record& next(std::string_view operation, std::uint64_t index)
    {
      require(_used < _records.size(), "Diagnostic sample storage is exhausted");
      auto& result = _records[_used++];
      result._operation = operation;
      result._actor = _actor;
      result._index = index;
      return result;
    }

    std::vector<Record> _records;
    std::size_t _used{};
    std::size_t _actor{};
  };

  using IdleSummary = DiagnosticTotals;

  struct Workload
  {
    const Options& _options;
    SchemaId _schema;
    MessageView _outgoing;
    std::span<const std::byte> _payload;
    std::atomic<std::uint64_t> _accepted;
    Clock::time_point _start;
    std::atomic<bool> _stop{};
    std::atomic<bool> _finished{};
    std::atomic<bool> _budgetExceeded{};
    std::uint64_t _sent{};
    std::uint64_t _completed{};
    std::uint64_t _checksum{};
    std::uint64_t _unacknowledgedAtStop{};

    bool work_budget_reached()
    {
      if (_stop.load(std::memory_order_acquire))
      {
        return true;
      }
      if (Clock::now() - _start >= std::chrono::milliseconds{_options._workBudgetMs})
      {
        _budgetExceeded.store(true, std::memory_order_release);
        _stop.store(true, std::memory_order_release);
        return true;
      }
      return false;
    }

    template <typename Function>
    auto measure_record(Record& record, std::string_view operation, std::uint64_t index, Function function)
    {
      record._operation = operation;
      record._index = index;
      record._historyBefore = _accepted.load(std::memory_order_acquire);
      record._startNs = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
          Clock::now() - _start).count());
      auto result = [&]
      {
        diagnostics::Capture capture{record._sample, _options._collection};
        return function();
      }();
      record._historyAfter = _accepted.load(std::memory_order_acquire);
      // The producer records acceptance after its public send returns. A
      // concurrent receiver can observe committed data before that bookkeeping
      // store, so use the known workload ceiling as a conservative bound.
      record._historyUpperBound = (_options._workload == "stream" || _options._workload == "process")
          ? _options._history + _options._messages : record._historyAfter;
      require(record._sample._valid && !record._sample._overflow, "Diagnostic clock or counter collection failed");
      return result;
    }

    template <typename Function>
    auto measure(Records& records, std::string_view operation, std::uint64_t index, Function function)
    {
      return measure_record(records.next(operation, index), operation, index, std::move(function));
    }

    template <typename Channel>
    bool send(Channel& channel, Records* records, std::uint64_t sequence)
    {
      const bool bounded = records && (_options._workload == "stream" || _options._workload == "process");
      if (bounded && work_budget_reached())
      {
        return false;
      }
      auto issued = records ? measure(*records, "issue", sequence, [&]
      {
        return channel._producer.next_publication_id();
      }) : channel._producer.next_publication_id();
      const auto publication = value(std::move(issued));
      require(publication == PublicationId{channel._producerClaim, sequence}, "Issued identity changed");
      // ID issuance is already settled. If its wait consumed the remaining
      // budget, retain that sample but do not start a new send mutation.
      if (bounded && work_budget_reached())
      {
        return false;
      }
      std::array<DestinationOutcome, 1> destinations;
      const auto report = records ? measure(*records, "send", sequence, [&]
      {
        return channel._writer.send(publication, _outgoing, destinations);
      }) : channel._writer.send(publication, _outgoing, destinations);
      check_report(report, publication, channel._queue);
      const auto accepted = _accepted.fetch_add(1, std::memory_order_release) + 1;
      if (records)
      {
        ++_sent;
        auto& sample = records->_records[records->_used - 1];
        sample._historyAfter = accepted;
        sample._historyUpperBound = accepted;
      }
      return true;
    }

    template <typename Channel>
    bool receive(Channel& channel, Records* records, std::uint64_t sequence, std::uint64_t queueSequence = 0)
    {
      const bool bounded = records && (_options._workload == "stream" || _options._workload == "process");
      if (bounded && work_budget_reached())
      {
        return false;
      }
      auto receiveTimeout = std::chrono::duration_cast<std::chrono::nanoseconds>(5s);
      if (bounded)
      {
        const auto remaining = std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::milliseconds{_options._workBudgetMs} - (Clock::now() - _start));
        if (remaining <= 0ns)
        {
          (void)work_budget_reached();
          return false;
        }
        receiveTimeout = std::min(receiveTimeout, remaining);
      }
      if (queueSequence == 0)
      {
        queueSequence = sequence;
      }
      auto received = records ? measure(*records, "receive", sequence, [&]
      {
        return channel._source.receive(receiveTimeout);
      }) : channel._source.receive(5s);
      if (const auto* error = std::get_if<Error>(&received))
      {
        throw BenchmarkFailure{*error};
      }
      const bool timeout = std::holds_alternative<ReceiveTimeout>(received);
      if (records)
      {
        records->_records[records->_used - 1]._result = timeout ? "timeout" : "delivery";
      }
      if (timeout)
      {
        return false;
      }
      const auto* delivery = std::get_if<Delivery>(&received);
      require(delivery != nullptr, "Unexpected delivery gap in recovery workload");
      const auto& header = delivery->_message._header;
      const auto& token = delivery->_token;
      require(header._schema == _schema && header._format == PayloadFormat::Binary &&
                  header._publication == PublicationId{channel._producerClaim, sequence} &&
                  header._payloadBytes == _payload.size() && header._flags == 0 &&
                  header._queue == channel._queue && header._queueSequence == queueSequence &&
                  header._consumer == channel._consumerClaim && header._enqueuedAt.has_value() &&
                  header._dequeuedAt.has_value() && *header._enqueuedAt == header._publishedAt &&
                  token._consumer == channel._consumerClaim && token._sourceSet == channel._sourceSet &&
                  token._queue == channel._queue && token._sequence == queueSequence && token._issuance != 0,
              "Received header or token changed");
      if (_options._workload == "process" && records)
      {
        // An independent producer's bookkeeping is not shared. The received
        // publication proves at least this many post-setup accepts committed.
        const auto known = _options._history + sequence;
        _accepted.store(known, std::memory_order_release);
        records->_records[records->_used - 1]._historyAfter = known;
      }
      require(delivery->_message._payload.size() == _payload.size() &&
                  std::equal(_payload.begin(), _payload.end(), delivery->_message._payload.begin()),
              "Received payload changed");
      std::uint64_t checksum{};
      for (const auto byte : delivery->_message._payload) checksum += std::to_integer<std::uint8_t>(byte);
      if (bounded && work_budget_reached())
      {
        // The receive mutation finished and its borrow remains valid. Close
        // after the measured phase releases this exact unfinished pin; no
        // acknowledgment is started after the work budget has expired.
        _unacknowledgedAtStop = 1;
        return false;
      }
      auto acknowledged = records ? measure(*records, "acknowledge", sequence, [&]
      {
        return channel._source.acknowledge(token);
      }) : channel._source.acknowledge(token);
      value(std::move(acknowledged));
      if (records)
      {
        ++_completed;
        _checksum += checksum;
      }
      return true;
    }
  };

  void write_measurement_fields(std::ostream& output, const diagnostics::Sample& sample)
  {
    output << "\"wall_ns\":" << sample._wallNs << ",\"cpu_ns\":" << sample._cpuNs
           << ",\"cpu_available\":" << (sample._cpuAvailable ? "true" : "false")
           << ",\"valid\":" << (sample._valid ? "true" : "false")
           << ",\"overflow\":" << (sample._overflow ? "true" : "false") << ",\"phases\":[";
    bool first = true;
    for (std::size_t index = 0; index < diagnostics::phase_count; ++index)
    {
      const auto& phase = sample._phases[index];
      if (phase._calls == 0) continue;
      if (!first) output << ',';
      first = false;
      output << "{\"name\":\"" << diagnostics::phase_names[index] << "\",\"calls\":" << phase._calls
             << ",\"inclusive_ns\":" << phase._inclusiveNs << ",\"exclusive_ns\":" << phase._exclusiveNs << '}';
    }
    output << "],\"counters\":{";
    for (std::size_t index = 0; index < diagnostics::counter_count; ++index)
    {
      if (index != 0) output << ',';
      output << '\"' << diagnostics::counter_names[index] << "\":" << sample._counters[index];
    }
    output << '}';
  }

  void write_sample(std::ostream& output, const Record& record)
  {
    output << "{\"operation\":\"" << record._operation << "\",\"result\":\"" << record._result << "\",\"index\":" << record._index
           << ",\"actor\":" << record._actor << ",\"start_ns\":" << record._startNs << ",\"history_before\":" << record._historyBefore
           << ",\"history_after\":" << record._historyAfter << ",\"history_upper_bound\":" << record._historyUpperBound
           << ',';
    write_measurement_fields(output, record._sample);
    output << '}';
  }

  struct PayloadData
  {
    SchemaId _schema;
    std::vector<std::byte> _payload;

    MessageView view() const
    {
      MessageHeader header;
      header._schema = _schema;
      header._payloadBytes = _payload.size();
      return MessageView{header, _payload};
    }
  };

  PayloadData prepare_payload(Registry registry, const Options& options, bool registerDefinition)
  {
    SchemaId schema;
    if (registerDefinition)
    {
      FieldDescriptor field;
      field._id = FieldId{0xee000001};
      field._name = "Text";
      field._base = EFieldBase::BASE_CHARS;
      field._size = static_cast<std::uint32_t>(options._payloadBytes / options._fields);
      field._logical._kind = FieldLogicalKind::FixedAscii;
      const auto fieldId = value(registry.register_field(field));
      MessageDescriptor definition;
      definition._schema = SchemaId{MessageType{1}, 1};
      definition._name = "Payload";
      definition._fields.assign(options._fields, fieldId);
      schema = value(registry.register_message(definition));
    }
    else
    {
      schema = value(registry.find_message("Payload", 1));
    }
    const auto codec = value(BinaryPayloadCodec::from_registry(registry, schema, options._payloadBytes));
    require(codec.fields().size() == options._fields && codec.payload_size() == options._payloadBytes,
            "Requested payload dimensions differ from the configured schema");
    const auto width = options._payloadBytes / options._fields;
    for (const auto& field : codec.fields())
    {
      require(field._base == EFieldBase::BASE_CHARS && field._logical._kind == FieldLogicalKind::FixedAscii &&
                  field._size == width, "Configured schema is not the requested fixed-ASCII shape");
    }
    const std::vector<PayloadValue> values(options._fields, FixedAsciiValue{std::string(width, 'A')});
    const auto payload = value(codec.encode(values));
    require(payload.size() == options._payloadBytes, "Encoded payload width changed");
    return PayloadData{schema, payload};
  }

  void write_case_fields(std::ostream& output, const Options& options, std::string_view version)
  {
    output << "\"framework_version\":\"" << version << "\",\"instrumentation_enabled\":"
           << (diagnostics::instrumentation_enabled ? "true" : "false")
           << ",\"cachegrind_enabled\":" << (cachegrind_enabled ? "true" : "false")
           << ",\"cachegrind_region\":\"" << options._cachegrindRegion << '\"'
           << ",\"collection\":\"" << options._collectionName << "\",\"workload\":\"" << options._workload
           << "\",\"messages\":" << options._messages << ",\"history\":" << options._history
           << ",\"payload_bytes\":" << options._payloadBytes << ",\"fields\":" << options._fields
           << ",\"capacity_scale\":" << options._capacityScale << ",\"history_scope\":\"" << options._historyScope
           << "\",\"idle_pollers\":" << options._idlePollers << ",\"channels\":" << channel_count
           << ",\"ring_capacity\":" << ring_capacity;
    output << ",\"work_budget_ms\":" << options._workBudgetMs;
  }

  std::string framework_version(const Session& session)
  {
    const auto version = value(session.metadata())._angariaVersion;
    require(!version.empty() && version.find_first_not_of("0123456789.") == std::string::npos,
            "Invalid framework version");
    return version;
  }

  void process_barrier(std::string_view marker)
  {
    std::cout << marker << '\n' << std::flush;
    std::string line;
    require(static_cast<bool>(std::getline(std::cin, line)) && line.empty(),
            "Process coordinator must release each barrier with one empty line");
  }

  void setup_process(const Options& options)
  {
    auto created = create_session(options._directory, "Native transport diagnostics", options._capacityScale);
    const auto data = prepare_payload(created._creator.registry(), options, true);
    auto channels = register_channels(created);
    Workload workload{options, data._schema, data.view(), data._payload, 0, Clock::now()};
    const auto historyChannel = options._historyScope == "same" ? 0u : 1u;
    for (std::uint64_t sequence = 1; sequence <= options._history; ++sequence)
    {
      workload.send(channels[historyChannel], nullptr, sequence);
      require(workload.receive(channels[historyChannel], nullptr, sequence), "Process preload receive timed out");
    }
    for (auto& channel : channels)
    {
      value(channel._source.close());
    }
    const auto identity = created._session.identity();
    std::ostringstream output;
    output.exceptions(std::ios::badbit | std::ios::failbit);
    output << '{';
    write_case_fields(output, options, framework_version(created._session));
    output << ",\"role\":\"setup\",\"history_before\":" << options._history
           << ",\"history_after\":" << options._history
           << ",\"queue_sequence_before\":" << (options._historyScope == "same" ? options._history : 0)
           << ",\"file_bytes_before\":" << std::filesystem::file_size(options._directory / "session.ang")
           << ",\"session_high\":" << identity._high << ",\"session_low\":" << identity._low
           << ",\"producer_task\":" << static_cast<std::uint16_t>(channels[0]._producerClaim._task)
           << ",\"consumer_task\":" << static_cast<std::uint16_t>(channels[0]._consumerClaim._task) << "}\n";
    std::cout << output.str();
    // Returning destroys the setup's endpoints/Tasks before the coordinator
    // starts either worker. Worker attachment advances each measured role to 2.
  }

  struct ProducerEndpoint
  {
    Task _producer;
    QueueWriter _writer;
    TaskClaim _producerClaim;
    QueueId _queue;
  };

  struct ConsumerEndpoint
  {
    Task _consumer;
    SourceQueueSet _source;
    TaskClaim _producerClaim;
    TaskClaim _consumerClaim;
    QueueId _queue;
    SourceQueueSetId _sourceSet;
  };

  ProducerEndpoint open_producer(Session& session, TaskId producer, QueueId queue)
  {
    auto task = value(session.attach_task(producer));
    const auto claim = task.claim();
    require(claim._incarnation == 2, "Measured producer must replace setup's first claim exactly once");
    auto writer = value(task.open_writer(queue));
    return ProducerEndpoint{std::move(task), std::move(writer), claim, queue};
  }

  ConsumerEndpoint open_consumer(Session& session, TaskId consumer, QueueId queue,
                                    SourceQueueSetId source, TaskClaim producer)
  {
    auto task = value(session.attach_task(consumer));
    const auto claim = task.claim();
    require(claim._incarnation == 2, "Measured consumer must replace setup's first claim exactly once");
    auto reader = value(task.open_source_set(source));
    return ConsumerEndpoint{std::move(task), std::move(reader), producer, claim, queue, source};
  }

  void worker_report(const Options& options, const Session& session, TaskClaim claim, const Records& records,
                       std::uint64_t before, std::uint64_t phase, const Workload& workload)
  {
    const auto identity = session.identity();
    const bool producer = options._role == "producer";
    const bool complete = producer ? workload._sent == options._messages : workload._completed == options._messages;
    std::ostringstream output;
    output.exceptions(std::ios::badbit | std::ios::failbit);
    output << '{';
    write_case_fields(output, options, framework_version(session));
    output << ",\"role\":\"" << options._role << "\",\"clock_scope\":\"actor_relative\""
           << ",\"worker_task\":" << static_cast<std::uint16_t>(claim._task)
           << ",\"worker_incarnation\":" << claim._incarnation
           << ",\"session_high\":" << identity._high << ",\"session_low\":" << identity._low
           << ",\"history_bounds_exact\":false,\"history_before\":" << options._history
           << ",\"history_after\":" << workload._accepted.load(std::memory_order_acquire)
           << ",\"file_bytes_before\":" << before
           << ",\"file_bytes_after\":" << std::filesystem::file_size(options._directory / "session.ang")
           << ",\"work_phase_ns\":" << phase
           << ",\"outcome\":\"" << (complete ? "complete" : "budget_exceeded") << '\"'
           << ",\"sent\":" << workload._sent
           << ",\"completed\":" << workload._completed << ",\"checksum\":" << workload._checksum
           << ",\"unacknowledged_at_stop\":" << workload._unacknowledgedAtStop
           << ",\"controls\":{\"status\":" << (producer && workload._sent != 0 ? "true" : "false")
           << ",\"identical_retry\":" << (producer && workload._sent != 0 ? "true" : "false")
           << ",\"empty_receive\":" << (!producer && workload._completed == options._messages ? "true" : "false") << '}'
           << ",\"samples\":[";
    for (std::size_t index = 0; index < records._used; ++index)
    {
      if (index != 0)
      {
        output << ',';
      }
      write_sample(output, records._records[index]);
    }
    output << "]}\n";
    std::cout << output.str();
  }

  void run_process_worker(const Options& options)
  {
    auto session = value(Session::attach(options._directory));
    auto registry = session.registry();
    const auto actual = value(session.options());
    require(actual._limits._tasks == 9 * options._capacityScale && actual._limits._queues == 4 * options._capacityScale &&
                actual._limits._sourceSets == 4 * options._capacityScale &&
                actual._limits._fields == options._capacityScale && actual._limits._messageSchemas == options._capacityScale &&
                actual._limits._membershipsPerSet == 1 && actual._maxLogBytes == 64ULL * 1024 * 1024,
            "Process worker capacity options differ from the prepared Session");
    const auto data = prepare_payload(registry, options, false);
    const auto queue = value(registry.find_queue("Queue1"));
    const auto source = value(registry.find_source_set("Inputs1"));
    const auto producer = value(registry.find_task("Producer1"));
    const auto consumer = value(registry.find_task("Consumer1"));
    const auto queueDefinition = value(registry.describe_queue(queue));
    require(queueDefinition._producer == producer && queueDefinition._sourceSet == source &&
                queueDefinition._ringCapacity == ring_capacity,
            "Process worker topology differs from the prepared channel");
    Workload workload{options, data._schema, data.view(), data._payload, options._history, Clock::now()};
    const auto before = std::filesystem::file_size(options._directory / "session.ang");
    if (options._role == "producer")
    {
      auto endpoint = open_producer(session, producer, queue);
      const auto claim = endpoint._producerClaim;
      Records records(2 * options._messages + 2, 0);
      process_barrier("READY");
      workload._start = Clock::now();
      for (std::uint64_t sequence = 1; sequence <= options._messages; ++sequence)
      {
        if (!workload.send(endpoint, &records, sequence))
        {
          break;
        }
      }
      const auto phase = elapsed_ns(workload._start, Clock::now());
      process_barrier("DONE");
      if (workload._sent != 0)
      {
        const PublicationId last{claim, workload._sent};
        std::array<DestinationOutcome, 1> destinations;
        const auto status = workload.measure(records, "status", last._sequence, [&]
        {
          return endpoint._producer.publication_status(last, destinations);
        });
        check_report(status, last, queue);
        records._records[records._used - 1]._result = "accepted";
        const auto retry = workload.measure(records, "identical_retry", last._sequence, [&]
        {
          return endpoint._writer.send(last, workload._outgoing, destinations);
        });
        check_report(retry, last, queue);
        records._records[records._used - 1]._result = "accepted";
      }
      require(workload._sent == options._messages || workload._budgetExceeded.load(),
              "Process producer stopped without exhausting its budget");
      worker_report(options, session, claim, records, before, phase, workload);
    }
    else
    {
      const TaskClaim producerClaim{session.identity(), producer, 2};
      auto endpoint = open_consumer(session, consumer, queue, source, producerClaim);
      const auto claim = endpoint._consumerClaim;
      Records records(2 * options._messages + 17, 1);
      process_barrier("READY");
      workload._start = Clock::now();
      const auto offset = options._historyScope == "same" ? options._history : 0;
      for (std::uint64_t sequence = 1; sequence <= options._messages && !workload.work_budget_reached();)
      {
        if (workload.receive(endpoint, &records, sequence, offset + sequence))
        {
          ++sequence;
        }
      }
      const auto phase = elapsed_ns(workload._start, Clock::now());
      process_barrier("DONE");
      const auto observed = value(registry.task_status(producer));
      require(observed._task == producer && observed._incarnation == producerClaim._incarnation,
              "Process producer identity changed during the measurement");
      if (workload._completed == options._messages)
      {
        auto empty = workload.measure(records, "empty_receive", 1, [&]
        {
          return endpoint._source.receive();
        });
        if (const auto* error = std::get_if<Error>(&empty))
        {
          throw BenchmarkFailure{*error};
        }
        require(std::holds_alternative<ReceiveTimeout>(empty), "Process consumer left an unexpected delivery");
        records._records[records._used - 1]._result = "timeout";
      }
      require((workload._completed == options._messages || workload._budgetExceeded.load()) &&
                  workload._checksum == workload._completed * options._payloadBytes * 65,
              "Process completion count or payload checksum changed");
      value(endpoint._source.close());
      worker_report(options, session, claim, records, before, phase, workload);
    }
  }

  void run(const Options& options)
  {
    auto created = create_session(options._directory, "Native transport diagnostics", options._capacityScale);
    const auto data = prepare_payload(created._creator.registry(), options, true);
    const auto& payload = data._payload;
    const auto schema = data._schema;
    auto channels = register_channels(created);
    Workload workload{options, schema, data.view(), payload, 0, Clock::now()};
    const auto historyChannel = options._historyScope == "same" ? 0u : 1u;
    for (std::uint64_t sequence = 1; sequence <= options._history; ++sequence)
    {
      workload.send(channels[historyChannel], nullptr, sequence);
      require(workload.receive(channels[historyChannel], nullptr, sequence), "Preload receive timed out");
    }
    const auto offset = options._historyScope == "same" ? options._history : 0;
    const auto file = options._directory / "session.ang";
    const auto before = std::filesystem::file_size(file);
    Records producer(2 * options._messages + 3, 0);
    Records consumer(2 * options._messages + 17, 1);
    std::vector<Records> idle;
    std::vector<IdleSummary> idleSummaries(options._idlePollers);
    idle.reserve(options._idlePollers);
    for (std::size_t index = 0; index < options._idlePollers; ++index)
    {
      idle.emplace_back(16 * options._messages + 64, index + 2);
    }
    std::optional<ProducerEndpoint> streamProducer;
    std::optional<ConsumerEndpoint> streamConsumer;
    std::vector<ConsumerEndpoint> idleConsumers;
    if (options._workload == "stream")
    {
      // Match the independent-process cohort: setup relinquishes every claim,
      // then only the measured producer/consumer and requested idle consumers
      // attach. Registration, history, and Queue sequences remain unchanged.
      struct Identity
      {
        TaskClaim _producer;
        TaskId _consumer;
        QueueId _queue;
        SourceQueueSetId _source;
      };
      std::array<Identity, channel_count> identities;
      for (std::size_t index = 0; index < channels.size(); ++index)
      {
        auto& channel = channels[index];
        identities[index] = {channel._producerClaim, channel._consumerClaim._task, channel._queue, channel._sourceSet};
        value(channel._source.close());
      }
      channels.clear();
      {
        [[maybe_unused]] auto releasedCreator = std::move(created._creator);
      }
      const auto& measured = identities[0];
      streamProducer.emplace(open_producer(created._session, measured._producer._task, measured._queue));
      streamConsumer.emplace(open_consumer(created._session, measured._consumer, measured._queue,
                                           measured._source, streamProducer->_producerClaim));
      idleConsumers.reserve(options._idlePollers);
      for (std::size_t index = 0; index < options._idlePollers; ++index)
      {
        const auto& identity = identities[index + 1];
        idleConsumers.push_back(open_consumer(created._session, identity._consumer, identity._queue,
                                               identity._source, identity._producer));
      }
    }
    workload._start = Clock::now();
    std::uint64_t publishPhase{};
    std::uint64_t drainPhase{};
    std::uint64_t cyclePhase{};
    std::uint64_t afterPublish{};
    if (options._workload == "stream")
    {
      cyclePhase = run_workers(2 + options._idlePollers, [&](std::size_t actor)
      {
        try
        {
          if (actor == 0)
          {
            for (std::uint64_t index = 1; index <= options._messages; ++index)
            {
              if (!workload.send(*streamProducer, &producer, index))
              {
                break;
              }
            }
          }
          else if (actor == 1)
          {
            for (std::uint64_t index = 1; index <= options._messages && !workload.work_budget_reached();)
            {
              if (workload.receive(*streamConsumer, &consumer, index, offset + index)) ++index;
            }
            workload._finished.store(true, std::memory_order_release);
          }
          else
          {
            auto& records = idle[actor - 2];
            auto& source = idleConsumers[actor - 2]._source;
            auto& summary = idleSummaries[actor - 2];
            Record overflowRecord;
            overflowRecord._actor = actor;
            for (std::uint64_t index = 1; !workload._finished.load(std::memory_order_acquire) &&
                 !workload.work_budget_reached(); ++index)
            {
              const auto remaining = std::chrono::duration_cast<std::chrono::nanoseconds>(
                  std::chrono::milliseconds{options._workBudgetMs} - (Clock::now() - workload._start));
              if (remaining <= 0ns)
              {
                (void)workload.work_budget_reached();
                break;
              }
              const auto timeout = std::min(std::chrono::duration_cast<std::chrono::nanoseconds>(1ms), remaining);
              // Retain a bounded prefix of raw polls, then reuse one scratch
              // sample while continuing to poll and aggregate every call.
              auto& record = records._used < records._records.size()
                  ? records.next("idle_empty_receive", index) : overflowRecord;
              auto result = workload.measure_record(record, "idle_empty_receive", index, [&]
              {
                return source.receive(timeout);
              });
              if (const auto* error = std::get_if<Error>(&result))
              {
                throw BenchmarkFailure{*error};
              }
              require(std::holds_alternative<ReceiveTimeout>(result), "Idle Source received unexpected work");
              record._result = "timeout";
              summary.add(record._sample);
              require(summary._sample._valid && !summary._sample._overflow, "Idle diagnostic aggregate overflowed");
            }
          }
        }
        catch (...)
        {
          workload._stop.store(true, std::memory_order_release);
          throw;
        }
      }, &workload._start);
      afterPublish = std::filesystem::file_size(file);
    }
    else if (options._workload == "backlog")
    {
      auto start = Clock::now();
      for (std::uint64_t index = 1; index <= options._messages; ++index)
      {
        workload.send(channels[0], &producer, offset + index);
      }
      publishPhase = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - start).count());
      afterPublish = std::filesystem::file_size(file);
      start = Clock::now();
      for (std::uint64_t index = 1; index <= options._messages; ++index)
      {
        require(workload.receive(channels[0], &consumer, offset + index), "Backlog receive timed out");
      }
      drainPhase = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - start).count());
    }
    else
    {
      const auto start = Clock::now();
      {
        // Setup, retained-history preload and sample allocation have ended.
        // The region ends before status/retry controls, output and Session teardown.
#if defined(ANGARIA_ENABLE_CACHEGRIND) && ANGARIA_ENABLE_CACHEGRIND
        const CachegrindRegion region{options._cachegrindRegion == "cycle"};
#endif
        for (std::uint64_t index = 1; index <= options._messages; ++index)
        {
          workload.send(channels[0], &producer, offset + index);
          require(workload.receive(channels[0], &consumer, offset + index), "Serial receive timed out");
        }
      }
      cyclePhase = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - start).count());
      afterPublish = std::filesystem::file_size(file);
    }
    const auto afterDrain = std::filesystem::file_size(file);
    const auto accepted = workload._sent;
    const bool complete = workload._completed == options._messages && accepted == options._messages;
    require(afterPublish == afterDrain && afterDrain >= before && ((accepted == 0) == (afterDrain == before)),
            "Draining changed physical append accounting");
    require(workload._completed <= accepted && accepted <= options._messages &&
                workload._completed + workload._unacknowledgedAtStop <= accepted &&
                (complete || workload._budgetExceeded.load()) &&
                workload._checksum == workload._completed * options._payloadBytes * 65,
            "Measured completion or checksum changed");
    auto& producerTask = streamProducer ? streamProducer->_producer : channels[0]._producer;
    auto& writer = streamProducer ? streamProducer->_writer : channels[0]._writer;
    auto& reader = streamConsumer ? streamConsumer->_source : channels[0]._source;
    const auto producerClaim = streamProducer ? streamProducer->_producerClaim : channels[0]._producerClaim;
    const auto consumerClaim = streamConsumer ? streamConsumer->_consumerClaim : channels[0]._consumerClaim;
    const auto queue = streamProducer ? streamProducer->_queue : channels[0]._queue;
    if (accepted != 0)
    {
      const PublicationId last{producerClaim, (streamProducer ? 0 : offset) + accepted};
      std::array<DestinationOutcome, 1> destinations;
      const auto status = workload.measure(producer, "status", last._sequence, [&]
      {
        return producerTask.publication_status(last, destinations);
      });
      check_report(status, last, queue);
      producer._records[producer._used - 1]._result = "accepted";
      const auto repeated = workload.measure(producer, "identical_retry", last._sequence, [&]
      {
        return writer.send(last, workload._outgoing, destinations);
      });
      check_report(repeated, last, queue);
      producer._records[producer._used - 1]._result = "accepted";
    }
    if (workload._completed == accepted)
    {
      auto empty = workload.measure(consumer, "empty_receive", 1, [&]
      {
        return reader.receive();
      });
      if (const auto* error = std::get_if<Error>(&empty))
      {
        throw BenchmarkFailure{*error};
      }
      require(std::holds_alternative<ReceiveTimeout>(empty), "Unexpected delivery remained after draining");
      consumer._records[consumer._used - 1]._result = "timeout";
    }
    require(std::filesystem::file_size(file) == afterDrain, "Lookup/retry/empty poll unexpectedly appended bytes");
    for (auto& channel : channels) value(channel._source.close());
    if (streamConsumer)
    {
      value(streamConsumer->_source.close());
    }
    for (auto& endpoint : idleConsumers)
    {
      value(endpoint._source.close());
    }
    const auto version = value(created._session.metadata())._angariaVersion;
    require(!version.empty() && version.find_first_not_of("0123456789.") == std::string::npos, "Invalid framework version");
    std::vector<const Record*> ordered;
    for (const auto* records : {&producer, &consumer})
    {
      for (std::size_t index = 0; index < records->_used; ++index) ordered.push_back(&records->_records[index]);
    }
    for (const auto& records : idle)
    {
      for (std::size_t index = 0; index < records._used; ++index) ordered.push_back(&records._records[index]);
    }
    std::sort(ordered.begin(), ordered.end(), [](const Record* first, const Record* second)
    {
      return first->_startNs < second->_startNs;
    });
    std::ostringstream output;
    output.exceptions(std::ios::badbit | std::ios::failbit);
    output << '{';
    write_case_fields(output, options, version);
    output << ",\"clock_scope\":\"process_relative\",\"producer_incarnation\":" << producerClaim._incarnation
           << ",\"consumer_incarnation\":" << consumerClaim._incarnation
           << ",\"active_claims\":" << (streamProducer ? 2 + options._idlePollers : 9)
           << ",\"history_before\":" << options._history
           << ",\"history_after\":" << options._history + accepted
           << ",\"history_bounds_exact\":" << (options._workload == "stream" ? "false" : "true")
           << ",\"file_bytes_before\":" << before << ",\"file_bytes_after_publish\":" << afterPublish
           << ",\"file_bytes_after_drain\":" << afterDrain << ",\"append_bytes\":" << afterDrain - before
           << ",\"publish_phase_ns\":" << publishPhase << ",\"drain_phase_ns\":" << drainPhase
           << ",\"cycle_phase_ns\":" << cyclePhase
           << ",\"outcome\":\"" << (complete ? "complete" : "budget_exceeded") << '\"'
           << ",\"accepted\":" << accepted << ",\"completed\":" << workload._completed
           << ",\"pending\":" << accepted - workload._completed
           << ",\"unacknowledged_at_stop\":" << workload._unacknowledgedAtStop
           << ",\"controls\":{\"status\":" << (accepted != 0 ? "true" : "false")
           << ",\"identical_retry\":" << (accepted != 0 ? "true" : "false")
           << ",\"empty_receive\":" << (workload._completed == accepted ? "true" : "false") << '}'
           << ",\"checksum\":" << workload._checksum << ",\"samples\":[";
    for (std::size_t index = 0; index < ordered.size(); ++index)
    {
      if (index != 0) output << ',';
      write_sample(output, *ordered[index]);
    }
    output << "],\"idle_pollers_summary\":[";
    for (std::size_t index = 0; index < idleSummaries.size(); ++index)
    {
      if (index != 0)
      {
        output << ',';
      }
      const auto& summary = idleSummaries[index];
      const auto captured = idle[index]._used;
      output << "{\"actor\":" << index + 2 << ",\"polls\":" << summary._calls
             << ",\"captured_samples\":" << captured << ",\"unrecorded_samples\":" << summary._calls - captured
             << ",\"raw_truncated\":" << (summary._calls > captured ? "true" : "false")
             << ",\"cpu_all_available\":" << (summary._sample._cpuAvailable ? "true" : "false")
             << ",\"aggregate\":{";
      write_measurement_fields(output, summary._sample);
      output << "}}";
    }
    output << "]}\n";
    std::cout << output.str();
  }
}

int main(int argc, char** argv)
{
  try
  {
    const auto options = parse_options(argc, argv);
    if (options._workload == "process")
    {
      if (options._role == "setup")
      {
        setup_process(options);
      }
      else
      {
        run_process_worker(options);
      }
    }
    else
    {
      run(options);
    }
    return 0;
  }
  catch (const angaria::benchmarks::BenchmarkFailure& failure)
  {
    std::cerr << angaria::detail::error_name(failure._error._code) << " [" << failure._error._path
              << "]: " << failure._error._message << '\n';
  }
  catch (const std::exception& error)
  {
    std::cerr << "diagnostic: " << error.what() << '\n';
  }
  return 1;
}
