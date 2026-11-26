// Compile-only application/API review. No implementation is linked or faked.
// The runnable primer remains examples/primer/; see docs/public-interface.md.
#include "angaria/proposed/api.h"

#include <array>
#include <atomic>
#include <chrono>
#include <functional>
#include <ostream>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <variant>

namespace angaria::api_review
{
  namespace api = angaria::proposed;
  using namespace std::chrono_literals;

  static_assert(std::is_same_v<decltype(api::Message::_header), api::MessageHeader>);
  static_assert(std::is_same_v<decltype(api::MessageView::_header), api::MessageHeader>);
  static_assert(std::is_same_v<std::variant_alternative_t<0, api::ReceiveResult>, api::Delivery>);
  static_assert(std::is_copy_constructible_v<api::Message>);

  template <typename T>
  T value(Result<T> result)
  {
    if (const auto* error = std::get_if<Error>(&result))
    {
      throw std::runtime_error(error->_path + ": " + error->_message);
    }
    return std::get<T>(std::move(result));
  }

  // Applications choose how to persist a request identity and its input before
  // attempting a send. This callback copies/resolves what it retains: pooled
  // Binary bytes alone are not durable logical input. Keep exact definitions
  // and values (or a retained archive context), and look up the old ID before
  // re-encoding a request after interruption.
  using RememberRequest = std::function<OperationResult(PublicationId, api::MessageView)>;
  using ProcessValues = std::function<OperationResult(
      std::span<const api::FieldValue>, const api::MessageContext&)>;

  api::MessageView prepared_message(SchemaId schema, PayloadFormat format,
      std::span<const std::byte> payload, std::uint32_t flags = 0)
  {
    // Application input supplies only construction fields. Publication and
    // destination identity/times are established by the runtime, not invented
    // by the caller while it is preparing bytes for a future send.
    api::MessageHeader header;
    header._schema = schema;
    header._format = format;
    header._payloadBytes = payload.size();
    header._flags = flags;
    return {header, payload};
  }

  Result<std::vector<api::FieldValue>> decode_delivery(const api::Delivery& delivery,
      const api::SchemaCodec& codec, const api::PoolContext& pools)
  {
    const auto& header = delivery._message._header;
    if (codec.schema() != header._schema ||
        codec.schema_session() != header._publication._producer._session)
    {
      return Error{ErrorCode::SchemaConflict,
          "Route each delivery to its exact Session/schema codec", "example.schema"};
    }
    return codec.decode(delivery._message._payload, header._format, pools);
  }

  void initialize(const std::filesystem::path& toml)
  {
    const auto configuration = value(api::Configuration::load(toml));
    const auto validation = configuration.validate();
    if (!validation._valid)
    {
      throw std::runtime_error("Review configuration diagnostics before creating a Session");
    }
    auto created = value(api::Session::create(configuration, "Setup"));
    const auto capabilities = value(created._session.capabilities());
    (void)capabilities; // Useful to reject a requested platform/policy at setup.
  }

  SchemaId register_quote(api::Registry& registry)
  {
    api::FieldDescriptor instrument;
    instrument._id = FieldId{0xee000001};
    instrument._name = "Instrument";
    instrument._type = api::SymbolField{64};
    value(registry.register_field(instrument));
    api::FieldDescriptor price;
    price._id = FieldId{0xee000002};
    price._name = "PriceTicks";
    price._type = api::ScalarField{api::ScalarType::Int64};
    price._constraints._units = "0.01 USD";
    value(registry.register_field(price));
    api::MessageDescriptor quote;
    quote._schema = {MessageType{17}, 1};
    quote._name = "Quote";
    quote._fields = {instrument._id, price._id};
    return value(registry.register_message(quote));
  }

  struct FeedEndpoints
  {
    api::Task _task;
    api::QueueWriter _output;
    api::SchemaCodec _codec;
    api::PoolContext _pools;
  };

  FeedEndpoints attach_feed(const std::filesystem::path& directory)
  {
    auto session = value(api::Session::attach(directory));
    auto feed = value(session.attach_task("Feed"));
    auto output = value(feed.open_writer("FeedToStrategy"));
    const auto schema = value(feed.registry().find_message("Quote", 1));
    auto codec = value(api::SchemaCodec::from_registry(feed.registry(), schema));
    auto pools = value(feed.pools());
    return {std::move(feed), std::move(output), std::move(codec), std::move(pools)};
  }

  // Names, claims, and codecs are retained across sends, outside the hot loop.
  Result<api::PublicationOutcome> send_quote(FeedEndpoints& feed,
      std::span<const api::FieldValue> values, const RememberRequest& remember)
  {
    const auto payload = value(feed._codec.encode(values, PayloadFormat::Binary, feed._pools));
    const auto message = payload.view();
    const auto publication = value(feed._task.next_publication_id());
    value(remember(publication, message));

    auto result = feed._output.send(publication, message);
    if (const auto* error = std::get_if<Error>(&result);
        error && error->_code == ErrorCode::OutcomeUnknown)
    {
      // Ask about the same request. Issuing a new identity here could duplicate
      // a message whose acceptance committed before the caller lost its result.
      return feed._task.publication_status(publication);
    }
    return result;
  }

  Result<api::PublicationOutcome> send_example_quote(FeedEndpoints& feed,
      const RememberRequest& remember)
  {
    const std::array values{
        api::FieldValue{api::SymbolValue{"IBM"}}, api::FieldValue{std::int64_t{12345}}};
    return send_quote(feed, values, remember);
  }

  api::PayloadLease encode_into_reused_buffer(const FeedEndpoints& feed,
      std::span<const api::FieldValue> values, std::span<std::byte> output)
  {
    // output remains caller-owned. Retain the returned lease until the send
    // call finishes; bytes and referenced pool values both need that lifetime.
    return value(feed._codec.encode_into(values, output, PayloadFormat::Binary, feed._pools));
  }

  Result<api::PublicationOutcome> publish_and_retry(api::Task& task,
      api::PublishingQueueSet& output, api::MessageView message, const RememberRequest& remember)
  {
    const auto publication = value(task.next_publication_id());
    value(remember(publication, message));
    auto result = output.publish(publication, message);
    if (const auto* error = std::get_if<Error>(&result))
    {
      if (error->_code == ErrorCode::OutcomeUnknown)
      {
        return task.publication_status(publication);
      }
      return *error;
    }
    const auto& outcome = std::get<api::PublicationOutcome>(result);
    if (outcome._status != api::AcceptanceStatus::Partial)
    {
      return result;
    }
    std::vector<QueueId> retry;
    for (const auto& destination : outcome._destinations)
    {
      if (destination._status == DestinationStatus::Rejected)
      {
        retry.push_back(destination._queue);
      }
    }
    // Production applications choose whether/when each reason is retryable.
    // Already accepted destinations are never re-published by this operation.
    return output.retry(publication, retry);
  }

  PublicationReport send_with_reused_output(api::QueueWriter& writer, PublicationId publication,
      api::MessageView message, std::span<DestinationOutcome> output)
  {
    // The caller has already issued and retained this request identity.
    auto report = writer.send(publication, message, output);
    if (report._error != ErrorCode::None)
    {
      // The caller retains report._publication for status lookup. An output
      // error on a lookup cannot prove that an earlier send was rejected.
      return report;
    }
    return report; // Only now may the caller interpret _status/_destinations.
  }

  OperationResult consume_one(api::SourceQueueSet& inputs, const api::SchemaCodec& codec,
      const api::PoolContext& pools, const ProcessValues& process,
      const std::function<OperationResult(const api::DeliveryGap&)>& report_gap,
      api::InputTraceRecorder* trace = nullptr)
  {
    auto received = inputs.receive(100ms);
    if (const auto* error = std::get_if<Error>(&received))
    {
      return *error;
    }
    if (const auto* gap = std::get_if<api::DeliveryGap>(&received))
    {
      return report_gap(*gap);
    }
    if (std::holds_alternative<api::ReceiveTimeout>(received))
    {
      return std::monostate{};
    }
    const auto& delivery = std::get<api::Delivery>(received);
    const auto values = [&]() -> Result<std::vector<api::FieldValue>>
    {
      if (trace)
      {
        const auto recorded = trace->record_input(delivery);
        if (const auto* error = std::get_if<Error>(&recorded))
        {
          return *error;
        }
      }
      return decode_delivery(delivery, codec, pools);
    }();
    OperationResult processed;
    if (const auto* error = std::get_if<Error>(&values))
    {
      processed = *error;
    }
    else
    {
      processed = process(std::get<std::vector<api::FieldValue>>(values), delivery.context());
    }
    if (const auto* error = std::get_if<Error>(&processed))
    {
      const auto released = inputs.release(delivery._token);
      if (const auto* cleanup = std::get_if<Error>(&released))
      {
        return *cleanup; // Stop using the borrowed view if progress is uncertain.
      }
      return *error;
    }
    return inputs.acknowledge(delivery._token);
  }

  api::Message retain_message_before_acknowledgment(api::SourceQueueSet& inputs,
      const api::Delivery& delivery, std::uint64_t maxPayloadBytes)
  {
    // This declaration-only example makes the copy contract visible: owning
    // bytes and decode dependencies does not discard the complete header or
    // keep the Source's delivery in flight. Both message forms expose _header.
    const auto expected = delivery._message._header;
    auto saved = value(delivery._message.copy_message(maxPayloadBytes));
    if (saved._header != expected || saved.view()._header != expected)
    {
      throw std::runtime_error("An owning copy must preserve the complete MessageHeader");
    }
    value(inputs.acknowledge(delivery._token));
    // The returned Message remains usable after the borrowed delivery ends.
    return saved;
  }

  struct StrategyEndpoints
  {
    api::Task _task;
    api::SourceQueueSet _inputs;
    api::SchemaCodec _codec;
    api::PoolContext _pools;
  };

  StrategyEndpoints attach_strategy(const std::filesystem::path& directory)
  {
    auto session = value(api::Session::attach(directory));
    auto strategy = value(session.attach_task("Strategy"));
    auto inputs = value(strategy.open_source_set("StrategyInputs"));
    const auto schema = value(strategy.registry().find_message("Quote", 1));
    auto codec = value(api::SchemaCodec::from_registry(strategy.registry(), schema));
    auto pools = value(session.pools());
    return {std::move(strategy), std::move(inputs), std::move(codec), std::move(pools)};
  }

  OperationResult consume_quote(StrategyEndpoints& strategy, const ProcessValues& process,
      const std::function<OperationResult(const api::DeliveryGap&)>& report_gap)
  {
    return consume_one(strategy._inputs, strategy._codec, strategy._pools, process, report_gap);
  }

  OperationResult finish_consumer(StrategyEndpoints& strategy)
  {
    return strategy._inputs.close();
  }

  void diagnose(api::Session& session, std::ostream& output)
  {
    const auto observation = value(session.observe());
    output << "Allocated " << observation._storage._allocatedBytes << " / "
           << observation._storage._maxLogBytes << " bytes\n";
    for (const auto& queue : observation._queues)
    {
      output << "Queue " << static_cast<std::uint16_t>(queue._queue)
             << ": " << queue._outstandingDeliveries << " outstanding\n";
    }
  }

  void inspect_archive(const std::filesystem::path& archivePath, std::ostream& output)
  {
    const auto archive = value(api::LogArchive::open_capture(archivePath));
    const auto catalog = value(archive.catalog());
    auto cursor = value(archive.publications());
    while (auto record = value(cursor.read_next()))
    {
      output << value(catalog.inspect(*record)) << '\n';
    }
  }

  // The same decoded-values handler can run live or under a debugger. Source
  // pool values are resolved by each LogRecord's retained archive context.
  void replay_debug(const std::filesystem::path& archivePath, SourceQueueSetId source, SchemaId schema,
      const ProcessValues& process, const std::function<OperationResult(std::string_view)>& save_checkpoint)
  {
    const auto archive = value(api::LogArchive::open_capture(archivePath));
    const auto catalog = value(archive.catalog());
    const auto codec = value(catalog.compile(schema));
    api::LogFilter filter;
    filter._schema = schema;
    // This reproduces accepted inputs for one consumer Source in acceptance
    // order, not its unrecorded historical priority/poll/selection schedule.
    auto replay = value(api::ReplaySession::create(value(archive.deliveries(source, filter))));
    while (auto frame = value(replay.step()))
    {
      const auto values = value(frame->_record.decode(codec));
      value(process(values, frame->_context));
      const auto checkpoint = value(replay.checkpoint());
      value(save_checkpoint(checkpoint.token()));
    }
  }

  api::InputTraceRecorder start_input_trace(StrategyEndpoints& strategy,
      const std::filesystem::path& newPath)
  {
    return value(api::InputTraceRecorder::create(newPath, strategy._task));
  }

  OperationResult consume_quote_with_trace(StrategyEndpoints& strategy,
      api::InputTraceRecorder& trace, const ProcessValues& process,
      const std::function<OperationResult(const api::DeliveryGap&)>& report_gap)
  {
    // The ordinary loop handles recorder failure with the same release/close
    // rules as processing failure. No hidden acknowledgment is introduced.
    return consume_one(strategy._inputs, strategy._codec, strategy._pools, process, report_gap, &trace);
  }

  struct RecordedReplay
  {
    api::ReplaySession _replay;
    api::ExecutionTrace::Status _traceStatus;
  };

  RecordedReplay replay_recorded_inputs(const std::filesystem::path& path)
  {
    const auto trace = value(api::ExecutionTrace::open(path));
    api::ReplayOptions options;
    options._timeBasis = api::ReplayTimeBasis::InputTraceElapsed;
    // Expose incomplete/unfinalized status to the caller before it chooses to
    // process this prefix. Creating the replay itself invokes no handler.
    return {value(api::ReplaySession::create(value(trace.inputs()), options)), value(trace.status())};
  }

  Result<api::ReplayRunReport> replay_bounded(api::ReplaySession& replay,
      const api::SchemaCodec& codec, const ProcessValues& process,
      const std::atomic<bool>& stopRequested)
  {
    api::ReplayRunLimits limits;
    limits._maxMessages = 1000;
    limits._maxElapsed = 2s;
    return replay.run([&](const api::ReplayFrame& frame) -> Result<api::ReplayDecision>
    {
      const auto values = frame._record.decode(codec);
      if (const auto* error = std::get_if<Error>(&values))
      {
        return *error;
      }
      const auto result = process(std::get<std::vector<api::FieldValue>>(values), frame._context);
      if (const auto* error = std::get_if<Error>(&result))
      {
        return *error;
      }
      return api::ReplayDecision::Continue;
    }, limits, [&]()
    {
      return stopRequested.load(std::memory_order_relaxed);
    });
  }

  using RememberReplay = std::function<OperationResult(const api::ReplayOrigin&,
      PublicationId, SchemaId, std::span<const api::FieldValue>)>;

  Result<api::ReplayInjection> reinject_one(const api::LogRecord& record,
      const api::SchemaCodec& sourceCodec, const api::SchemaCodec& targetCodec,
      const api::PoolContext& targetPools, api::Task& testProducer,
      api::QueueWriter& testQueue, const RememberReplay& remember)
  {
    // The caller explicitly chooses the isolated test Session/Queue and target
    // schema. Resolve source pool IDs, then re-encode against target pools.
    const auto targetSession = testProducer.claim()._session;
    if (targetSession == record._origin._publication._producer._session ||
        targetSession != targetCodec.schema_session())
    {
      return Error{ErrorCode::SchemaConflict,
          "Choose a distinct test Session and its exact target codec", "example.reinjection"};
    }
    const auto values = value(record.decode(sourceCodec));
    const auto payload = value(targetCodec.encode(values, PayloadFormat::Binary, targetPools));
    const auto message = payload.view(record._message._header._flags);
    const auto publication = value(testProducer.next_publication_id());
    // Persist source position -> target ID and owning logical input before the
    // attempt. After a crash, resolve that ID before advancing/reinjecting.
    value(remember(record._origin, publication, targetCodec.schema(), values));
    auto result = testQueue.send(publication, message);
    if (const auto* error = std::get_if<Error>(&result))
    {
      return *error; // Retained request identity is used with ordinary status lookup.
    }
    return api::ReplayInjection{record._origin, publication,
        std::get<api::PublicationOutcome>(std::move(result))};
  }
} // namespace angaria::api_review
