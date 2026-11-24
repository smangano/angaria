// Independent process harness for Field/schema registration. Normal modes use the public
// Registry; fault modes invoke its shared private implementation with qualification
// hooks so the parent can stop at a real resource, journal, or recovery boundary.
// Stdout carries one-byte markers followed by native u64 values on this same host.

#include "angaria/runtime.h"
#include "runtime_state.h"

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <variant>

#include <poll.h>
#include <signal.h>
#include <unistd.h>

namespace
{
  using namespace angaria;
  namespace runtime = angaria::detail;
  namespace mapped = angaria::detail::mapped;

  template <typename T>
  T require_value(Result<T> result)
  {
    if (const auto* error = std::get_if<Error>(&result))
    {
      throw std::runtime_error(error->_path + ": " + error->_message);
    }
    return std::get<T>(std::move(result));
  }

  void report(const void* data, std::size_t length)
  {
    const auto* bytes = static_cast<const char*>(data);
    while (length != 0)
    {
      const auto count = ::write(STDOUT_FILENO, bytes, length);
      if (count > 0)
      {
        bytes += count;
        length -= static_cast<std::size_t>(count);
      }
      else if (count != -1 || errno != EINTR)
      {
        throw std::runtime_error("registration worker report failed");
      }
    }
  }

  void report(char value)
  {
    report(&value, 1);
  }

  void report(std::uint64_t value)
  {
    report(&value, sizeof(value));
  }

  std::uint64_t identity(FieldId id)
  {
    return static_cast<std::uint64_t>(id);
  }

  std::uint64_t identity(SchemaId id)
  {
    return (static_cast<std::uint64_t>(id._type) << 32) | id._version;
  }

  template <typename T>
  void report_result(const Result<T>& result)
  {
    if (const auto* error = std::get_if<Error>(&result))
    {
      report('E');
      report(static_cast<std::uint64_t>(error->_code));
    }
    else
    {
      report('A');
      report(identity(std::get<T>(result)));
    }
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
      throw std::runtime_error("registration worker continuation deadline expired");
    }
    char value{};
    ssize_t count;
    do
    {
      count = ::read(STDIN_FILENO, &value, 1);
    } while (count == -1 && errno == EINTR);
    if (count == -1)
    {
      throw std::runtime_error("registration worker continuation failed");
    }
  }

  [[noreturn]] void stop_for_kill()
  {
    report('R');
    for (;;)
    {
      ::pause();
    }
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
        stop_for_kill();
      }
      return std::monostate{};
    }

  private:
    mapped::JournalCheckpoint _point;
    std::uint64_t _index;
  };

  // A one-shot failure permits execute's in-call repair. The persistent case
  // also interrupts accepted redo, forcing OutcomeUnknown until another caller
  // recovers without this injector. No application callback uses these hooks.
  class IoFailure : public mapped::JournalFaultInjector
  {
  public:
    explicit IoFailure(std::string mode) : _mode(std::move(mode))
    {
    }

    OperationResult checkpoint(mapped::JournalCheckpoint point, std::uint64_t) override
    {
      if (_mode == "io_unknown")
      {
        if (point == mapped::JournalCheckpoint::Committed)
        {
          _failed = true;
        }
        if (_failed && (point == mapped::JournalCheckpoint::Committed || point == mapped::JournalCheckpoint::RedoWord))
        {
          return failure();
        }
      }
      else if (!_failed && point == (_mode == "io_before" ? mapped::JournalCheckpoint::FileGrowth :
                                                            mapped::JournalCheckpoint::Committed))
      {
        _failed = true;
        return failure();
      }
      return std::monostate{};
    }

  private:
    static OperationResult failure()
    {
      return Error{ErrorCode::IoError, "Injected registration I/O failure", "test.schema"};
    }

    std::string _mode;
    bool _failed{};
  };
} // namespace

int main(int argc, char** argv)
{
  try
  {
    if (argc != 11)
    {
      throw std::runtime_error("usage: native_schema_worker mode directory actor kind identity version name doc checkpoint index");
    }
    ::alarm(9);
    const std::string mode{argv[1]};
    const auto actor = static_cast<TaskId>(std::stoul(argv[3]));
    const bool isField = std::string{argv[4]} == "field";
    const auto raw = std::stoull(argv[5]);
    const auto version = static_cast<std::uint32_t>(std::stoul(argv[6]));
    const auto point = static_cast<mapped::JournalCheckpoint>(std::stoul(argv[9]));
    const auto index = std::stoull(argv[10]);
    StopAt stop(point, index);
    if (mode == "recover")
    {
      // Open the journal directly so public attachment does not finish the
      // pending operation before this worker installs its recovery checkpoint.
      auto controller = require_value(mapped::SessionController::open_existing(argv[2]));
      auto guard = require_value(controller->acquire_mutation());
      auto journal = require_value(mapped::MutationJournal::open(controller, guard, &stop));
      require_value(journal.recover(guard));
      throw std::runtime_error("schema recovery did not reach the requested checkpoint");
    }

    FieldDescriptor field;
    field._id = static_cast<FieldId>(raw);
    field._name = argv[7];
    field._doc = argv[8];
    field._base = EFieldBase::BASE_INT64;
    field._size = 8;
    MessageDescriptor schema;
    schema._schema = SchemaId{static_cast<MessageType>(raw), version};
    schema._name = argv[7];
    schema._doc = argv[8];
    schema._fields = {static_cast<FieldId>(0xee000001), static_cast<FieldId>(0xee000001)};
    auto session = require_value(Session::attach(argv[2]));
    if (mode == "inspect")
    {
      auto registry = session.registry();
      RegistrationMetadata metadata;
      std::uint64_t value{};
      std::uint64_t count{};
      if (isField)
      {
        const auto id = require_value(registry.find_field(field._name));
        const auto recorded = require_value(registry.describe_field(id));
        metadata = require_value(registry.metadata(id));
        if (id != field._id || recorded._base != field._base || recorded._size != 8 || recorded._doc != field._doc)
        {
          throw std::runtime_error("independent reader found inconsistent Field metadata");
        }
        value = identity(id);
        count = recorded._size;
      }
      else
      {
        const auto id = require_value(registry.find_message(schema._name, version));
        const auto recorded = require_value(registry.describe_message(id));
        metadata = require_value(registry.metadata(id));
        if (id != schema._schema || recorded._fields != schema._fields || recorded._doc != schema._doc)
        {
          throw std::runtime_error("independent reader found inconsistent message schema");
        }
        value = identity(id);
        count = recorded._fields.size();
      }
      if (metadata._createdBy != actor)
      {
        throw std::runtime_error("schema attribution changed across processes");
      }
      report('I');
      report(value);
      report(static_cast<std::uint64_t>(metadata._createdBy));
      report(metadata._revision);
      report(count);
      return 0;
    }
    if (mode == "register")
    {
      auto owner = require_value(session.attach_task(actor));
      auto controller = require_value(mapped::SessionController::open_existing(argv[2]));
      report('R');
      wait_for_parent();
      {
        // Both contenders report a busy common gate before the parent opens it.
        auto gate = controller->try_acquire_mutation();
        const auto* error = std::get_if<Error>(&gate);
        report(error && error->_code == ErrorCode::OwnershipBusy ? 'B' : 'G');
      }
      if (isField)
      {
        report_result(owner.registry().register_field(field));
      }
      else
      {
        report_result(owner.registry().register_message(schema));
      }
      wait_for_parent();
      return 0;
    }

    // Fault modes share the implementation behind the public methods, with a
    // genuinely attached actor. Only the private test hook supplies failures.
    auto state = std::make_shared<runtime::SessionState>();
    state->_directory = std::filesystem::canonical(argv[2]);
    state->_controller = require_value(mapped::SessionController::open_existing(argv[2]));
    auto owner = require_value(runtime::attach_runtime_task(state, actor));
    const auto perform = [&](mapped::JournalFaultInjector* injector)
    {
      if (isField)
      {
        report_result(runtime::runtime_register_field(state, owner, field, injector));
      }
      else
      {
        report_result(runtime::runtime_register_message(state, owner, schema, injector));
      }
    };
    if (mode == "crash")
    {
      perform(&stop);
      throw std::runtime_error("schema registration did not reach the requested checkpoint");
    }
    if (mode == "io_before" || mode == "io_after" || mode == "io_unknown")
    {
      IoFailure failure(mode);
      perform(&failure);
      wait_for_parent();
      perform(nullptr);
      wait_for_parent();
      return 0;
    }
    throw std::runtime_error("unknown schema worker mode");
  }
  catch (const std::exception& error)
  {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
