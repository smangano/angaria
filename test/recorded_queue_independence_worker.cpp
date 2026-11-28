// Independent peers for connection-independence qualification. Pipes carry only
// barrier/completion reports; payloads and acceptance evidence use the mapped
// Queue and producer log. Every waiting loop and parent barrier has a deadline.
#include "recorded_queue_independence_fixture.h"
#include "process_support.h"
#include "session_controller.h"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <variant>

#include <unistd.h>

namespace
{
  using namespace angaria;
  namespace mapped = angaria::detail::mapped;
  namespace recorded = angaria::detail::recorded;
  namespace support = angaria::test_support;
  using support::require_value;
  using support::require_recorded_send;

  void require(bool condition, const char* message)
  {
    if (!condition)
    {
      throw std::runtime_error(message);
    }
  }

  void yield_before(std::chrono::steady_clock::time_point deadline)
  {
    require(std::chrono::steady_clock::now() < deadline, "Independent connection made no progress before its deadline");
    std::this_thread::yield();
  }

  class PausedAcceptance final : public recorded::RecordedQueueObserver
  {
  public:
    OperationResult checkpoint(recorded::RecordedQueueCheckpoint point) override
    {
      if (point == recorded::RecordedQueueCheckpoint::CommittedBeforeHint)
      {
        support::report('R');
        support::wait_for_parent();
      }
      return std::monostate{};
    }
  };

  class PausedRecovery final : public recorded::RecordedQueueObserver
  {
  public:
    OperationResult checkpoint(recorded::ProducerLogCheckpoint point) override
    {
      if (point == recorded::ProducerLogCheckpoint::RecoveryBeforeScan)
      {
        // The actual producer data-inode claim is held, but no committed prefix
        // has been scanned and no replacement incarnation has been installed.
        support::report('R');
        support::wait_for_parent();
      }
      return std::monostate{};
    }

    OperationResult checkpoint(recorded::RecordedQueueCheckpoint) override
    {
      return std::monostate{};
    }
  };

  void run_producer(const recorded::RecordedQueueOptions& options, unsigned connection)
  {
    auto producer = require_value(recorded::RecordedProducer::open(options));
    support::report('O');
    support::report(require_value(producer.claim())._incarnation);
    support::wait_for_parent();
    const auto codec = support::recorded_queue_codec(options);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{3};
    std::uint64_t checksum{};
    for (std::uint64_t sequence = 1; sequence <= support::independence_message_count; ++sequence)
    {
      const auto id = require_value(producer.next_publication_id());
      const auto bytes = support::recorded_queue_payload(codec, support::independence_payload_value(connection, sequence));
      const auto outgoing = support::recorded_queue_message(options, bytes);
      for (;;)
      {
        const auto sent = producer.try_send(id, outgoing);
        if (std::holds_alternative<recorded::QueueCreditFull>(sent))
        {
          yield_before(deadline);
          continue;
        }
        const auto accepted = require_recorded_send(sent);
        require(!accepted._duplicate && accepted._queueSequence == sequence, "Independent producer returned a wrong sequence");
        const auto lookup = require_value(producer.lookup(id));
        require(lookup && lookup->_reference == accepted._reference && lookup->_queueSequence == sequence,
                "Independent producer cannot resolve its ordinary accepted ID");
        checksum += sequence;
        break;
      }
    }
    support::report('A');
    support::report(support::independence_message_count);
    support::report(checksum);
  }

  template <typename Consumer>
  void run_consumer(const recorded::RecordedQueueOptions& options, unsigned connection)
  {
    auto consumer = require_value(Consumer::open(options));
    support::report('O');
    support::report(require_value(consumer.claim())._incarnation);
    support::wait_for_parent();
    const auto codec = support::recorded_queue_codec(options);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{3};
    std::uint64_t checksum{};
    for (std::uint64_t sequence = 1; sequence <= support::independence_message_count; ++sequence)
    {
      for (;;)
      {
        const auto received = require_value(consumer.try_receive());
        if (!received)
        {
          yield_before(deadline);
          continue;
        }
        const auto& message = [&]() -> const MessageView&
        {
          if constexpr (std::is_same_v<Consumer, recorded::RecordedAcknowledgedConsumer>)
          {
            return received->_message;
          }
          else
          {
            return *received;
          }
        }();
        require(message._header._publication._producer._session == options._log._header._session &&
                    message._header._publication._producer._task == options._log._header._producer &&
                    message._header._publication._sequence == sequence && message._header._queue == options._queue &&
                    message._header._queueSequence == sequence && message._header._schema == options._schema,
                "Independent consumer received the wrong publication or Queue");
        const auto fields = require_value(codec.decode(message._payload));
        require(std::get<std::uint64_t>(fields.at(0)) == support::independence_payload_value(connection, sequence),
                "Independent consumer received corrupt or unrelated bytes");
        if constexpr (std::is_same_v<Consumer, recorded::RecordedAcknowledgedConsumer>)
        {
          // Exercise every processing operation while the independent parent
          // holds administrative or unrelated-stream ownership. Repeated pins
          // stay exact; release is followed by a new issuance of the same ID.
          const auto publication = message._header._publication;
          const auto repeated = require_value(consumer.try_receive());
          require(repeated && repeated->_token == received->_token &&
                      repeated->_message._header == message._header,
                  "Repeated independent pin changed its token or dequeue observation");
          require_value(consumer.release(received->_token));
          const auto redelivered = require_value(consumer.try_receive());
          require(redelivered && redelivered->_message._header._publication == publication &&
                      redelivered->_token._sequence == sequence && redelivered->_token != received->_token,
                  "Independent release did not produce a distinct issuance of the same publication");
          require_value(consumer.acknowledge(redelivered->_token));
        }
        checksum += sequence;
        break;
      }
    }
    require(!require_value(consumer.try_receive()), "Independent consumer received a duplicate");
    support::report('A');
    support::report(support::independence_message_count);
    support::report(checksum);
  }
}

int main(int argc, char** argv)
{
  try
  {
    if (argc != 5)
    {
      throw std::runtime_error("usage: recorded_queue_independence_worker directory mode connection publications");
    }
    ::alarm(15);
    const std::filesystem::path directory{argv[1]};
    std::string mode{argv[2]};
    const bool acknowledged = mode.ends_with("-acknowledged");
    if (acknowledged)
    {
      mode.resize(mode.size() - std::string_view{"-acknowledged"}.size());
    }
    const auto connection = static_cast<unsigned>(std::stoul(argv[3]));
    const auto publications = std::stoull(argv[4]);
    auto controller = require_value(mapped::SessionController::open_existing(directory));
    if (mode == "gate")
    {
      // This is the real controller guard used by Session administration, not
      // a lock on a separate test file. Report only once native ownership exists.
      auto guard = require_value(controller->acquire_mutation());
      require_value(guard.check_owner());
      support::report('G');
      support::wait_for_parent();
      require_value(guard.check_owner());
      return 0;
    }
    const auto options = support::recorded_independence_options(directory, controller->session_id(), connection, publications, acknowledged);
    if (mode == "produce")
    {
      run_producer(options, connection);
      return 0;
    }
    if (mode == "consume")
    {
      if (acknowledged)
      {
        run_consumer<recorded::RecordedAcknowledgedConsumer>(options, connection);
      }
      else
      {
        run_consumer<recorded::RecordedConsumer>(options, connection);
      }
      return 0;
    }
    if (mode == "probe-producer")
    {
      require(support::has_error(recorded::RecordedProducer::open(options), ErrorCode::OwnershipBusy),
              "A busy producer allowed replacement ownership");
      support::report('B');
      return 0;
    }
    if (mode == "recover")
    {
      PausedRecovery observer;
      auto producer = require_value(recorded::RecordedProducer::open(options, &observer));
      support::report('O');
      support::report(require_value(producer.claim())._incarnation);
      return 0;
    }
    if (mode != "paused" && mode != "full" && mode != "logfull")
    {
      throw std::runtime_error("Unknown independent Queue worker mode");
    }
    PausedAcceptance observer;
    auto producer = require_value(recorded::RecordedProducer::open(options, mode == "paused" ? &observer : nullptr));
    support::report('O');
    support::report(require_value(producer.claim())._incarnation);
    const auto codec = support::recorded_queue_codec(options);
    const auto bytes = support::recorded_queue_payload(codec, 42);
    const auto outgoing = support::recorded_queue_message(options, bytes);
    if (mode == "paused")
    {
      require_recorded_send(producer.try_send(require_value(producer.next_publication_id()), outgoing));
      support::report('A');
      return 0;
    }
    recorded::RecordedConsumer consumer;
    recorded::RecordedAcknowledgedConsumer acknowledgedConsumer;
    if (mode == "logfull")
    {
      if (acknowledged)
      {
        acknowledgedConsumer = require_value(recorded::RecordedAcknowledgedConsumer::open(options));
      }
      else
      {
        consumer = require_value(recorded::RecordedConsumer::open(options));
      }
    }
    const auto count = mode == "full" ? options._capacity : publications;
    for (std::uint64_t sequence = 0; sequence != count; ++sequence)
    {
      require_recorded_send(producer.try_send(require_value(producer.next_publication_id()), outgoing));
      if (mode == "logfull")
      {
        if (acknowledged)
        {
          const auto delivery = require_value(acknowledgedConsumer.try_receive());
          require(bool(delivery), "Expected acknowledged delivery before log exhaustion");
          require_value(acknowledgedConsumer.acknowledge(delivery->_token));
        }
        else
        {
          require(bool(require_value(consumer.try_receive())), "Expected credited accepted message before log exhaustion");
        }
      }
    }
    const auto waiting = require_value(producer.next_publication_id());
    const auto rejected = producer.try_send(waiting, outgoing);
    require(mode == "full" ? std::holds_alternative<recorded::QueueCreditFull>(rejected) :
                (std::holds_alternative<Error>(rejected) && std::get<Error>(rejected)._code == ErrorCode::LogFull),
            "Unrelated Queue did not reach its intended resource limit");
    support::report(mode == "full" ? 'F' : 'L');
    support::wait_for_parent();
    require(!require_value(producer.lookup(waiting)), "Rejected unrelated publication became accepted");
  }
  catch (const std::exception& error)
  {
    std::cerr << "recorded Queue independence worker: " << error.what() << '\n';
    return 1;
  }
}
