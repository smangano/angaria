// Configure two producers, two consumers, and a copy-to-audit route. This
// program records the wiring; message delivery and selection arrive later.
#include "example_support.h"

#include <exception>
#include <iostream>
#include <string>
#include <vector>

namespace
{
  angaria::TaskId register_task(angaria::Registry registry, const std::string& name)
  {
    angaria::TaskDescriptor descriptor;
    descriptor._name = name;
    return primer::value(registry.register_task(descriptor));
  }
}

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
    TaskDescriptor coordinator;
    coordinator._name = "Coordinator";
    auto created = primer::value(Session::create(
        primer::session_options(argv[1], "Primer fan-out"), coordinator));
    auto registry = created._creator.registry();
    const auto feed = register_task(registry, "Feed");
    const auto strategy = register_task(registry, "Strategy");
    const auto risk = register_task(registry, "Risk");
    const auto audit = register_task(registry, "Audit");

    SourceQueueSetDescriptor risk_inputs;
    risk_inputs._name = "RiskInputs";
    risk_inputs._owner = risk;
    const auto risk_source = primer::value(registry.register_source_set(risk_inputs));
    SourceQueueSetDescriptor audit_inputs;
    audit_inputs._name = "AuditInputs";
    const auto audit_source = primer::value(registry.register_source_set(audit_inputs));
    // A draft can be completed later. Adoption changes its logical owner,
    // without requiring the Audit process to be running.
    primer::value(registry.adopt_source_set(audit_source, audit));

    QueueDescriptor quotes_to_risk;
    quotes_to_risk._name = "QuotesToRisk";
    quotes_to_risk._producer = feed;
    quotes_to_risk._sourceSet = risk_source;
    quotes_to_risk._priority = 2;
    const auto risk_quotes = primer::value(registry.register_queue(quotes_to_risk));
    auto orders_to_risk = quotes_to_risk;
    orders_to_risk._name = "OrdersToRisk";
    orders_to_risk._producer = strategy;
    orders_to_risk._priority = 8;
    const auto risk_orders = primer::value(registry.register_queue(orders_to_risk));
    auto quotes_to_audit = quotes_to_risk;
    quotes_to_audit._name = "QuotesToAudit";
    quotes_to_audit._sourceSet = SourceQueueSetId::Invalid;
    quotes_to_audit._priority = 1;
    const auto audit_quotes = primer::value(registry.register_queue(quotes_to_audit));
    // Assignment updates the Queue and its Source's member list together.
    primer::value(registry.assign_source(audit_quotes, audit_source, 1));
    auto orders_to_audit = orders_to_risk;
    orders_to_audit._name = "OrdersToAudit";
    orders_to_audit._sourceSet = audit_source;
    orders_to_audit._priority = 1;
    const auto audit_orders = primer::value(registry.register_queue(orders_to_audit));

    PublishingQueueSetDescriptor quote_updates;
    quote_updates._name = "QuoteUpdates";
    quote_updates._owner = feed;
    quote_updates._destinations = {risk_quotes, audit_quotes};
    const auto quote_publisher = primer::value(registry.register_publishing_set(quote_updates));
    PublishingQueueSetDescriptor order_updates;
    order_updates._name = "OrderUpdates";
    order_updates._owner = strategy;
    order_updates._destinations = {risk_orders, audit_orders};
    const auto order_publisher = primer::value(registry.register_publishing_set(order_updates));

    for (const auto source : {risk_source, audit_source})
    {
      primer::value(registry.activate_source_set(source));
    }
    for (const auto queue : {risk_quotes, risk_orders, audit_quotes, audit_orders})
    {
      primer::value(registry.activate_queue(queue));
      const auto descriptor = primer::value(registry.describe_queue(queue));
      const auto producer = primer::value(registry.describe_task(descriptor._producer));
      const auto input = primer::value(registry.describe_source_set(descriptor._sourceSet));
      std::cout << descriptor._name << ": " << producer._name << " -> " << input._name
                << ", priority=" << static_cast<unsigned>(descriptor._priority) << '\n';
    }
    for (const auto publisher : {quote_publisher, order_publisher})
    {
      primer::value(registry.activate_publishing_set(publisher));
    }
    // Empty overrides inherit the Session's recorded defaults. These names
    // describe the configured policy profile; this program does not run it.
    std::cout << "Policy defaults: Strict selection, Spin wait, RecoverFromLog, AllDestinations.\n";

    // Active Publishing sets can change destinations through their owner.
    // Descriptors own their values: the earlier copy still describes two
    // destinations after the Registry records a new one-destination snapshot.
    const auto before = primer::value(registry.describe_publishing_set(quote_publisher));
    auto feed_task = primer::value(created._session.attach_task(feed));
    const std::vector<QueueId> destinations{risk_quotes};
    primer::value(feed_task.registry().set_destinations(quote_publisher, destinations));
    const auto after = primer::value(registry.describe_publishing_set(quote_publisher));
    const auto metadata = primer::value(registry.metadata(quote_publisher));
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
