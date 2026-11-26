// Independent exec client for the complete Registry qualification. Ordinary
// modes use public APIs; crash/recover modes alone install private checkpoints.
#include "process_support.h"
#include "result_helpers.h"
#include "angaria/runtime.h"
#include "runtime_state.h"
#include "topology_state.h"

#include <algorithm>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <map>
#include <memory>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>
#include <variant>

#include <poll.h>
#include <unistd.h>

namespace
{
  using namespace angaria;
  namespace runtime = angaria::detail;
  namespace mapped = runtime::mapped;

  using angaria::test_support::require_value;

  void check(bool condition)
  {
    if (!condition)
    {
      throw std::runtime_error("independent Registry snapshot has an inconsistent relationship");
    }
  }

  void report(const void* data, std::size_t length)
  {
    angaria::test_support::write_all(STDOUT_FILENO, data, length, "Registry worker report failed");
  }

  void report(char value)
  {
    report(&value, sizeof(value));
  }

  void report(std::uint64_t value)
  {
    report(&value, sizeof(value));
  }

  void wait_for_parent()
  {
    pollfd event{STDIN_FILENO, POLLIN, 0};
    int ready;
    do
    {
      ready = ::poll(&event, 1, 5000);
    } while (ready == -1 && errno == EINTR);
    if (ready <= 0)
    {
      throw std::runtime_error("Registry worker continuation deadline expired");
    }
    char value{};
    ssize_t count;
    do
    {
      count = ::read(STDIN_FILENO, &value, 1);
    } while (count == -1 && errno == EINTR);
    if (count == -1)
    {
      throw std::runtime_error("Registry worker continuation failed");
    }
  }

  void check_snapshot(const RegistrySnapshot& snapshot)
  {
    std::set<TaskId> tasks;
    std::set<FieldId> fields;
    std::map<QueueId, const QueueSnapshot*> queues;
    std::map<SourceQueueSetId, const SourceSnapshot*> sources;
    for (const auto& entry : snapshot._tasks)
    {
      check(tasks.insert(entry._id).second);
      check(entry._registration._revision <= snapshot._revision);
    }
    for (const auto& entry : snapshot._fields)
    {
      check(fields.insert(entry._descriptor._id).second);
      check(tasks.contains(entry._registration._createdBy));
    }
    for (const auto& entry : snapshot._queues)
    {
      check(queues.emplace(entry._id, &entry).second);
      check(entry._descriptor._producer == TaskId::Invalid || tasks.contains(entry._descriptor._producer));
      check(entry._registration._revision <= snapshot._revision);
    }
    for (const auto& entry : snapshot._sources)
    {
      check(sources.emplace(entry._id, &entry).second);
      check(entry._descriptor._owner == TaskId::Invalid || tasks.contains(entry._descriptor._owner));
      for (const auto id : entry._members)
      {
        check(queues.contains(id));
        check(queues.at(id)->_descriptor._sourceSet == entry._id);
      }
    }
    for (const auto& [id, queue] : queues)
    {
      if (queue->_descriptor._sourceSet != SourceQueueSetId::Invalid)
      {
        check(sources.contains(queue->_descriptor._sourceSet));
        const auto& members = sources.at(queue->_descriptor._sourceSet)->_members;
        check(std::count(members.begin(), members.end(), id) == 1);
      }
    }
    for (const auto& entry : snapshot._publishing)
    {
      for (const auto destination : entry._descriptor._destinations)
      {
        check(queues.contains(destination));
        check(queues.at(destination)->_descriptor._producer == entry._descriptor._owner);
        check(entry._state != EntityState::Active || queues.at(destination)->_state == EntityState::Active);
      }
    }
    for (const auto& entry : snapshot._messages)
    {
      for (const auto field : entry._descriptor._fields)
      {
        check(fields.contains(field));
      }
      check(entry._registration._revision <= snapshot._revision);
    }
  }

  void inspect(Session& session)
  {
    auto reader = session.registry();
    const auto snapshot = require_value(reader.snapshot());
    check_snapshot(snapshot);
    const auto bootstrap = require_value(reader.snapshot(1));
    check_snapshot(bootstrap);
    std::uint64_t attached{};
    for (const auto& task : snapshot._tasks)
    {
      attached += require_value(reader.task_status(task._id))._attached ? 1 : 0;
    }
    report('I');
    for (const auto value : {snapshot._revision, static_cast<std::uint64_t>(snapshot._tasks.size()),
                            static_cast<std::uint64_t>(snapshot._fields.size()),
                            static_cast<std::uint64_t>(snapshot._messages.size()),
                            static_cast<std::uint64_t>(snapshot._queues.size()),
                            static_cast<std::uint64_t>(snapshot._sources.size()),
                            static_cast<std::uint64_t>(snapshot._publishing.size()), attached,
                            static_cast<std::uint64_t>(bootstrap._tasks.size())})
    {
      report(value);
    }
  }

  void register_graph(Task& actor, const std::string& prefix)
  {
    auto registry = actor.registry();
    TaskDescriptor participant;
    participant._name = prefix + "Participant";
    const auto task = require_value(registry.register_task(participant));
    FieldDescriptor field;
    const auto actor_number = static_cast<std::uint32_t>(actor.claim()._task);
    field._id = static_cast<FieldId>(0xee010000u + actor_number);
    field._name = prefix + "Price";
    field._base = EFieldBase::BASE_INT64;
    field._size = 8;
    const auto price = require_value(registry.register_field(field));
    MessageDescriptor message;
    message._schema = {static_cast<MessageType>(100 + actor_number), 1};
    message._name = prefix + "Quote";
    message._fields = {price};
    require_value(registry.register_message(message));
    SourceQueueSetDescriptor source;
    source._name = prefix + "Inputs";
    source._owner = task;
    const auto inputs = require_value(registry.register_source_set(source));
    QueueDescriptor queue;
    queue._name = prefix + "Queue";
    queue._producer = task;
    queue._sourceSet = inputs;
    const auto connection = require_value(registry.register_queue(queue));
    PublishingQueueSetDescriptor publishing;
    publishing._name = prefix + "Updates";
    publishing._owner = task;
    publishing._destinations = {connection};
    const auto updates = require_value(registry.register_publishing_set(publishing));
    require_value(registry.activate_source_set(inputs));
    require_value(registry.activate_queue(connection));
    require_value(registry.activate_publishing_set(updates));
    message._schema._version = 2;
    message._fields = {price, price};
    require_value(registry.register_message(message));
  }

  class StopAt : public mapped::JournalFaultInjector
  {
  public:
    StopAt(mapped::JournalCheckpoint point, std::uint64_t index) : _point(point), _index(index)
    {
    }

    OperationResult checkpoint(mapped::JournalCheckpoint point, std::uint64_t index) override
    {
      if (point == _point && index == _index)
      {
        report('R');
        for (;;)
        {
          ::pause();
        }
      }
      return std::monostate{};
    }

  private:
    mapped::JournalCheckpoint _point;
    std::uint64_t _index;
  };
} // namespace

int main(int argc, char** argv)
{
  try
  {
    if (argc != 7)
    {
      throw std::runtime_error("usage: native_registry_worker mode path actor prefix checkpoint index");
    }
    ::alarm(9);
    const std::string mode{argv[1]};
    StopAt stop(static_cast<mapped::JournalCheckpoint>(std::stoul(argv[5])), std::stoull(argv[6]));
    if (mode == "bootstrap")
    {
      auto created = require_value(Session::create_from_file(argv[2], argv[3]));
      report('D');
      report(require_value(created._session.registry().snapshot())._revision);
      return 0;
    }
    if (mode == "recover")
    {
      auto controller = require_value(mapped::SessionController::open_existing(argv[2]));
      auto guard = require_value(controller->acquire_mutation());
      auto journal = require_value(mapped::MutationJournal::open(controller, guard, &stop));
      require_value(journal.recover(guard));
      throw std::runtime_error("Registry recovery did not reach its checkpoint");
    }
    auto session = require_value(Session::attach(argv[2]));
    auto reader = session.registry();
    if (mode == "inspect")
    {
      inspect(session);
      return 0;
    }
    if (mode == "observe")
    {
      report('R');
      wait_for_parent();
      const auto initial = require_value(reader.snapshot());
      check_snapshot(initial);
      report('S');
      report(initial._revision);
      wait_for_parent();
      auto previous = initial._revision;
      for (unsigned sample = 0; sample != 14; ++sample)
      {
        const auto snapshot = require_value(reader.snapshot());
        check_snapshot(snapshot);
        check(snapshot._revision >= previous);
        previous = snapshot._revision;
      }
      report('M');
      wait_for_parent();
      const auto final = require_value(reader.snapshot());
      check_snapshot(final);
      check(final._revision >= previous);
      report('O');
      report(std::uint64_t{16});
      report(final._revision);
      return 0;
    }
    const auto actor_id = require_value(reader.find_task(argv[3]));
    if (mode == "claim")
    {
      auto result = session.attach_task(actor_id);
      if (const auto* error = std::get_if<Error>(&result))
      {
        report('E');
        report(static_cast<std::uint64_t>(error->_code));
      }
      else
      {
        auto task = std::get<Task>(std::move(result));
        report('A');
        report(task.claim()._incarnation);
        wait_for_parent();
      }
      return 0;
    }
    if (mode == "register")
    {
      auto task = require_value(session.attach_task(actor_id));
      report('R');
      wait_for_parent();
      register_graph(task, argv[4]);
      report('D');
      report(require_value(task.registry().snapshot())._revision);
      wait_for_parent();
      return 0;
    }

    // Only these qualification modes use the runtime's private injector. The
    // definitions/actor are normal public setup; SIGKILL interrupts real writes.
    auto state = std::make_shared<runtime::SessionState>();
    state->_directory = std::filesystem::canonical(argv[2]);
    state->_controller = require_value(mapped::SessionController::open_existing(argv[2]));
    auto actor = require_value(runtime::attach_runtime_task(state, actor_id));
    if (mode == "crash_queue")
    {
      QueueDescriptor queue;
      queue._name = "RecoveredQueue";
      queue._producer = actor_id;
      queue._sourceSet = require_value(reader.find_source_set("Inputs"));
      require_value(runtime::runtime_register_queue(state, actor, queue, &stop));
    }
    else if (mode == "crash_schema")
    {
      MessageDescriptor message;
      message._name = "Quote";
      message._schema = {static_cast<MessageType>(7), 2};
      const auto price = require_value(reader.find_field("Price"));
      message._fields = {price, price};
      require_value(runtime::runtime_register_message(state, actor, message, &stop));
    }
    else
    {
      throw std::runtime_error("Unknown Registry worker mode");
    }
    throw std::runtime_error("Registry operation did not reach its checkpoint");
  }
  catch (const std::exception& error)
  {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
