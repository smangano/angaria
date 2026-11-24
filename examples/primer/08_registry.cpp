// Independent programs build and inspect one persistent Registry. Snapshots
// record definitions and topology; this example does not transfer messages.
#include "example_support.h"

#include <charconv>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace
{
  using namespace angaria;

  const char* state_name(EntityState state)
  {
    return state == EntityState::Active ? "Active" : "Draft";
  }

  template <typename Id>
  void print_ids(const std::vector<Id>& ids)
  {
    for (std::size_t index = 0; index < ids.size(); ++index)
    {
      if (index != 0)
      {
        std::cout << ',';
      }
      std::cout << static_cast<std::uint64_t>(ids[index]);
    }
  }

  void initialize(const std::filesystem::path& file)
  {
    auto created = primer::value(Session::create_from_file(file, "Setup"));
    auto registry = created._creator.registry();
    for (const auto* name : {"RegistrarA", "RegistrarB"})
    {
      TaskDescriptor descriptor;
      descriptor._name = name;
      primer::value(registry.register_task(descriptor));
    }
    std::cout << "INITIALIZED " << primer::value(created._session.options())._directory << '\n';
    std::cout << "Initial Registry revision=" << primer::value(registry.snapshot())._revision << '\n';
  }

  void register_definitions(const std::filesystem::path& directory, const std::string& label, bool wait)
  {
    if (label != "A" && label != "B")
    {
      throw std::invalid_argument("Registrar label must be A or B");
    }
    auto session = primer::value(Session::attach(directory));
    auto reader = session.registry();
    auto task = primer::value(session.attach_task(primer::value(reader.find_task("Registrar" + label))));
    auto registry = task.registry();
    if (wait)
    {
      // A test or a person can release both registrars after both have attached.
      std::cout << "READY registrar=" << label << std::endl;
      std::string line;
      if (!std::getline(std::cin, line))
      {
        throw std::runtime_error("Registrar input closed before its start signal");
      }
    }

    TaskDescriptor shared;
    shared._name = "Shared";
    shared._doc = "An identical registration from either registrar";
    const auto shared_id = primer::value(registry.register_task(shared));

    FieldDescriptor field;
    field._id = static_cast<FieldId>(label == "A" ? 0xee000010 : 0xee000011);
    field._name = "Metric" + label;
    field._base = EFieldBase::BASE_UINT32;
    field._size = 4;
    primer::value(registry.register_field(field));
    MessageDescriptor record;
    record._schema = {static_cast<MessageType>(label == "A" ? 2 : 3), 1};
    record._name = "Record" + label;
    record._fields = {field._id};
    primer::value(registry.register_message(record));

    // Both processes retry this same new version. Version 1 remains available.
    MessageDescriptor quote;
    quote._schema = {static_cast<MessageType>(1), 2};
    quote._name = "Quote";
    quote._fields = {primer::value(reader.find_field("Quantity")), primer::value(reader.find_field("PriceTicks"))};
    quote._doc = "Quantity and price, in that order";
    primer::value(registry.register_message(quote));

    QueueDescriptor queue;
    queue._name = "Extra" + label;
    queue._producer = primer::value(reader.find_task("Feed"));
    queue._sourceSet = primer::value(reader.find_source_set("ConsumerInputs"));
    queue._priority = label == "A" ? 2 : 3;
    const auto queue_id = primer::value(registry.register_queue(queue));
    primer::value(registry.activate_queue(queue_id));

    // A different descriptor under one name is a conflict, even when the two
    // callers are different Tasks. The first committed definition wins.
    TaskDescriptor contested;
    contested._name = "Contested";
    contested._doc = "Proposed by registrar " + label;
    const auto result = registry.register_task(contested);
    if (const auto* error = std::get_if<Error>(&result))
    {
      if (error->_code != ErrorCode::Conflict)
      {
        primer::value(result);
      }
      std::cout << "CONFLICT rejected=" << label << '\n';
    }
    else
    {
      std::cout << "CONFLICT accepted=" << label << '\n';
    }
    std::cout << "REGISTERED registrar=" << label << " shared=" << static_cast<unsigned>(shared_id)
              << " queue=" << static_cast<unsigned>(queue_id) << '\n';
  }

  void inspect(const std::filesystem::path& directory, std::optional<std::uint64_t> revision)
  {
    auto session = primer::value(Session::attach(directory));
    auto registry = session.registry();
    const auto snapshot = primer::value(registry.snapshot(revision));
    std::cout << "SNAPSHOT revision=" << snapshot._revision << " current=" << snapshot._currentRevision
              << " tasks=" << snapshot._tasks.size() << " queues=" << snapshot._queues.size()
              << " sources=" << snapshot._sources.size() << " publishing=" << snapshot._publishing.size()
              << " fields=" << snapshot._fields.size() << " messages=" << snapshot._messages.size() << '\n';
    for (const auto& entry : snapshot._tasks)
    {
      std::cout << "TASK id=" << static_cast<unsigned>(entry._id) << " name=" << entry._descriptor._name
                << " registered=" << entry._registration._revision << '\n';
    }
    for (const auto& entry : snapshot._fields)
    {
      std::cout << "FIELD id=" << static_cast<std::uint32_t>(entry._descriptor._id)
                << " name=" << entry._descriptor._name << " bytes=" << entry._descriptor._size << '\n';
    }
    for (const auto& entry : snapshot._messages)
    {
      std::cout << "MESSAGE type=" << static_cast<unsigned>(entry._descriptor._schema._type)
                << " version=" << entry._descriptor._schema._version << " name=" << entry._descriptor._name << " fields=";
      print_ids(entry._descriptor._fields);
      std::cout << '\n';
    }
    for (const auto& entry : snapshot._sources)
    {
      std::cout << "SOURCE id=" << static_cast<unsigned>(entry._id) << " name=" << entry._descriptor._name
                << " owner=" << static_cast<unsigned>(entry._descriptor._owner)
                << " state=" << state_name(entry._state) << " members=";
      print_ids(entry._members);
      std::cout << '\n';
    }
    for (const auto& entry : snapshot._queues)
    {
      std::cout << "QUEUE id=" << static_cast<unsigned>(entry._id) << " name=" << entry._descriptor._name
                << " producer=" << static_cast<unsigned>(entry._descriptor._producer)
                << " source=" << static_cast<unsigned>(entry._descriptor._sourceSet)
                << " state=" << state_name(entry._state) << '\n';
    }
    for (const auto& entry : snapshot._publishing)
    {
      std::cout << "PUBLISHING id=" << static_cast<unsigned>(entry._id) << " name=" << entry._descriptor._name
                << " owner=" << static_cast<unsigned>(entry._descriptor._owner)
                << " state=" << state_name(entry._state) << " topology=" << entry._topologyRevision << " destinations=";
      print_ids(entry._descriptor._destinations);
      std::cout << '\n';
    }
    TaskDescriptor forbidden;
    forbidden._name = "CannotRegisterThroughAnInspector";
    const auto attempted = registry.register_task(forbidden);
    const auto* error = std::get_if<Error>(&attempted);
    if (!error || error->_code != ErrorCode::ReadOnlyRegistry)
    {
      throw std::runtime_error("Expected ReadOnlyRegistry from the Session inspection view");
    }
    std::cout << "READ_ONLY ReadOnlyRegistry\n";
    if (!revision)
    {
      // Claims change independently of Registry revisions. Label them as a
      // separate current query, never as historical snapshot information.
      const auto feed = primer::value(registry.find_task("Feed"));
      const auto status = primer::value(registry.task_status(feed));
      std::cout << "CURRENT task=Feed attached=" << (status._attached ? "yes" : "no")
                << " incarnation=" << status._incarnation << '\n';
    }
  }

  void run_owner(const std::filesystem::path& directory, bool hold)
  {
    auto session = primer::value(Session::attach(directory));
    auto registry = session.registry();
    std::optional<QueueWriter> writer;
    std::optional<PublishingQueueSet> publishing;
    std::uint64_t incarnation{};
    {
      auto task = primer::value(session.attach_task(primer::value(registry.find_task("Feed"))));
      incarnation = task.claim()._incarnation;
      writer.emplace(primer::value(task.open_writer(primer::value(registry.find_queue("FeedToConsumer")))));
      publishing.emplace(primer::value(task.open_publishing_set(primer::value(registry.find_publishing_set("FeedUpdates")))));
    }
    // Endpoint handles keep Feed claimed after the Task variable has ended.
    std::cout << "READY owner incarnation=" << incarnation << std::endl;
    if (hold)
    {
      std::string line;
      std::getline(std::cin, line);
    }
  }

  void change_route(const std::filesystem::path& directory)
  {
    auto session = primer::value(Session::attach(directory));
    auto reader = session.registry();
    auto feed = primer::value(session.attach_task(primer::value(reader.find_task("Feed"))));
    const std::vector<QueueId> destinations{
        primer::value(reader.find_queue("ExtraA")), primer::value(reader.find_queue("ExtraB"))};
    primer::value(feed.registry().set_destinations(primer::value(reader.find_publishing_set("FeedUpdates")), destinations));
    std::cout << "ROUTE destinations=ExtraA,ExtraB revision=" << primer::value(reader.snapshot())._revision << '\n';
  }

  std::uint64_t parse_revision(std::string_view text)
  {
    std::uint64_t result{};
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), result);
    if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size())
    {
      throw std::invalid_argument("Revision must be an unsigned decimal integer");
    }
    return result;
  }

  void usage()
  {
    std::cerr << "Usage: angaria_primer_08_registry init <configuration.toml>\n"
              << "       angaria_primer_08_registry register <session-directory> <A|B> [--wait]\n"
              << "       angaria_primer_08_registry inspect <session-directory> [revision]\n"
              << "       angaria_primer_08_registry owner <session-directory> [--hold]\n"
              << "       angaria_primer_08_registry route <session-directory>\n";
  }
} // namespace

int main(int argc, char** argv)
{
  if (argc < 3)
  {
    usage();
    return 1;
  }
  try
  {
    const std::string mode = argv[1];
    if (mode == "init" && argc == 3)
    {
      initialize(argv[2]);
    }
    else if (mode == "register" && (argc == 4 || (argc == 5 && std::string_view{argv[4]} == "--wait")))
    {
      register_definitions(argv[2], argv[3], argc == 5);
    }
    else if (mode == "inspect" && (argc == 3 || argc == 4))
    {
      inspect(argv[2], argc == 4 ? std::optional{parse_revision(argv[3])} : std::nullopt);
    }
    else if (mode == "owner" && (argc == 3 || (argc == 4 && std::string_view{argv[3]} == "--hold")))
    {
      run_owner(argv[2], argc == 4);
    }
    else if (mode == "route" && argc == 3)
    {
      change_route(argv[2]);
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
