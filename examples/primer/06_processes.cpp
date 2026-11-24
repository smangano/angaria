// Run these roles as independent programs against one Session directory.
// They share definitions and ownership claims; they do not transfer payloads.
#include "example_support.h"

#include <cstdint>
#include <exception>
#include <filesystem>
#include <iostream>
#include <optional>
#include <string>

namespace
{
  using namespace angaria;

  void initialize(const std::filesystem::path& directory)
  {
    TaskDescriptor setup;
    setup._name = "Setup";
    auto created = primer::value(Session::create(
        primer::session_options(directory, "Primer processes"), setup));
    auto registry = created._creator.registry();
    TaskDescriptor producer;
    producer._name = "Feed";
    const auto feed = primer::value(registry.register_task(producer));
    TaskDescriptor consumer;
    consumer._name = "Strategy";
    const auto strategy = primer::value(registry.register_task(consumer));
    SourceQueueSetDescriptor inputs;
    inputs._name = "StrategyInputs";
    inputs._owner = strategy;
    const auto source = primer::value(registry.register_source_set(inputs));
    QueueDescriptor quotes;
    quotes._name = "Quotes";
    quotes._producer = feed;
    quotes._sourceSet = source;
    const auto queue = primer::value(registry.register_queue(quotes));
    PublishingQueueSetDescriptor updates;
    updates._name = "QuoteUpdates";
    updates._owner = feed;
    updates._destinations = {queue};
    const auto publisher = primer::value(registry.register_publishing_set(updates));
    primer::value(registry.activate_source_set(source));
    primer::value(registry.activate_queue(queue));
    primer::value(registry.activate_publishing_set(publisher));
    std::cout << "Initialized Feed -> Quotes -> StrategyInputs.\n";
    // Ending this function closes Setup's claim. The registered definitions
    // stay in the Session so later processes can find them by name.
  }

  void inspect(const std::filesystem::path& directory)
  {
    auto session = primer::value(Session::attach(directory));
    auto registry = session.registry();
    const auto metadata = primer::value(session.metadata());
    std::cout << "Session=" << primer::value(session.options())._name
              << ", Angaria=" << metadata._angariaVersion << '\n';
    for (const auto* name : {"Feed", "Strategy"})
    {
      const auto id = primer::value(registry.find_task(name));
      const auto status = primer::value(registry.task_status(id));
      std::cout << name << " attached=" << (status._attached ? "yes" : "no")
                << " incarnation=" << status._incarnation << '\n';
    }
    const auto inputs = primer::value(registry.find_source_set("StrategyInputs"));
    std::cout << "StrategyInputs members=" << primer::value(registry.source_members(inputs)).size() << '\n';
    // A Session Registry reads definitions without taking a Task claim.
  }

  void wait_if_requested(bool hold)
  {
    if (hold)
    {
      std::cout << "Press Enter to close this role, or terminate the process.\n";
      std::string line;
      std::getline(std::cin, line);
    }
  }

  void run_producer(const std::filesystem::path& directory, bool hold)
  {
    auto session = primer::value(Session::attach(directory));
    auto registry = session.registry();
    const auto feed = primer::value(registry.find_task("Feed"));
    const auto quotes = primer::value(registry.find_queue("Quotes"));
    const auto updates = primer::value(registry.find_publishing_set("QuoteUpdates"));
    std::optional<QueueWriter> writer;
    std::optional<PublishingQueueSet> publisher;
    std::uint64_t incarnation{};
    {
      auto task = primer::value(session.attach_task(feed));
      incarnation = task.claim()._incarnation;
      writer.emplace(primer::value(task.open_writer(quotes)));
      publisher.emplace(primer::value(task.open_publishing_set(updates)));
      const auto publication = primer::value(task.next_publication_id());
      std::cout << "Reserved publication sequence=" << publication._sequence << " (not sent).\n";
    }
    // The Task variable has ended, but the endpoint handles retain its claim.
    std::cout << "Writer=" << static_cast<unsigned>(writer->id())
              << ", publisher=" << static_cast<unsigned>(publisher->id()) << '\n';
    std::cout << "READY producer incarnation=" << incarnation << std::endl;
    wait_if_requested(hold);
  }

  void run_consumer(const std::filesystem::path& directory, bool hold)
  {
    auto session = primer::value(Session::attach(directory));
    auto registry = session.registry();
    const auto strategy = primer::value(registry.find_task("Strategy"));
    const auto inputs = primer::value(registry.find_source_set("StrategyInputs"));
    std::optional<SourceQueueSet> source;
    std::uint64_t incarnation{};
    {
      auto task = primer::value(session.attach_task(strategy));
      incarnation = task.claim()._incarnation;
      source.emplace(primer::value(task.open_source_set(inputs)));
    }
    std::cout << "Source=" << static_cast<unsigned>(source->id()) << '\n';
    std::cout << "READY consumer incarnation=" << incarnation << std::endl;
    wait_if_requested(hold);
  }

  void usage()
  {
    std::cerr << "Usage: angaria_primer_06_processes <init|inspect|producer|consumer> <session-directory> [--hold]\n";
    std::cerr << "Use --hold only with producer or consumer. No messages are transferred yet.\n";
  }
}

int main(int argc, char** argv)
{
  if (argc != 3 && argc != 4)
  {
    usage();
    return 1;
  }
  const std::string command = argv[1];
  const bool hold = argc == 4;
  if (hold && (std::string{argv[3]} != "--hold" || (command != "producer" && command != "consumer")))
  {
    usage();
    return 1;
  }
  try
  {
    if (command == "init")
    {
      initialize(argv[2]);
    }
    else if (command == "inspect")
    {
      inspect(argv[2]);
    }
    else if (command == "producer")
    {
      run_producer(argv[2], hold);
    }
    else if (command == "consumer")
    {
      run_consumer(argv[2], hold);
    }
    else
    {
      usage();
      return 1;
    }
    return 0;
  }
  catch (const std::exception& error)
  {
    std::cerr << argv[2] << ": " << error.what() << '\n';
    return 1;
  }
}
