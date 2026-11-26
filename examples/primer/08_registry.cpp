// Independent programs build and inspect one persistent Registry. Snapshots
// record definitions and topology; this example does not transfer messages.
#include "angaria/runtime.h"

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
#include <utility>
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

  OperationResult initialize(const std::filesystem::path& file)
  {
    auto create_from_file_result = Session::create_from_file(file, "Setup");
    if (const auto* error = std::get_if<angaria::Error>(&create_from_file_result))
    {
      return *error;
    }
    auto created = std::get<angaria::CreatedSession>(std::move(create_from_file_result));
    auto registry = created._creator.registry();
    for (const auto* name : {"RegistrarA", "RegistrarB"})
    {
      TaskDescriptor descriptor;
      descriptor._name = name;
      auto register_task_result = registry.register_task(descriptor);
      if (const auto* error = std::get_if<angaria::Error>(&register_task_result))
      {
        return *error;
      }
    }
    auto options_result = created._session.options();
    if (const auto* error = std::get_if<angaria::Error>(&options_result))
    {
      return *error;
    }
    std::cout << "INITIALIZED " << std::get<angaria::SessionOptions>(std::move(options_result))._directory << '\n';
    auto snapshot_result = registry.snapshot();
    if (const auto* error = std::get_if<angaria::Error>(&snapshot_result))
    {
      return *error;
    }
    std::cout << "Initial Registry revision=" << std::get<angaria::RegistrySnapshot>(std::move(snapshot_result))._revision << '\n';
    return std::monostate{};
  }

  OperationResult register_definitions(const std::filesystem::path& directory, const std::string& label, bool wait)
  {
    if (label != "A" && label != "B")
    {
      throw std::invalid_argument("Registrar label must be A or B");
    }
    auto attach_result = Session::attach(directory);
    if (const auto* error = std::get_if<angaria::Error>(&attach_result))
    {
      return *error;
    }
    auto session = std::get<angaria::Session>(std::move(attach_result));
    auto reader = session.registry();
    auto find_task_result = reader.find_task("Registrar" + label);
    if (const auto* error = std::get_if<angaria::Error>(&find_task_result))
    {
      return *error;
    }
    auto attach_task_result = session.attach_task(std::get<angaria::TaskId>(std::move(find_task_result)));
    if (const auto* error = std::get_if<angaria::Error>(&attach_task_result))
    {
      return *error;
    }
    auto task = std::get<angaria::Task>(std::move(attach_task_result));
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
    auto register_task_result_2 = registry.register_task(shared);
    if (const auto* error = std::get_if<angaria::Error>(&register_task_result_2))
    {
      return *error;
    }
    const auto shared_id = std::get<angaria::TaskId>(std::move(register_task_result_2));

    FieldDescriptor field;
    field._id = static_cast<FieldId>(label == "A" ? 0xee000010 : 0xee000011);
    field._name = "Metric" + label;
    field._base = EFieldBase::BASE_UINT32;
    field._size = 4;
    auto register_field_result = registry.register_field(field);
    if (const auto* error = std::get_if<angaria::Error>(&register_field_result))
    {
      return *error;
    }
    MessageDescriptor record;
    record._schema = {static_cast<MessageType>(label == "A" ? 2 : 3), 1};
    record._name = "Record" + label;
    record._fields = {field._id};
    auto register_message_result = registry.register_message(record);
    if (const auto* error = std::get_if<angaria::Error>(&register_message_result))
    {
      return *error;
    }

    // Both processes retry this same new version. Version 1 remains available.
    MessageDescriptor quote;
    quote._schema = {static_cast<MessageType>(1), 2};
    quote._name = "Quote";
    auto find_field_result = reader.find_field("Quantity");
    if (const auto* error = std::get_if<angaria::Error>(&find_field_result))
    {
      return *error;
    }
    auto find_field_result_2 = reader.find_field("PriceTicks");
    if (const auto* error = std::get_if<angaria::Error>(&find_field_result_2))
    {
      return *error;
    }
    quote._fields = {std::get<angaria::FieldId>(std::move(find_field_result)), std::get<angaria::FieldId>(std::move(find_field_result_2))};
    quote._doc = "Quantity and price, in that order";
    auto register_message_result_2 = registry.register_message(quote);
    if (const auto* error = std::get_if<angaria::Error>(&register_message_result_2))
    {
      return *error;
    }

    QueueDescriptor queue;
    queue._name = "Extra" + label;
    auto find_task_result_2 = reader.find_task("Feed");
    if (const auto* error = std::get_if<angaria::Error>(&find_task_result_2))
    {
      return *error;
    }
    queue._producer = std::get<angaria::TaskId>(std::move(find_task_result_2));
    auto find_source_set_result = reader.find_source_set("ConsumerInputs");
    if (const auto* error = std::get_if<angaria::Error>(&find_source_set_result))
    {
      return *error;
    }
    queue._sourceSet = std::get<angaria::SourceQueueSetId>(std::move(find_source_set_result));
    queue._priority = label == "A" ? 2 : 3;
    auto register_queue_result = registry.register_queue(queue);
    if (const auto* error = std::get_if<angaria::Error>(&register_queue_result))
    {
      return *error;
    }
    const auto queue_id = std::get<angaria::QueueId>(std::move(register_queue_result));
    auto activate_queue_result = registry.activate_queue(queue_id);
    if (const auto* error = std::get_if<angaria::Error>(&activate_queue_result))
    {
      return *error;
    }

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
        return *error;
      }
      std::cout << "CONFLICT rejected=" << label << '\n';
    }
    else
    {
      std::cout << "CONFLICT accepted=" << label << '\n';
    }
    std::cout << "REGISTERED registrar=" << label << " shared=" << static_cast<unsigned>(shared_id)
              << " queue=" << static_cast<unsigned>(queue_id) << '\n';
    return std::monostate{};
  }

  OperationResult inspect(const std::filesystem::path& directory, std::optional<std::uint64_t> revision)
  {
    auto attach_result_2 = Session::attach(directory);
    if (const auto* error = std::get_if<angaria::Error>(&attach_result_2))
    {
      return *error;
    }
    auto session = std::get<angaria::Session>(std::move(attach_result_2));
    auto registry = session.registry();
    auto snapshot_result_2 = registry.snapshot(revision);
    if (const auto* error = std::get_if<angaria::Error>(&snapshot_result_2))
    {
      return *error;
    }
    const auto snapshot = std::get<angaria::RegistrySnapshot>(std::move(snapshot_result_2));
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
    if (!error)
    {
      throw std::runtime_error("The Session inspection view unexpectedly registered a Task");
    }
    if (error->_code != ErrorCode::ReadOnlyRegistry)
    {
      return *error;
    }
    std::cout << "READ_ONLY ReadOnlyRegistry\n";
    if (!revision)
    {
      // Claims change independently of Registry revisions. Label them as a
      // separate current query, never as historical snapshot information.
      auto find_task_result_3 = registry.find_task("Feed");
      if (const auto* error = std::get_if<angaria::Error>(&find_task_result_3))
      {
        return *error;
      }
      const auto feed = std::get<angaria::TaskId>(std::move(find_task_result_3));
      auto task_status_result = registry.task_status(feed);
      if (const auto* error = std::get_if<angaria::Error>(&task_status_result))
      {
        return *error;
      }
      const auto status = std::get<angaria::TaskRuntimeInfo>(std::move(task_status_result));
      std::cout << "CURRENT task=Feed attached=" << (status._attached ? "yes" : "no")
                << " incarnation=" << status._incarnation << '\n';
    }
    return std::monostate{};
  }

  OperationResult run_owner(const std::filesystem::path& directory, bool hold)
  {
    auto attach_result_3 = Session::attach(directory);
    if (const auto* error = std::get_if<angaria::Error>(&attach_result_3))
    {
      return *error;
    }
    auto session = std::get<angaria::Session>(std::move(attach_result_3));
    auto registry = session.registry();
    std::optional<QueueWriter> writer;
    std::optional<PublishingQueueSet> publishing;
    std::uint64_t incarnation{};
    {
      auto find_task_result_4 = registry.find_task("Feed");
      if (const auto* error = std::get_if<angaria::Error>(&find_task_result_4))
      {
        return *error;
      }
      auto attach_task_result_2 = session.attach_task(std::get<angaria::TaskId>(std::move(find_task_result_4)));
      if (const auto* error = std::get_if<angaria::Error>(&attach_task_result_2))
      {
        return *error;
      }
      auto task = std::get<angaria::Task>(std::move(attach_task_result_2));
      incarnation = task.claim()._incarnation;
      auto find_queue_result = registry.find_queue("FeedToConsumer");
      if (const auto* error = std::get_if<angaria::Error>(&find_queue_result))
      {
        return *error;
      }
      auto open_writer_result = task.open_writer(std::get<angaria::QueueId>(std::move(find_queue_result)));
      if (const auto* error = std::get_if<angaria::Error>(&open_writer_result))
      {
        return *error;
      }
      writer.emplace(std::get<angaria::QueueWriter>(std::move(open_writer_result)));
      auto find_publishing_set_result = registry.find_publishing_set("FeedUpdates");
      if (const auto* error = std::get_if<angaria::Error>(&find_publishing_set_result))
      {
        return *error;
      }
      auto open_publishing_set_result = task.open_publishing_set(std::get<angaria::PublishingQueueSetId>(std::move(find_publishing_set_result)));
      if (const auto* error = std::get_if<angaria::Error>(&open_publishing_set_result))
      {
        return *error;
      }
      publishing.emplace(std::get<angaria::PublishingQueueSet>(std::move(open_publishing_set_result)));
    }
    // Endpoint handles keep Feed claimed after the Task variable has ended.
    std::cout << "READY owner incarnation=" << incarnation << std::endl;
    if (hold)
    {
      std::string line;
      std::getline(std::cin, line);
    }
    return std::monostate{};
  }

  OperationResult change_route(const std::filesystem::path& directory)
  {
    auto attach_result_4 = Session::attach(directory);
    if (const auto* error = std::get_if<angaria::Error>(&attach_result_4))
    {
      return *error;
    }
    auto session = std::get<angaria::Session>(std::move(attach_result_4));
    auto reader = session.registry();
    auto find_task_result_5 = reader.find_task("Feed");
    if (const auto* error = std::get_if<angaria::Error>(&find_task_result_5))
    {
      return *error;
    }
    auto attach_task_result_3 = session.attach_task(std::get<angaria::TaskId>(std::move(find_task_result_5)));
    if (const auto* error = std::get_if<angaria::Error>(&attach_task_result_3))
    {
      return *error;
    }
    auto feed = std::get<angaria::Task>(std::move(attach_task_result_3));
    auto find_queue_result_2 = reader.find_queue("ExtraA");
    if (const auto* error = std::get_if<angaria::Error>(&find_queue_result_2))
    {
      return *error;
    }
    auto find_queue_result_3 = reader.find_queue("ExtraB");
    if (const auto* error = std::get_if<angaria::Error>(&find_queue_result_3))
    {
      return *error;
    }
    const std::vector<QueueId> destinations{
        std::get<angaria::QueueId>(std::move(find_queue_result_2)), std::get<angaria::QueueId>(std::move(find_queue_result_3))};
    auto find_publishing_set_result_2 = reader.find_publishing_set("FeedUpdates");
    if (const auto* error = std::get_if<angaria::Error>(&find_publishing_set_result_2))
    {
      return *error;
    }
    auto registry_result = feed.registry().set_destinations(std::get<angaria::PublishingQueueSetId>(std::move(find_publishing_set_result_2)), destinations);
    if (const auto* error = std::get_if<angaria::Error>(&registry_result))
    {
      return *error;
    }
    auto snapshot_result_3 = reader.snapshot();
    if (const auto* error = std::get_if<angaria::Error>(&snapshot_result_3))
    {
      return *error;
    }
    std::cout << "ROUTE destinations=ExtraA,ExtraB revision=" << std::get<angaria::RegistrySnapshot>(std::move(snapshot_result_3))._revision << '\n';
    return std::monostate{};
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
    angaria::OperationResult operation{std::monostate{}};
    const std::string mode = argv[1];
    if (mode == "init" && argc == 3)
    {
      operation = initialize(argv[2]);
    }
    else if (mode == "register" && (argc == 4 || (argc == 5 && std::string_view{argv[4]} == "--wait")))
    {
      operation = register_definitions(argv[2], argv[3], argc == 5);
    }
    else if (mode == "inspect" && (argc == 3 || argc == 4))
    {
      operation = inspect(argv[2], argc == 4 ? std::optional{parse_revision(argv[3])} : std::nullopt);
    }
    else if (mode == "owner" && (argc == 3 || (argc == 4 && std::string_view{argv[3]} == "--hold")))
    {
      operation = run_owner(argv[2], argc == 4);
    }
    else if (mode == "route" && argc == 3)
    {
      operation = change_route(argv[2]);
    }
    else
    {
      usage();
      return 1;
    }
    if (const auto* error = std::get_if<angaria::Error>(&operation))
    {
      if (error->_code == angaria::ErrorCode::Conflict)
      {
        std::cerr << "Conflict: ";
      }
      else if (error->_code == angaria::ErrorCode::OwnershipBusy)
      {
        std::cerr << "OwnershipBusy: ";
      }
      std::cerr << "Angaria error " << static_cast<int>(error->_code)
                << " at " << error->_path << ": " << error->_message << '\n';
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
