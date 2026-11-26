#include "angaria/runtime.h"

#include <array>
#include <chrono>
#include <filesystem>
#include <ostream>
#include <stdexcept>
#include <utility>
#include <variant>

namespace angaria::examples
{
  template <typename T>
  T require_value(Result<T> result)
  {
    if (const auto error = std::get_if<Error>(&result))
    {
      throw std::runtime_error(error->_message);
    }
    return std::get<T>(std::move(result));
  }

  struct TradingTopology
  {
    TaskId _marketData{};
    TaskId _strategyHost{};
    TaskId _strategy{};
    TaskId _risk{};
    SourceQueueSetId _strategyInputs{};
    SourceQueueSetId _riskInputs{};
    QueueId _marketToStrategy{};
    QueueId _marketToRisk{};
    QueueId _strategyToRisk{};
    PublishingQueueSetId _updates{};
    SchemaId _quote{};
  };

  TradingTopology register_trading_topology(Registry& registry)
  {
    TradingTopology topology;

    TaskDescriptor market;
    market._name = "MarketData";
    topology._marketData = require_value(registry.register_task(market));

    TaskDescriptor host;
    host._name = "StrategyHost";
    topology._strategyHost = require_value(registry.register_task(host));

    TaskDescriptor strategy;
    strategy._name = "Strategy";
    strategy._kind = TaskKind::Thread;
    strategy._process = topology._strategyHost;
    topology._strategy = require_value(registry.register_task(strategy));

    TaskDescriptor risk;
    risk._name = "Risk";
    topology._risk = require_value(registry.register_task(risk));

    SourceQueueSetDescriptor strategyInputs;
    strategyInputs._name = "StrategyInputs";
    strategyInputs._owner = topology._strategy;
    topology._strategyInputs = require_value(registry.register_source_set(strategyInputs));

    SourceQueueSetDescriptor riskInputs;
    riskInputs._name = "RiskInputs";
    riskInputs._owner = topology._risk;
    topology._riskInputs = require_value(registry.register_source_set(riskInputs));

    QueueDescriptor marketToStrategy;
    marketToStrategy._name = "MarketDataToStrategy";
    marketToStrategy._producer = topology._marketData;
    marketToStrategy._sourceSet = topology._strategyInputs;
    marketToStrategy._priority = 100;
    marketToStrategy._delivery = DeliveryPolicy::AllowLoss;
    topology._marketToStrategy = require_value(registry.register_queue(marketToStrategy));

    QueueDescriptor marketToRisk;
    marketToRisk._name = "MarketDataToRisk";
    marketToRisk._producer = topology._marketData;
    marketToRisk._sourceSet = topology._riskInputs;
    marketToRisk._priority = 100;
    topology._marketToRisk = require_value(registry.register_queue(marketToRisk));

    QueueDescriptor strategyToRisk;
    strategyToRisk._name = "StrategyToRisk";
    strategyToRisk._producer = topology._strategy;
    strategyToRisk._sourceSet = topology._riskInputs;
    strategyToRisk._priority = 200;
    topology._strategyToRisk = require_value(registry.register_queue(strategyToRisk));

    PublishingQueueSetDescriptor updates;
    updates._name = "MarketDataUpdates";
    updates._owner = topology._marketData;
    updates._destinations = {topology._marketToStrategy, topology._marketToRisk};
    topology._updates = require_value(registry.register_publishing_set(updates));

    FieldDescriptor price;
    price._id = FieldId{0xEE000001};
    price._name = "PriceTicks";
    price._base = EFieldBase::BASE_INT64;
    price._size = 8;
    const auto priceId = require_value(registry.register_field(price));

    FieldDescriptor quantity;
    quantity._id = FieldId{0xEE000002};
    quantity._name = "Quantity";
    quantity._base = EFieldBase::BASE_UINT64;
    quantity._size = 8;
    const auto quantityId = require_value(registry.register_field(quantity));

    MessageDescriptor quote;
    quote._schema = {MessageType{1}, 1};
    quote._name = "Quote";
    quote._fields = {priceId, quantityId};
    topology._quote = require_value(registry.register_message(quote));

    require_value(registry.activate_source_set(topology._strategyInputs));
    require_value(registry.activate_source_set(topology._riskInputs));
    require_value(registry.activate_queue(topology._marketToStrategy));
    require_value(registry.activate_queue(topology._marketToRisk));
    require_value(registry.activate_queue(topology._strategyToRisk));
    require_value(registry.activate_publishing_set(topology._updates));
    return topology;
  }

  CreatedSession create_trading_session(const std::filesystem::path& directory)
  {
    SessionOptions options;
    options._name = "Trading";
    options._applicationVersion = "Trading-1";
    options._directory = directory;
    options._maxLogBytes = 67108864;
    options._limits = CapacityLimits{}.tasks(16).queues(128).source_sets(16);

    TaskDescriptor creator;
    creator._name = "MarketData";
    auto created = require_value(Session::create(options, creator));
    auto registry = created._creator.registry();
    register_trading_topology(registry);
    return created;
  }

  struct MarketDataEndpoints
  {
    Task _task;
    PublishingQueueSet _updates;
    SchemaId _quote;
  };

  CreatedSession create_trading_from_toml(const std::filesystem::path& path)
  {
    return require_value(Session::create_from_file(path, "MarketData"));
  }

  // Pass the creator claim or an attached MarketData Task and keep these endpoints alive.
  MarketDataEndpoints open_market_data(Task market)
  {
    auto registry = market.registry();
    const auto setId = require_value(registry.find_publishing_set("MarketDataUpdates"));
    auto updates = require_value(market.open_publishing_set(setId));
    const auto schema = require_value(registry.find_message("Quote", 1));
    return {std::move(market), std::move(updates), schema};
  }

  // Reuse the endpoints across publications. The caller owns payload and output storage.
  PublicationReport publish_quote(MarketDataEndpoints& market, std::span<const std::byte> payload,
                                  std::span<DestinationOutcome> outcomes)
  {
    const auto publication = require_value(market._task.next_publication_id());
    const MessageView message{market._quote, PayloadFormat::Binary, payload, 0};
    auto report = market._updates.publish(publication, message, outcomes);
    if (report._error == ErrorCode::OutcomeUnknown ||
        (report._error == ErrorCode::None && report._status == PublicationStatus::OutcomeUnknown))
    {
      report = market._task.publication_status(publication, outcomes);
    }
    return report;
  }

  PublicationReport retry_rejected(Task& producer, PublishingQueueSet& updates, PublicationId publication,
                                   std::span<const QueueId> rejected, std::span<DestinationOutcome> outcomes)
  {
    const auto previous = producer.publication_status(publication, outcomes);
    if (previous._error != ErrorCode::None || previous._status != PublicationStatus::Partial)
    {
      return previous;
    }
    return updates.retry(publication, rejected, outcomes);
  }

  struct RiskEndpoints
  {
    Task _task;
    SourceQueueSet _inputs;
  };

  RiskEndpoints attach_risk(Session& session)
  {
    auto registry = session.registry();
    auto risk = require_value(session.attach_task(require_value(registry.find_task("Risk"))));
    auto inputs = require_value(risk.open_source_set(require_value(registry.find_source_set("RiskInputs"))));
    return {std::move(risk), std::move(inputs)};
  }

  void process_risk_input(RiskEndpoints& risk, void (*process)(const DeliveryView&),
                          void (*report_gap)(const DeliveryGap&))
  {
    auto& inputs = risk._inputs;
    const auto received = inputs.receive(std::chrono::seconds{1});
    if (const auto delivery = std::get_if<DeliveryView>(&received))
    {
      process(*delivery);
      require_value(inputs.acknowledge(delivery->_token));
    }
    else if (const auto gap = std::get_if<DeliveryGap>(&received))
    {
      report_gap(*gap);
    }
    else if (const auto error = std::get_if<Error>(&received))
    {
      throw std::runtime_error(error->_message);
    }
  }

  OperationResult release_strategy_input(Session& session)
  {
    auto registry = session.registry();
    // The StrategyHost process keeps its host claim alive for its Strategy thread.
    auto host = require_value(session.attach_task(require_value(registry.find_task("StrategyHost"))));
    auto strategy = require_value(session.attach_task(require_value(registry.find_task("Strategy"))));
    auto inputs =
        require_value(strategy.open_source_set(require_value(registry.find_source_set("StrategyInputs"))));
    const auto received = inputs.receive();
    if (const auto delivery = std::get_if<DeliveryView>(&received))
    {
      return inputs.release(delivery->_token);
    }
    if (const auto error = std::get_if<Error>(&received))
    {
      return *error;
    }
    return std::monostate{};
  }

  void inspect_trading(Session& session, std::ostream& output)
  {
    const auto sessionMetadata = require_value(session.metadata());
    const auto options = require_value(session.options());
    auto registry = session.registry();
    const auto market = require_value(registry.find_task("MarketData"));
    const auto registration = require_value(registry.metadata(market));
    const auto status = require_value(registry.task_status(market));
    output << options._name << " version=" << sessionMetadata._applicationVersion
           << " creator=" << static_cast<std::uint16_t>(registration._createdBy)
           << " incarnation=" << status._incarnation << " pid=" << status._processId << '\n';
  }

  PublicationReport send_strategy_result(Task& strategy, QueueId queue, SchemaId schema,
                                         std::span<const std::byte> encoded, DestinationOutcome& outcome)
  {
    auto writer = require_value(strategy.open_writer(queue));
    const auto publication = require_value(strategy.next_publication_id());
    return writer.send(publication, MessageView{schema, PayloadFormat::Binary, encoded, 0}, {&outcome, 1});
  }
} // namespace angaria::examples
