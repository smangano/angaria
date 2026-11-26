// Configure two producers, two consumers, and a copy-to-audit route. This
// program records the wiring; later lessons use the Queue endpoints.
#include "angaria/runtime.h"

#include <exception>
#include <iostream>
#include <utility>
#include <variant>
#include <string>
#include <vector>

int main(int argc, char** argv)
{
  if (argc != 2)
  {
    std::cerr << "Usage: angaria_primer_05_fanout <new-session-directory>\n";
    return 1;
  }
  try
  {
    using namespace angaria;
    // These bounds are configuration choices for this example.
    SessionOptions options;
    options._name = "Primer fan-out";
    options._directory = argv[1];
    options._maxLogBytes = 1024 * 1024;
    options._limits = CapacityLimits::empty().tasks(8).queues(8)
        .source_sets(4).publishing_sets(4).memberships_per_set(4)
        .fields(16).message_schemas(8);
    options._defaults._ringCapacity = 8;
    TaskDescriptor coordinator;
    coordinator._name = "Coordinator";
    auto created_result = Session::create(options, coordinator);
    if (const auto* error = std::get_if<Error>(&created_result))
    {
      if (error->_code == ErrorCode::Conflict)
      {
        std::cerr << "Conflict: ";
      }
      std::cerr << "Session creation failed: " << error->_message << " at " << error->_path
                << " (code " << static_cast<int>(error->_code) << ")" << '\n';
      return 1;
    }
    auto created = std::get<CreatedSession>(std::move(created_result));
    auto registry = created._creator.registry();
    TaskDescriptor feed_descriptor;
    feed_descriptor._name = "Feed";
    auto feed_result = registry.register_task(feed_descriptor);
    if (const auto* error = std::get_if<Error>(&feed_result))
    {
      std::cerr << "Registering Feed failed: " << error->_message << " at " << error->_path
                << " (code " << static_cast<int>(error->_code) << ")" << '\n';
      return 1;
    }
    const auto feed = std::get<TaskId>(std::move(feed_result));
    TaskDescriptor strategy_descriptor;
    strategy_descriptor._name = "Strategy";
    auto strategy_result = registry.register_task(strategy_descriptor);
    if (const auto* error = std::get_if<Error>(&strategy_result))
    {
      std::cerr << "Registering Strategy failed: " << error->_message << " at " << error->_path
                << " (code " << static_cast<int>(error->_code) << ")" << '\n';
      return 1;
    }
    const auto strategy = std::get<TaskId>(std::move(strategy_result));
    TaskDescriptor risk_descriptor;
    risk_descriptor._name = "Risk";
    auto risk_result = registry.register_task(risk_descriptor);
    if (const auto* error = std::get_if<Error>(&risk_result))
    {
      std::cerr << "Registering Risk failed: " << error->_message << " at " << error->_path
                << " (code " << static_cast<int>(error->_code) << ")" << '\n';
      return 1;
    }
    const auto risk = std::get<TaskId>(std::move(risk_result));
    TaskDescriptor audit_descriptor;
    audit_descriptor._name = "Audit";
    auto audit_result = registry.register_task(audit_descriptor);
    if (const auto* error = std::get_if<Error>(&audit_result))
    {
      std::cerr << "Registering Audit failed: " << error->_message << " at " << error->_path
                << " (code " << static_cast<int>(error->_code) << ")" << '\n';
      return 1;
    }
    const auto audit = std::get<TaskId>(std::move(audit_result));

    SourceQueueSetDescriptor risk_inputs;
    risk_inputs._name = "RiskInputs";
    risk_inputs._owner = risk;
    auto risk_source_result = registry.register_source_set(risk_inputs);
    if (const auto* error = std::get_if<Error>(&risk_source_result))
    {
      std::cerr << "Registering RiskInputs failed: " << error->_message << " at " << error->_path
                << " (code " << static_cast<int>(error->_code) << ")" << '\n';
      return 1;
    }
    const auto risk_source = std::get<SourceQueueSetId>(std::move(risk_source_result));
    SourceQueueSetDescriptor audit_inputs;
    audit_inputs._name = "AuditInputs";
    auto audit_source_result = registry.register_source_set(audit_inputs);
    if (const auto* error = std::get_if<Error>(&audit_source_result))
    {
      std::cerr << "Registering AuditInputs failed: " << error->_message << " at " << error->_path
                << " (code " << static_cast<int>(error->_code) << ")" << '\n';
      return 1;
    }
    const auto audit_source = std::get<SourceQueueSetId>(std::move(audit_source_result));
    // A draft can be completed later. Adoption changes its logical owner,
    // without requiring the Audit process to be running.
    auto adoption_result = registry.adopt_source_set(audit_source, audit);
    if (const auto* error = std::get_if<Error>(&adoption_result))
    {
      std::cerr << "Adopting AuditInputs failed: " << error->_message << " at " << error->_path
                << " (code " << static_cast<int>(error->_code) << ")" << '\n';
      return 1;
    }

    QueueDescriptor quotes_to_risk;
    quotes_to_risk._name = "QuotesToRisk";
    quotes_to_risk._producer = feed;
    quotes_to_risk._sourceSet = risk_source;
    quotes_to_risk._priority = 2;
    auto risk_quotes_result = registry.register_queue(quotes_to_risk);
    if (const auto* error = std::get_if<Error>(&risk_quotes_result))
    {
      std::cerr << "Registering QuotesToRisk failed: " << error->_message << " at " << error->_path
                << " (code " << static_cast<int>(error->_code) << ")" << '\n';
      return 1;
    }
    const auto risk_quotes = std::get<QueueId>(std::move(risk_quotes_result));
    auto orders_to_risk = quotes_to_risk;
    orders_to_risk._name = "OrdersToRisk";
    orders_to_risk._producer = strategy;
    orders_to_risk._priority = 8;
    auto risk_orders_result = registry.register_queue(orders_to_risk);
    if (const auto* error = std::get_if<Error>(&risk_orders_result))
    {
      std::cerr << "Registering OrdersToRisk failed: " << error->_message << " at " << error->_path
                << " (code " << static_cast<int>(error->_code) << ")" << '\n';
      return 1;
    }
    const auto risk_orders = std::get<QueueId>(std::move(risk_orders_result));
    auto quotes_to_audit = quotes_to_risk;
    quotes_to_audit._name = "QuotesToAudit";
    quotes_to_audit._sourceSet = SourceQueueSetId::Invalid;
    quotes_to_audit._priority = 1;
    auto audit_quotes_result = registry.register_queue(quotes_to_audit);
    if (const auto* error = std::get_if<Error>(&audit_quotes_result))
    {
      std::cerr << "Registering QuotesToAudit failed: " << error->_message << " at " << error->_path
                << " (code " << static_cast<int>(error->_code) << ")" << '\n';
      return 1;
    }
    const auto audit_quotes = std::get<QueueId>(std::move(audit_quotes_result));
    // Assignment updates the Queue and its Source's member list together.
    auto assignment_result = registry.assign_source(audit_quotes, audit_source, 1);
    if (const auto* error = std::get_if<Error>(&assignment_result))
    {
      std::cerr << "Assigning QuotesToAudit failed: " << error->_message << " at " << error->_path
                << " (code " << static_cast<int>(error->_code) << ")" << '\n';
      return 1;
    }
    auto orders_to_audit = orders_to_risk;
    orders_to_audit._name = "OrdersToAudit";
    orders_to_audit._sourceSet = audit_source;
    orders_to_audit._priority = 1;
    auto audit_orders_result = registry.register_queue(orders_to_audit);
    if (const auto* error = std::get_if<Error>(&audit_orders_result))
    {
      std::cerr << "Registering OrdersToAudit failed: " << error->_message << " at " << error->_path
                << " (code " << static_cast<int>(error->_code) << ")" << '\n';
      return 1;
    }
    const auto audit_orders = std::get<QueueId>(std::move(audit_orders_result));

    PublishingQueueSetDescriptor quote_updates;
    quote_updates._name = "QuoteUpdates";
    quote_updates._owner = feed;
    quote_updates._destinations = {risk_quotes, audit_quotes};
    auto quote_publisher_result = registry.register_publishing_set(quote_updates);
    if (const auto* error = std::get_if<Error>(&quote_publisher_result))
    {
      std::cerr << "Registering QuoteUpdates failed: " << error->_message << " at " << error->_path
                << " (code " << static_cast<int>(error->_code) << ")" << '\n';
      return 1;
    }
    const auto quote_publisher = std::get<PublishingQueueSetId>(std::move(quote_publisher_result));
    PublishingQueueSetDescriptor order_updates;
    order_updates._name = "OrderUpdates";
    order_updates._owner = strategy;
    order_updates._destinations = {risk_orders, audit_orders};
    auto order_publisher_result = registry.register_publishing_set(order_updates);
    if (const auto* error = std::get_if<Error>(&order_publisher_result))
    {
      std::cerr << "Registering OrderUpdates failed: " << error->_message << " at " << error->_path
                << " (code " << static_cast<int>(error->_code) << ")" << '\n';
      return 1;
    }
    const auto order_publisher = std::get<PublishingQueueSetId>(std::move(order_publisher_result));

    for (const auto source : {risk_source, audit_source})
    {
      auto source_activation_result = registry.activate_source_set(source);
      if (const auto* error = std::get_if<Error>(&source_activation_result))
      {
        std::cerr << "Activating a Source failed: " << error->_message << " at " << error->_path
                << " (code " << static_cast<int>(error->_code) << ")" << '\n';
        return 1;
      }
    }
    for (const auto queue : {risk_quotes, risk_orders, audit_quotes, audit_orders})
    {
      auto queue_activation_result = registry.activate_queue(queue);
      if (const auto* error = std::get_if<Error>(&queue_activation_result))
      {
        std::cerr << "Activating a Queue failed: " << error->_message << " at " << error->_path
                << " (code " << static_cast<int>(error->_code) << ")" << '\n';
        return 1;
      }
      auto descriptor_result = registry.describe_queue(queue);
      if (const auto* error = std::get_if<Error>(&descriptor_result))
      {
        std::cerr << "Reading a Queue failed: " << error->_message << " at " << error->_path
                << " (code " << static_cast<int>(error->_code) << ")" << '\n';
        return 1;
      }
      const auto descriptor = std::get<QueueDescriptor>(std::move(descriptor_result));
      auto producer_result = registry.describe_task(descriptor._producer);
      if (const auto* error = std::get_if<Error>(&producer_result))
      {
        std::cerr << "Reading the producer failed: " << error->_message << " at " << error->_path
                << " (code " << static_cast<int>(error->_code) << ")" << '\n';
        return 1;
      }
      const auto producer = std::get<TaskDescriptor>(std::move(producer_result));
      auto input_result = registry.describe_source_set(descriptor._sourceSet);
      if (const auto* error = std::get_if<Error>(&input_result))
      {
        std::cerr << "Reading the Source failed: " << error->_message << " at " << error->_path
                << " (code " << static_cast<int>(error->_code) << ")" << '\n';
        return 1;
      }
      const auto input = std::get<SourceQueueSetDescriptor>(std::move(input_result));
      std::cout << descriptor._name << ": " << producer._name << " -> " << input._name
                << ", priority=" << static_cast<unsigned>(descriptor._priority) << '\n';
    }
    for (const auto publisher : {quote_publisher, order_publisher})
    {
      auto publishing_activation_result = registry.activate_publishing_set(publisher);
      if (const auto* error = std::get_if<Error>(&publishing_activation_result))
      {
        std::cerr << "Activating a Publishing set failed: " << error->_message << " at " << error->_path
                << " (code " << static_cast<int>(error->_code) << ")" << '\n';
        return 1;
      }
    }
    // Empty overrides inherit the Session's recorded defaults. These names
    // describe the configured policy profile; this program does not run it.
    std::cout << "Policy defaults: Strict selection, Spin wait, RecoverFromLog, AllDestinations.\n";

    // Active Publishing sets can change destinations through their owner.
    // Descriptors own their values: the earlier copy still describes two
    // destinations after the Registry records a new one-destination snapshot.
    auto before_result = registry.describe_publishing_set(quote_publisher);
    if (const auto* error = std::get_if<Error>(&before_result))
    {
      std::cerr << "Reading original destinations failed: " << error->_message << " at " << error->_path
                << " (code " << static_cast<int>(error->_code) << ")" << '\n';
      return 1;
    }
    const auto before = std::get<PublishingQueueSetDescriptor>(std::move(before_result));
    auto feed_task_result = created._session.attach_task(feed);
    if (const auto* error = std::get_if<Error>(&feed_task_result))
    {
      std::cerr << "Attaching Feed failed: " << error->_message << " at " << error->_path
                << " (code " << static_cast<int>(error->_code) << ")" << '\n';
      return 1;
    }
    auto feed_task = std::get<Task>(std::move(feed_task_result));
    const std::vector<QueueId> destinations{risk_quotes};
    auto replacement_result = feed_task.registry().set_destinations(quote_publisher, destinations);
    if (const auto* error = std::get_if<Error>(&replacement_result))
    {
      std::cerr << "Replacing destinations failed: " << error->_message << " at " << error->_path
                << " (code " << static_cast<int>(error->_code) << ")" << '\n';
      return 1;
    }
    auto after_result = registry.describe_publishing_set(quote_publisher);
    if (const auto* error = std::get_if<Error>(&after_result))
    {
      std::cerr << "Reading changed destinations failed: " << error->_message << " at " << error->_path
                << " (code " << static_cast<int>(error->_code) << ")" << '\n';
      return 1;
    }
    const auto after = std::get<PublishingQueueSetDescriptor>(std::move(after_result));
    auto metadata_result = registry.metadata(quote_publisher);
    if (const auto* error = std::get_if<Error>(&metadata_result))
    {
      std::cerr << "Reading publishing metadata failed: " << error->_message << " at " << error->_path
                << " (code " << static_cast<int>(error->_code) << ")" << '\n';
      return 1;
    }
    const auto metadata = std::get<RegistrationMetadata>(std::move(metadata_result));
    std::cout << "QuoteUpdates destinations: " << before._destinations.size()
              << " -> " << after._destinations.size() << '\n';
    std::cout << "QuoteUpdates Registry revision=" << metadata._revision << '\n';
    std::cout << "Configured four queues and two publishing sets in one process.\n";
    std::cout << "No messages were sent; priorities and routes are recorded metadata.\n";
    return 0;
  }
  catch (const std::exception& error)
  {
    std::cerr << argv[1] << ": " << error.what() << '\n';
    return 1;
  }
}
