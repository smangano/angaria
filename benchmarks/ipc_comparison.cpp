// Independent-process adapters for the bounded 1:1 IPC comparison. Control
// traffic uses stdin/stdout; application messages use only the selected library.
#include "ipc_comparison_support.h"
#include "ipc_operation_diagnostics.h"
#if defined(ANGARIA_BUILD_STALL_STUDY) && ANGARIA_BUILD_STALL_STUDY
#include "recorded_spsc.h"
#endif
#include "build_version.h"

#include <nanomsg/nn.h>
#include <nanomsg/pipeline.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <limits>
#include <optional>
#include <sstream>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include <unistd.h>

namespace
{
  using namespace angaria;
  using namespace angaria::benchmarks;
  namespace ipc = angaria::benchmarks::ipc;
  constexpr std::uint64_t log_limit = 64ULL * 1024 * 1024;

  std::array<int, 3> runtime_abi()
  {
    const std::array<std::string_view, 3> names{
        "NN_VERSION_CURRENT", "NN_VERSION_REVISION", "NN_VERSION_AGE"};
    std::array<int, 3> values{-1, -1, -1};
    for (int index = 0; index < 4096; ++index)
    {
      int value{};
      const auto* name = ::nn_symbol(index, &value);
      if (!name)
      {
        break;
      }
      for (std::size_t entry = 0; entry < names.size(); ++entry)
      {
        if (name == names[entry])
        {
          values[entry] = value;
        }
      }
    }
    require(values == std::array{NN_VERSION_CURRENT, NN_VERSION_REVISION, NN_VERSION_AGE},
            "Runtime nanomsg ABI constants differ from the compiled headers");
    return values;
  }

  std::string abi_fields()
  {
    const auto values = runtime_abi();
    return "\"nanomsg_runtime_abi_current\":" + std::to_string(values[0]) +
        ",\"nanomsg_runtime_abi_revision\":" + std::to_string(values[1]) +
        ",\"nanomsg_runtime_abi_age\":" + std::to_string(values[2]) +
        ",\"nanomsg_header_abi_current\":" + std::to_string(NN_VERSION_CURRENT) +
        ",\"nanomsg_header_abi_revision\":" + std::to_string(NN_VERSION_REVISION) +
        ",\"nanomsg_header_abi_age\":" + std::to_string(NN_VERSION_AGE);
  }

  std::string current_error()
  {
    try
    {
      throw;
    }
    catch (const BenchmarkFailure& failure)
    {
      return failure._error._path + ": " + failure._error._message +
          " (code " + std::to_string(static_cast<unsigned>(failure._error._code)) + ")";
    }
    catch (const std::exception& failure)
    {
      return failure.what();
    }
    catch (...)
    {
      return "Unexpected IPC comparison failure";
    }
  }

  std::uint64_t file_bytes(const ipc::Options& options)
  {
    return options._profile != ipc::Profile::Nanomsg ?
        std::filesystem::file_size(options._directory / "session.ang") : 0;
  }

  std::string fields(const ipc::Options& options, bool includeRole = true)
  {
    auto result = "\"profile\":" + ipc::json_string(ipc::profile_name(options._profile)) +
        ",\"scenario\":" + ipc::json_string(options._scenario) +
        ",\"collection\":" + ipc::json_string(options._collectionName) +
        ",\"instrumentation_enabled\":" + (ipc::observation::instrumentation_enabled ? "true" : "false");
    if (includeRole)
    {
      result += ",\"role\":" + ipc::json_string(ipc::role_name(options._role));
    }
    return result;
  }

  void event(const ipc::Options& options, std::string_view name, std::string extra = {})
  {
    auto members = fields(options, false);
    if (!extra.empty())
    {
      members += ',' + extra;
    }
    ipc::emit_control(name, options._role, members);
  }

  class Socket
  {
  public:
    explicit Socket(const ipc::Options& options)
    {
      _fd = ::nn_socket(AF_SP, options._role == ipc::Role::Producer ? NN_PUSH : NN_PULL);
      require(_fd >= 0, "Cannot create nanomsg socket");
      try
      {
        set_option(NN_LINGER, 0);
        set_option(NN_SNDTIMEO, 10);
        set_option(NN_RCVTIMEO, 10);
        set_option(NN_SNDBUF, static_cast<int>(options._socketBufferBytes));
        set_option(NN_RCVBUF, static_cast<int>(options._socketBufferBytes));
        _sendBuffer = get_option(NN_SNDBUF);
        _receiveBuffer = get_option(NN_RCVBUF);
        const auto address = "ipc://" + (options._directory / "data.ipc").string();
        const auto endpoint = options._role == ipc::Role::Consumer ?
            ::nn_bind(_fd, address.c_str()) : ::nn_connect(_fd, address.c_str());
        if (endpoint < 0)
        {
          // Capture the native result before allocation, diagnostics or close
          // can change errno. Retain the endpoint to distinguish access and
          // address failures without changing the execution environment.
          const auto error = ::nn_errno();
          const auto* operation = options._role == ipc::Role::Consumer ? "nn_bind" : "nn_connect";
          throw BenchmarkFailure{Error{ErrorCode::IoError,
              std::string{operation} + " failed for " + address + ": " + ::nn_strerror(error) +
                  " (nn_errno=" + std::to_string(error) + ")", "benchmark.nanomsg"}};
        }
        // connect() alone does not prove that PUSH has an available pipe.
        // This readiness wait sends no hidden warmup message at H=0.
        if (options._role == ipc::Role::Producer)
        {
          wait(NN_POLLOUT, ipc::deadline_ns(ipc::monotonic_ns(), options._budgetMs));
        }
      }
      catch (...)
      {
        ::nn_close(_fd);
        _fd = -1;
        throw;
      }
    }

    ~Socket()
    {
      if (_fd >= 0)
      {
        ::nn_close(_fd);
      }
    }

    Socket(const Socket&) = delete;
    Socket& operator=(const Socket&) = delete;

    void wait(short requested, std::uint64_t deadline) const
    {
      for (;;)
      {
        nn_pollfd descriptor{_fd, requested, 0};
        const auto result = ::nn_poll(&descriptor, 1, std::min(10, ipc::remaining_ms(deadline)));
        if (result < 0 && ::nn_errno() == EINTR)
        {
          continue;
        }
        require(result >= 0, "nanomsg poll failed");
        if (result > 0 && (descriptor.revents & requested) != 0)
        {
          return;
        }
        ipc::check_deadline(deadline);
      }
    }

    int _fd{-1};
    int _sendBuffer{};
    int _receiveBuffer{};

  private:
    void set_option(int option, int setting) const
    {
      require(::nn_setsockopt(_fd, NN_SOL_SOCKET, option, &setting, sizeof(setting)) == 0,
              "Cannot configure nanomsg socket");
    }

    int get_option(int option) const
    {
      int setting{};
      std::size_t bytes = sizeof(setting);
      require(::nn_getsockopt(_fd, NN_SOL_SOCKET, option, &setting, &bytes) == 0 && bytes == sizeof(setting),
              "Cannot inspect nanomsg socket option");
      return setting;
    }
  };

  struct AngariaEndpoint
  {
    explicit AngariaEndpoint(const ipc::Options& options)
        : _session(value(Session::attach(options._directory)))
    {
      auto registry = _session.registry();
      _schema = value(registry.find_message("ComparisonPayload", 1));
      _queue = value(registry.find_queue("Connection"));
      _sourceId = value(registry.find_source_set("Input"));
      _producerId = value(registry.find_task("Producer"));
      const auto fieldId = value(registry.find_field("Word"));
      const auto field = value(registry.describe_field(fieldId));
      const auto definition = value(registry.describe_message(_schema));
      require(field._base == EFieldBase::BASE_UINT64 && field._size == 8 &&
                  field._logical._kind == FieldLogicalKind::None,
              "Session payload Field must accept an arbitrary unsigned 64-bit word");
      require(definition._fields.size() == options._payloadBytes / 8 &&
                  std::all_of(definition._fields.begin(), definition._fields.end(),
                      [fieldId](FieldId entry)
                      {
                        return entry == fieldId;
                      }),
              "Session payload schema does not match the requested word count");
      _task.emplace(value(_session.attach_task(options._role == ipc::Role::Producer ? "Producer" : "Consumer")));
      // claim() authenticates through RuntimeAccess. Capture the immutable
      // expected identity before timing; verification below only compares it.
      _claim = _task->claim();
      if (options._role == ipc::Role::Producer)
      {
        _writer.emplace(value(_task->open_writer(_queue)));
      }
      else if (options._profile == ipc::Profile::Angaria)
      {
        _source.emplace(value(_task->open_source_set(_sourceId)));
      }
    }

#if defined(ANGARIA_BUILD_STALL_STUDY) && ANGARIA_BUILD_STALL_STUDY
    spsc::Options spsc_options(const ipc::Options& options) const
    {
      spsc::Options result;
      result._directory = options._directory;
      result._session = _session.identity();
      result._queue = _queue;
      result._producer = _producerId;
      result._schema = _schema;
      result._payloadBytes = options._payloadBytes;
      result._capacity = options._ringSlots;
      result._maxLogBytes = log_limit;
      return result;
    }
#endif

    Session _session;
    SchemaId _schema{};
    QueueId _queue{};
    SourceQueueSetId _sourceId{};
    TaskId _producerId{};
    TaskClaim _claim{};
    std::optional<Task> _task;
    std::optional<QueueWriter> _writer;
    std::optional<SourceQueueSet> _source;
  };

  void setup(const ipc::Options& options)
  {
    if (options._profile != ipc::Profile::Nanomsg)
    {
      // All setup and history claims die before independent measured roles
      // attach. The retained H Publications have been verified and acknowledged.
      SessionOptions configuration;
      configuration._name = "IPC comparison";
      configuration._applicationVersion = "1";
      configuration._directory = options._directory;
      configuration._maxLogBytes = log_limit;
      configuration._limits = CapacityLimits::empty().tasks(3).queues(1).source_sets(1)
          .memberships_per_set(1).fields(1).message_schemas(1);
      configuration._defaults._ringCapacity = options._ringSlots;
      TaskDescriptor creator;
      creator._name = "Setup";
      auto created = value(Session::create(configuration, creator));
      auto registry = created._creator.registry();
      TaskDescriptor producerDefinition;
      producerDefinition._name = "Producer";
      const auto producerId = value(registry.register_task(producerDefinition));
      TaskDescriptor consumerDefinition;
      consumerDefinition._name = "Consumer";
      const auto consumerId = value(registry.register_task(consumerDefinition));
      SourceQueueSetDescriptor sourceDefinition;
      sourceDefinition._name = "Input";
      sourceDefinition._owner = consumerId;
      const auto sourceId = value(registry.register_source_set(sourceDefinition));
      QueueDescriptor queueDefinition;
      queueDefinition._name = "Connection";
      queueDefinition._producer = producerId;
      queueDefinition._sourceSet = sourceId;
      const auto queueId = value(registry.register_queue(queueDefinition));
      value(registry.activate_source_set(sourceId));
      value(registry.activate_queue(queueId));
      FieldDescriptor field;
      field._id = FieldId{0xee000001};
      field._name = "Word";
      field._base = EFieldBase::BASE_UINT64;
      field._size = 8;
      // Every 64-bit pattern is valid. Repetition covers the arbitrary binary
      // envelope/body exactly, without text constraints or re-encoding bytes.
      const auto fieldId = value(registry.register_field(field));
      MessageDescriptor definition;
      definition._schema = SchemaId{MessageType{1}, 1};
      definition._name = "ComparisonPayload";
      definition._fields.assign(options._payloadBytes / 8, fieldId);
      const auto schema = value(registry.register_message(definition));
      auto producer = value(created._session.attach_task(producerId));
      auto consumer = value(created._session.attach_task(consumerId));
      auto writer = value(producer.open_writer(queueId));
      auto source = value(consumer.open_source_set(sourceId));
      std::vector<std::byte> payload(options._payloadBytes);
      MessageHeader header;
      header._schema = schema;
      header._payloadBytes = payload.size();
      const auto deadline = ipc::deadline_ns(ipc::monotonic_ns(), std::max<std::uint64_t>(10'000, options._budgetMs));
      for (std::uint64_t sequence = 1; sequence <= options._history; ++sequence)
      {
        ipc::check_deadline(deadline);
        ipc::fill_payload(payload, sequence, 0, 0);
        const auto publication = value(producer.next_publication_id());
        std::array<DestinationOutcome, 1> outcomes;
        check_report(writer.send(publication, MessageView{header, payload}, outcomes), publication, queueId);
        auto result = source.receive(std::chrono::milliseconds{ipc::remaining_ms(deadline)});
        if (const auto* error = std::get_if<Error>(&result))
        {
          throw BenchmarkFailure{*error};
        }
        const auto* delivery = std::get_if<Delivery>(&result);
        require(delivery != nullptr, "Angaria history did not produce its expected delivery");
        const auto envelope = ipc::validate_payload(delivery->_message._payload, sequence);
        require(envelope._scheduledNs == 0 && envelope._logicalStartNs == 0, "Preload timestamps must be zero");
        value(source.acknowledge(delivery->_token));
      }
      value(source.close());
#if defined(ANGARIA_BUILD_STALL_STUDY) && ANGARIA_BUILD_STALL_STUDY
      if (options._profile == ipc::Profile::RecordedSpsc)
      {
        spsc::Options connection;
        connection._directory = options._directory;
        connection._session = created._session.identity();
        connection._queue = queueId;
        connection._producer = producerId;
        connection._schema = schema;
        connection._payloadBytes = options._payloadBytes;
        connection._capacity = options._ringSlots;
        connection._maxLogBytes = log_limit;
        spsc::RecordedSpsc::create(connection);
      }
#endif
    }
    else
    {
      require(std::filesystem::create_directory(options._directory), "IPC comparison directory already exists");
    }
    event(options, "SETUP", "\"format_version\":1,\"framework_version\":" +
        ipc::json_string(angaria::detail::framework_version) +
        ",\"nanomsg_version\":" + ipc::json_string(ANGARIA_NANOMSG_VERSION) +
        ",\"nanomsg_commit\":" + ipc::json_string(ANGARIA_NANOMSG_COMMIT) +
        ",\"nanomsg_archive_sha256\":" + ipc::json_string(ANGARIA_NANOMSG_ARCHIVE_SHA256) +
        ',' + abi_fields() + ",\"pid\":" + std::to_string(::getpid()) +
        ",\"messages\":" + std::to_string(options._messages) +
        ",\"history\":" + std::to_string(options._history) +
        ",\"payload_bytes\":" + std::to_string(options._payloadBytes) +
        ",\"clock_id\":\"CLOCK_MONOTONIC\",\"clock_resolution_ns\":" +
        std::to_string(ipc::clock_resolution_ns()) +
        ",\"history_count\":" + std::to_string(options._history) +
        ",\"file_bytes\":" + std::to_string(file_bytes(options)));
  }

  struct Sample
  {
    std::uint64_t _sequence{};
    std::uint64_t _scheduled{};
    std::uint64_t _logicalStart{};
    std::uint64_t _idStart{};
    std::uint64_t _idEnd{};
    std::uint64_t _sendStart{};
    std::uint64_t _sendEnd{};
    std::uint64_t _sendApi{};
    std::uint64_t _sendAttempts{};
    std::uint64_t _successfulSendStart{};
    std::uint64_t _backpressure{};
    std::uint64_t _creditWaits{};
    std::uint64_t _receiveStart{};
    std::uint64_t _receive{};
    std::uint64_t _receiveApi{};
    std::uint64_t _receiveAttempts{};
    std::uint64_t _timeouts{};
    std::uint64_t _successfulReceiveStart{};
    std::uint64_t _verified{};
    std::uint64_t _ackStart{};
    std::uint64_t _ackEnd{};
    std::uint64_t _hash{};
    std::uint64_t _publicationSequence{};
    std::uint64_t _publicationIncarnation{};
    bool _accepted{};
  };

  class Worker
  {
  public:
    explicit Worker(ipc::Options options)
        : _options(std::move(options)), _payload(_options._payloadBytes), _samples(_options._messages)
    {
      if (_options._profile != ipc::Profile::Nanomsg)
      {
        _angaria.emplace(_options);
#if defined(ANGARIA_BUILD_STALL_STUDY) && ANGARIA_BUILD_STALL_STUDY
        if (_options._profile == ipc::Profile::RecordedSpsc)
        {
          _spsc.emplace(_angaria->spsc_options(_options),
              _options._role == ipc::Role::Producer ? spsc::Role::Producer : spsc::Role::Consumer,
              _angaria->_claim);
        }
#endif
      }
      else
      {
        _socket.emplace(_options);
      }
      calibrate_clock();
    }

    bool run()
    {
      event(_options, "CONNECTED");
      auto command = ipc::read_command();
      while (command._cmd == "CLOCK")
      {
        const auto now = ipc::monotonic_ns();
        event(_options, "CLOCK", "\"nonce\":" + std::to_string(command._nonce) +
            ",\"clock_ns\":" + std::to_string(now) + ",\"clock_id\":\"CLOCK_MONOTONIC\"" +
            ",\"clock_resolution_ns\":" + std::to_string(ipc::clock_resolution_ns()) +
            ",\"pid\":" + std::to_string(::getpid()));
        command = ipc::read_command();
      }
      require(command._cmd == "PRELOAD", "Expected PRELOAD after clock probes");
      preload();
      event(_options, "DRAINED", "\"history_count\":" + std::to_string(_options._history));
      require(ipc::read_command()._cmd == "READY", "Expected READY after drained preload");
      _fileBefore = file_bytes(_options);
      _threadsStart = ipc::thread_count();
      event(_options, "READY");
      command = ipc::read_command();
      require(command._cmd == "START", "Expected START after READY");
      measured(command._t0Ns);
      event(_options, "DONE", "\"status\":" + ipc::json_string(_status) +
          ",\"data_start_ns\":" + std::to_string(_dataStart) +
          ",\"data_end_ns\":" + std::to_string(_dataEnd));
      require(ipc::read_command()._cmd == "FINISH", "Expected FINISH after DONE");
      // PUSH may return while nanomsg background threads still deliver bytes.
      // Keep process CPU accounting open until both roles have reached DONE.
      _resourceEnd = ipc::monotonic_ns();
      _cpuEnd = ipc::process_cpu_ns();
      check_extra_messages();
      _fileAfter = file_bytes(_options);
      _threadsEnd = ipc::thread_count();
      report();
      if (_angaria && _angaria->_source)
      {
        value(_angaria->_source->close());
      }
      return _status == "complete";
    }

  private:
    void calibrate_clock()
    {
      for (std::size_t index = 0; index < 16; ++index)
      {
        (void)ipc::monotonic_ns();
      }
      std::array<std::uint64_t, 256> pairs{};
      for (auto& delta : pairs)
      {
        const auto first = ipc::monotonic_ns();
        delta = ipc::monotonic_ns() - first;
      }
      std::sort(pairs.begin(), pairs.end());
      _clockPairMin = pairs.front();
      _clockPairMedian = (static_cast<double>(pairs[127]) + static_cast<double>(pairs[128])) / 2;
    }

    void preload()
    {
      if (!_socket)
      {
        return; // The setup process already verified and acknowledged all H.
      }
      const auto deadline = ipc::deadline_ns(ipc::monotonic_ns(), _options._budgetMs);
      for (std::uint64_t sequence = 1; sequence <= _options._history; ++sequence)
      {
        if (_options._role == ipc::Role::Producer)
        {
          ipc::fill_payload(_payload, sequence, 0, 0);
          Sample unused;
          send_nanomsg(unused, deadline);
        }
        else
        {
          Sample unused;
          receive_nanomsg(unused, deadline);
          const auto envelope = ipc::validate_payload(_payload, sequence);
          require(envelope._scheduledNs == 0 && envelope._logicalStartNs == 0, "Preload timestamps must be zero");
        }
      }
    }

    void send_nanomsg(Sample& sample, std::uint64_t deadline)
    {
      sample._sendStart = ipc::monotonic_ns();
      for (;;)
      {
        ipc::check_deadline(deadline);
        const auto start = ipc::monotonic_ns();
        const auto sent = ::nn_send(_socket->_fd, _payload.data(), _payload.size(), NN_DONTWAIT);
        const auto error = sent < 0 ? ::nn_errno() : 0;
        const auto end = ipc::monotonic_ns();
        sample._sendApi += end - start;
        ++sample._sendAttempts;
        if (sent >= 0)
        {
          require(static_cast<std::size_t>(sent) == _payload.size(), "nanomsg sent an unexpected message length");
          sample._sendEnd = end;
          sample._successfulSendStart = start;
          sample._accepted = true;
          return;
        }
        if (error == EINTR)
        {
          continue;
        }
        require(error == EAGAIN, "nanomsg send failed");
        ++sample._backpressure;
        _socket->wait(NN_POLLOUT, deadline);
      }
    }

    void receive_nanomsg(Sample& sample, std::uint64_t deadline)
    {
      sample._receiveStart = ipc::monotonic_ns();
      for (;;)
      {
        ipc::check_deadline(deadline);
        const auto start = ipc::monotonic_ns();
        const auto received = ::nn_recv(_socket->_fd, _payload.data(), _payload.size(), NN_DONTWAIT);
        const auto error = received < 0 ? ::nn_errno() : 0;
        const auto end = ipc::monotonic_ns(); // Before validation or any other work.
        sample._receiveApi += end - start;
        ++sample._receiveAttempts;
        if (received >= 0)
        {
          require(static_cast<std::size_t>(received) == _payload.size(), "nanomsg received an unexpected message length");
          sample._receive = end;
          sample._successfulReceiveStart = start;
          return;
        }
        if (error == EINTR)
        {
          continue;
        }
        require(error == EAGAIN, "nanomsg receive failed");
        ++sample._timeouts;
        _socket->wait(NN_POLLIN, deadline);
      }
    }

    void produce(Sample& sample, std::uint64_t deadline)
    {
      sample._logicalStart = ipc::monotonic_ns();
      ipc::fill_payload(_payload, sample._sequence, sample._scheduled, sample._logicalStart);
      sample._hash = ipc::payload_hash(_payload);
      if (_socket)
      {
        send_nanomsg(sample, deadline);
      }
      else
      {
#if defined(ANGARIA_BUILD_STALL_STUDY) && ANGARIA_BUILD_STALL_STUDY
        if (_spsc)
        {
          while (!_spsc->has_credit())
          {
            ++sample._creditWaits;
            ipc::check_deadline(deadline);
            std::this_thread::yield();
          }
        }
#endif
        sample._idStart = ipc::monotonic_ns();
        auto issued = ipc::observe_operation(_observations[0], _options._collection,
            [&]
            {
              return _angaria->_task->next_publication_id();
            });
        sample._idEnd = ipc::monotonic_ns();
        const auto publication = value(std::move(issued));
        sample._publicationSequence = publication._sequence;
        sample._publicationIncarnation = publication._producer._incarnation;
        ipc::check_deadline(deadline);
        MessageHeader header;
        header._schema = _angaria->_schema;
        header._payloadBytes = _payload.size();
        const MessageView outgoing{header, _payload};
        std::array<DestinationOutcome, 1> outcomes;
#if defined(ANGARIA_BUILD_STALL_STUDY) && ANGARIA_BUILD_STALL_STUDY
        const auto reference = _spsc ? _spsc->next_record_reference() : 0;
#endif
        sample._sendStart = ipc::monotonic_ns();
        const auto sent = ipc::observe_operation(_observations[1], _options._collection,
            [&]
            {
              return _angaria->_writer->send(publication, outgoing, outcomes);
            });
        sample._sendEnd = ipc::monotonic_ns();
        sample._sendApi = sample._sendEnd - sample._sendStart;
        sample._successfulSendStart = sample._sendStart;
        sample._sendAttempts = 1;
        check_report(sent, publication, _angaria->_queue);
        sample._accepted = true;
        ++_accepted;
#if defined(ANGARIA_BUILD_STALL_STUDY) && ANGARIA_BUILD_STALL_STUDY
        if (_spsc)
        {
          _spsc->publish_committed(reference, publication);
          sample._sendEnd = ipc::monotonic_ns();
          sample._sendApi = sample._sendEnd - sample._sendStart;
        }
#endif
        return;
      }
      ++_accepted;
    }

    void consume(Sample& sample, std::uint64_t deadline)
    {
      std::optional<Delivery> delivery;
      MessageView message;
      std::span<const std::byte> payload;
      if (_socket)
      {
        receive_nanomsg(sample, deadline);
        payload = _payload;
      }
      else
      {
        sample._receiveStart = ipc::monotonic_ns();
        for (;;)
        {
          ipc::check_deadline(deadline);
          const auto timeout = std::chrono::milliseconds{std::min(5, ipc::remaining_ms(deadline))};
          const auto start = ipc::monotonic_ns();
#if defined(ANGARIA_BUILD_STALL_STUDY) && ANGARIA_BUILD_STALL_STUDY
          if (_spsc)
          {
            const auto received = ipc::observe_operation(_observations[2], _options._collection,
                [&]
                {
                  return _spsc->try_receive();
                });
            const auto end = ipc::monotonic_ns();
            ++sample._receiveAttempts;
            sample._receiveApi += end - start;
            if (received)
            {
              message = *received;
              sample._receive = end;
              sample._successfulReceiveStart = start;
              break;
            }
            ++sample._timeouts;
            std::this_thread::yield();
            continue;
          }
#endif
          auto received = ipc::observe_operation(_observations[2], _options._collection,
              [&]
              {
                return _angaria->_source->receive(timeout);
              });
          const auto end = ipc::monotonic_ns();
          ++sample._receiveAttempts;
          sample._receiveApi += end - start;
          if (const auto* error = std::get_if<Error>(&received))
          {
            throw BenchmarkFailure{*error};
          }
          if (const auto* found = std::get_if<Delivery>(&received))
          {
            sample._receive = end;
            sample._successfulReceiveStart = start;
            delivery.emplace(*found);
            message = delivery->_message;
            break;
          }
          require(std::holds_alternative<ReceiveTimeout>(received), "Unexpected delivery gap");
          ++sample._timeouts;
        }
        const auto& header = message._header;
        require(header._schema == _angaria->_schema && header._format == PayloadFormat::Binary &&
                    header._payloadBytes == _payload.size() && header._flags == 0 &&
                    header._queue == _angaria->_queue && header._queueSequence == sample._sequence &&
                    header._consumer == _angaria->_claim &&
                    header._publication._producer._task == _angaria->_producerId &&
                    header._publication._sequence != 0 && header._publication._producer._incarnation != 0,
                "Angaria received header/identity differs from the configured connection");
        sample._publicationSequence = header._publication._sequence;
        sample._publicationIncarnation = header._publication._producer._incarnation;
        payload = message._payload;
      }
      const auto envelope = ipc::validate_payload(payload, sample._sequence);
      sample._scheduled = envelope._scheduledNs;
      sample._logicalStart = envelope._logicalStartNs;
      sample._hash = ipc::payload_hash(payload);
      sample._verified = ipc::monotonic_ns();
      ++_verified;
      if (delivery)
      {
        ipc::check_deadline(deadline);
        sample._ackStart = ipc::monotonic_ns();
        auto acknowledged = ipc::observe_operation(_observations[3], _options._collection,
            [&]
            {
              return _angaria->_source->acknowledge(delivery->_token);
            });
        sample._ackEnd = ipc::monotonic_ns();
        value(std::move(acknowledged));
        ++_acknowledged;
      }
      ++_completed;
    }

    void measured(std::uint64_t t0)
    {
      const auto deadline = ipc::deadline_ns(t0, _options._budgetMs);
      try
      {
        // Library threads can receive data while the application thread is
        // asleep at T0. Include that work as well as the declared lead wait.
        _cpuStartTime = ipc::monotonic_ns();
        _cpuStart = ipc::process_cpu_ns();
        _dataStart = ipc::wait_until(t0, deadline);
        const auto count = _options._scenario == "empty" && _options._role == ipc::Role::Producer ? 0 : _options._messages;
        for (std::uint64_t index = 0; index < count; ++index)
        {
          auto& sample = _samples[_used++];
          sample._sequence = _options._history + index + 1;
          require(index == 0 || _options._intervalNs <=
                      (std::numeric_limits<std::uint64_t>::max() - t0) / index,
                  "Offered schedule exceeds the clock range");
          sample._scheduled = t0 + index * _options._intervalNs;
          if (_options._role == ipc::Role::Producer)
          {
            (void)ipc::wait_until(sample._scheduled, deadline);
            produce(sample, deadline);
          }
          else
          {
            consume(sample, deadline);
          }
        }
      }
      catch (const ipc::DeadlineExceeded& error)
      {
        _error = error.what();
        _status = "budget_exceeded";
        if (_dataStart == 0)
        {
          _dataStart = ipc::monotonic_ns();
        }
      }
      catch (...)
      {
        _error = current_error();
        _status = "error";
        if (_dataStart == 0)
        {
          _dataStart = ipc::monotonic_ns();
        }
      }
      _activeCpuEnd = ipc::process_cpu_ns();
      _dataEnd = ipc::monotonic_ns();
      if (_status == "complete" && _dataEnd >= deadline)
      {
        _status = "budget_exceeded";
        _error = "Work completed after the declared data budget";
      }
    }

    void check_extra_messages()
    {
      if (_options._role != ipc::Role::Consumer || _status != "complete")
      {
        return;
      }
      // Both producers/consumers have finished before FINISH. This check is
      // outside every data interval and never acts as per-message feedback.
      try
      {
        if (_socket)
        {
          const auto extra = ::nn_recv(_socket->_fd, _payload.data(), _payload.size(), NN_DONTWAIT);
          if (extra >= 0)
          {
            ++_unexpectedMessages;
          }
          else
          {
            require(::nn_errno() == EAGAIN, "nanomsg final empty check failed");
          }
        }
#if defined(ANGARIA_BUILD_STALL_STUDY) && ANGARIA_BUILD_STALL_STUDY
        else if (_spsc)
        {
          _unexpectedMessages = _spsc->try_receive() ? 1 : 0;
        }
#endif
        else
        {
          const auto extra = _angaria->_source->receive(std::chrono::nanoseconds{0});
          if (const auto* error = std::get_if<Error>(&extra))
          {
            throw BenchmarkFailure{*error};
          }
          _unexpectedMessages = std::holds_alternative<ReceiveTimeout>(extra) ? 0 : 1;
        }
        require(_unexpectedMessages == 0, "Unexpected messages remain after the completed cohort");
      }
      catch (...)
      {
        _status = "error";
        _error = current_error();
      }
    }

    void report() const
    {
      std::ostringstream output;
      output << "{\"event\":\"REPORT\",\"format_version\":1," << fields(_options)
             << ",\"framework_version\":" << ipc::json_string(angaria::detail::framework_version)
             << ",\"nanomsg_version\":" << ipc::json_string(ANGARIA_NANOMSG_VERSION)
             << ",\"nanomsg_commit\":" << ipc::json_string(ANGARIA_NANOMSG_COMMIT)
             << ",\"nanomsg_archive_sha256\":" << ipc::json_string(ANGARIA_NANOMSG_ARCHIVE_SHA256)
             << ',' << abi_fields()
             << ",\"pid\":" << ::getpid() << ",\"status\":" << ipc::json_string(_status)
             << ",\"error\":" << ipc::json_string(_error)
             << ",\"messages\":" << _options._messages << ",\"history\":" << _options._history
             << ",\"payload_bytes\":" << _options._payloadBytes << ",\"budget_ms\":" << _options._budgetMs
             << ",\"interval_ns\":" << _options._intervalNs << ",\"completed\":" << _completed
             << ",\"accepted\":" << _accepted << ",\"verified\":" << _verified
             << ",\"acknowledged\":" << _acknowledged
             << ",\"unexpected_messages\":" << _unexpectedMessages
             << ",\"data_start_ns\":" << _dataStart << ",\"data_end_ns\":" << _dataEnd
             << ",\"cpu_start_ns\":" << _cpuStart << ",\"cpu_end_ns\":" << _cpuEnd
             << ",\"cpu_start_time_ns\":" << _cpuStartTime
             << ",\"active_cpu_end_ns\":" << _activeCpuEnd << ",\"resource_end_ns\":" << _resourceEnd
             << ",\"cpu_scope\":\"RUSAGE_SELF user+system sampled after START receipt before T0 wait through FINISH; includes lead wait, library threads and control wait\""
             << ",\"clock_id\":\"CLOCK_MONOTONIC\",\"clock_resolution_ns\":" << ipc::clock_resolution_ns()
             << ",\"clock_pair_samples\":256,\"clock_pair_min_ns\":" << _clockPairMin
             << ",\"clock_pair_median_ns\":" << _clockPairMedian
             << ",\"clock_calibration_scope\":\"Warmed consecutive monotonic-read pair deltas outside data phase; not subtracted from message samples\""
             << ",\"thread_count_start\":" << (_threadsStart ? std::to_string(*_threadsStart) : "null")
             << ",\"thread_count_end\":" << (_threadsEnd ? std::to_string(*_threadsEnd) : "null")
             << ",\"peak_rss_bytes\":" << ipc::peak_rss_bytes()
             << ",\"file_bytes_before\":" << _fileBefore << ",\"file_bytes_after\":" << _fileAfter
             << ",\"socket_send_buffer_bytes\":" << (_socket ? _socket->_sendBuffer : 0)
             << ",\"socket_receive_buffer_bytes\":" << (_socket ? _socket->_receiveBuffer : 0)
             << ",\"log_limit_bytes\":" << (_angaria ? log_limit : 0)
             << ",\"ring_slots\":" << (_angaria ? _options._ringSlots : 0) << ",\"diagnostics\":";
      ipc::write_diagnostics(output, _observations);
      output << ",\"rows\":[";
      for (std::size_t index = 0; index < _used; ++index)
      {
        const auto& sample = _samples[index];
        if (index != 0)
        {
          output << ',';
        }
        output << "{\"sequence\":" << sample._sequence << ",\"scheduled_ns\":" << sample._scheduled
               << ",\"logical_start_ns\":" << sample._logicalStart
               << ",\"payload_hash\":" << ipc::json_string(ipc::hash_hex(sample._hash))
               << ",\"publication_sequence\":" << sample._publicationSequence
               << ",\"publication_incarnation\":" << sample._publicationIncarnation;
        if (_options._role == ipc::Role::Producer)
        {
          output << ",\"id_start_ns\":" << sample._idStart << ",\"id_end_ns\":" << sample._idEnd
                 << ",\"send_start_ns\":" << sample._sendStart << ",\"send_end_ns\":" << sample._sendEnd
                 << ",\"send_wall_ns\":" << (sample._sendEnd ? sample._sendEnd - sample._sendStart : 0)
                 << ",\"send_api_ns\":" << sample._sendApi << ",\"send_attempts\":" << sample._sendAttempts
                 << ",\"successful_send_start_ns\":" << sample._successfulSendStart
                 << ",\"successful_send_end_ns\":" << sample._sendEnd
                 << ",\"credit_waits\":" << sample._creditWaits
                 << ",\"backpressure_count\":" << sample._backpressure
                 << ",\"accepted\":" << (sample._accepted ? "true" : "false");
        }
        else
        {
          output << ",\"receive_start_ns\":" << sample._receiveStart << ",\"receive_ns\":" << sample._receive
                 << ",\"receive_wall_ns\":" << (sample._receive ? sample._receive - sample._receiveStart : 0)
                 << ",\"receive_api_ns\":" << sample._receiveApi
                 << ",\"receive_attempts\":" << sample._receiveAttempts << ",\"receive_timeouts\":" << sample._timeouts
                 << ",\"successful_receive_start_ns\":" << sample._successfulReceiveStart
                 << ",\"verified_ns\":" << sample._verified
                 << ",\"ack_start_ns\":" << sample._ackStart << ",\"ack_end_ns\":" << sample._ackEnd;
        }
        output << '}';
      }
      output << "]}";
      ipc::write_record(output.str());
    }

    ipc::Options _options;
    std::vector<std::byte> _payload;
    std::vector<Sample> _samples;
    std::optional<AngariaEndpoint> _angaria;
    std::optional<Socket> _socket;
#if defined(ANGARIA_BUILD_STALL_STUDY) && ANGARIA_BUILD_STALL_STUDY
    std::optional<spsc::RecordedSpsc> _spsc;
#endif
    std::array<ipc::OperationTotals, 4> _observations;
    std::size_t _used{};
    std::uint64_t _completed{};
    std::uint64_t _accepted{};
    std::uint64_t _verified{};
    std::uint64_t _acknowledged{};
    std::uint64_t _unexpectedMessages{};
    std::uint64_t _dataStart{};
    std::uint64_t _dataEnd{};
    std::uint64_t _cpuStart{};
    std::uint64_t _cpuStartTime{};
    std::uint64_t _cpuEnd{};
    std::uint64_t _activeCpuEnd{};
    std::uint64_t _resourceEnd{};
    std::uint64_t _clockPairMin{};
    double _clockPairMedian{};
    std::uint64_t _fileBefore{};
    std::uint64_t _fileAfter{};
    std::optional<std::uint64_t> _threadsStart;
    std::optional<std::uint64_t> _threadsEnd;
    std::string _status{"complete"};
    std::string _error;
  };
}

int main(int argc, char** argv)
{
  try
  {
    const auto options = ipc::parse_options(argc, argv);
    (void)runtime_abi();
    if (options._role == ipc::Role::Setup)
    {
      setup(options);
      return 0;
    }
    return Worker{options}.run() ? 0 : 1;
  }
  catch (...)
  {
    const auto message = current_error();
    std::cerr << "ipc_comparison: " << message << '\n';
    ipc::write_record("{\"event\":\"ERROR\",\"message\":" + ipc::json_string(message) + '}');
    return 1;
  }
}
