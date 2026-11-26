// Independent producer/consumer for combined failure qualification. Public
// handles perform application operations. The explicitly labeled journal seams
// stop native stores; they are not a second application-facing transport API.
#include "angaria/runtime.h"
#include "angaria/payload.h"
#include "native_fixture.h"
#include "process_support.h"
#include "publication_runtime.h"
#include "source_runtime_state.h"
#include "topology_state.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <span>
#include <utility>
#include <variant>
#include <vector>

#include <unistd.h>

namespace
{
  using namespace angaria;
  namespace runtime = angaria::detail;
  namespace mapped = runtime::mapped;
  namespace support = angaria::test_support;
  using support::require_value;

  class StopAt final : public mapped::JournalFaultInjector
  {
  public:
    StopAt(mapped::JournalCheckpoint point, std::uint64_t index) : _point(point), _index(index)
    {
    }

    OperationResult checkpoint(mapped::JournalCheckpoint point, std::uint64_t index) override
    {
      if (point == _point && index == _index)
      {
        support::stop_for_kill();
      }
      return std::monostate{};
    }

  private:
    mapped::JournalCheckpoint _point;
    std::uint64_t _index;
  };

  MessageView prepared(SchemaId schema, std::span<const std::byte> bytes)
  {
    MessageHeader header;
    header._schema = schema;
    header._payloadBytes = bytes.size();
    return {header, bytes};
  }

  void report_token(char stage, const DeliveryToken& token)
  {
    support::report(stage);
    support::report(token._consumer._incarnation);
    support::report(token._sequence);
    support::report(token._issuance);
  }

  void report_publication(char stage, PublicationId id)
  {
    support::report(stage);
    support::report(id._producer._incarnation);
    support::report(id._sequence);
  }

  Delivery delivery(ReceiveResult result)
  {
    if (const auto* error = std::get_if<Error>(&result))
    {
      throw std::runtime_error(error->_path + ": " + error->_message);
    }
    if (!std::holds_alternative<Delivery>(result))
    {
      throw std::runtime_error("Transport worker expected a Delivery");
    }
    return std::get<Delivery>(std::move(result));
  }

  void producer(const char* directory, bool interrupted, std::uint32_t value, StopAt& stop)
  {
    auto session = require_value(Session::attach(directory));
    auto task = require_value(session.attach_task("Producer"));
    auto writer = require_value(task.open_writer("Updates"));
    const auto queue = writer.id();
    const auto schema = require_value(session.registry().find_message("Sample", 1));
    const auto codec = require_value(BinaryPayloadCodec::from_registry(session.registry(), schema, 4));
    const auto claim = task.claim();
    // Two ordinary accepts wrap the ring while the independent consumer still
    // owns its first borrow. Only the final append has a private crash hook.
    if (interrupted)
    {
      for (std::uint32_t item = 2; item < value; ++item)
      {
        const auto bytes = require_value(codec.encode({item}));
        const auto id = require_value(task.next_publication_id());
        if (require_value(writer.send(id, prepared(schema, bytes)))._status != AcceptanceStatus::Accepted)
        {
          throw std::runtime_error("Transport worker expected accepted backlog");
        }
      }
    }
    const auto bytes = require_value(codec.encode({value}));
    const auto id = require_value(task.next_publication_id());
    if (!interrupted)
    {
      if (require_value(writer.send(id, prepared(schema, bytes)))._status != AcceptanceStatus::Accepted)
      {
        throw std::runtime_error("Transport worker expected a fresh acceptance");
      }
      report_publication('S', id);
      return;
    }
    report_publication('I', id);
    runtime::RuntimeAccess access(support::open_runtime_state(directory), true, &stop);
    std::array<DestinationOutcome, 1> outcomes;
    const auto result = runtime::runtime_send(access, claim, queue, id, prepared(schema, bytes), outcomes);
    throw std::runtime_error("Producer did not reach the journal cut: " +
        std::to_string(static_cast<unsigned>(result._error)));
  }

  void consumer(const char* directory, bool acknowledge, std::uint32_t value, StopAt& stop)
  {
    auto session = require_value(Session::attach(directory));
    auto task = require_value(session.attach_task("Consumer"));
    runtime::SourceOperationHooks hooks;
    auto source = require_value(task.open_source_set("Inputs"));
    const auto first = delivery(source.receive());
    if (acknowledge)
    {
      report_token('A', first._token);
      hooks._journal = &stop;
      runtime::SourceAccess::set_hooks(source, &hooks);
      require_value(source.acknowledge(first._token));
      throw std::runtime_error("Acknowledgment did not reach its journal cut");
    }
    const auto header = first._message._header;
    const auto* address = first._message._payload.data();
    const std::vector<std::byte> bytes(first._message._payload.begin(), first._message._payload.end());
    report_token('P', first._token);
    support::wait_for_parent();
    // Receive performs any producer recovery before returning this same pin.
    // Compare the complete header and bytes inside the owning child process.
    const auto repeated = delivery(source.receive());
    const auto schema = require_value(session.registry().find_message("Sample", 1));
    const auto codec = require_value(BinaryPayloadCodec::from_registry(session.registry(), schema, 4));
    const auto decoded = require_value(codec.decode(repeated._message._payload));
    if (repeated._token != first._token || repeated._message._header != header ||
        repeated._message._payload.data() != address ||
        std::vector<std::byte>(repeated._message._payload.begin(), repeated._message._payload.end()) != bytes ||
        decoded.size() != 1 || std::get<std::uint32_t>(decoded.front()) != value)
    {
      throw std::runtime_error("Recovery or ring overwrite changed a live consumer borrow");
    }
    support::report('B');
    support::stop_for_kill();
  }
}

int main(int argc, char** argv)
{
  try
  {
    if (argc != 6)
    {
      throw std::runtime_error("usage: native_transport_worker directory mode checkpoint index value");
    }
    ::alarm(8);
    const std::string_view mode{argv[2]};
    StopAt stop(static_cast<mapped::JournalCheckpoint>(std::stoul(argv[3])), std::stoull(argv[4]));
    const auto value = static_cast<std::uint32_t>(std::stoul(argv[5]));
    if (mode == "recover")
    {
      runtime::RuntimeAccess access(support::open_runtime_state(argv[1]), true, &stop);
      throw std::runtime_error("Recovery did not reach its journal cut");
    }
    if (mode == "cleanup")
    {
      // The parent has already killed the native owner. Submit its captured
      // ReleaseClaim intent directly so stores can be interrupted. A later
      // replacement still acquires its claim through public attach_task().
      runtime::RuntimeAccess access(support::open_runtime_state(argv[1]), true, &stop);
      const auto task = runtime::runtime_task(access, TaskId{static_cast<std::uint16_t>(value)});
      const auto incarnation = access.read_words(task._stateOffset, 1).front();
      mapped::TransactionPlan plan;
      plan._base = access._snapshot;
      plan._kind = mapped::JournalKind::FixedState;
      plan._actor = task._definition._id;
      plan._actorIncarnation = incarnation;
      plan._newRevision = access._snapshot._revision;
      const std::array<std::uint64_t, 2> captured{incarnation, 0};
      auto command = runtime::runtime_write_words(task._stateOffset, captured);
      command._opcode = mapped::RedoOpcode::ReleaseClaim;
      plan._commands.push_back(std::move(command));
      require_value(access._journal.execute(access._guard, plan));
      throw std::runtime_error("Claim cleanup did not reach its journal cut");
    }
    if (mode == "hold")
    {
      auto session = require_value(Session::attach(argv[1]));
      auto task = require_value(session.attach_task("Producer"));
      support::report('H');
      support::report(task.claim()._incarnation);
      support::stop_for_kill();
    }
    if (mode == "produce" || mode == "send")
    {
      producer(argv[1], mode == "produce", value, stop);
      return 0;
    }
    if (mode == "pin" || mode == "ack")
    {
      consumer(argv[1], mode == "ack", value, stop);
      return 0;
    }
    throw std::runtime_error("Unknown native transport worker mode");
  }
  catch (...)
  {
    const auto error = runtime::runtime_caught_error();
    std::cerr << error._path << ": " << error._message << '\n';
    return 1;
  }
}
