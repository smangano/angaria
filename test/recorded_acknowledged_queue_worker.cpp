// Pipes only coordinate barriers and report results. The actual operations
// below use the same mapped endpoints and local progress protocol as clients.
#include "recorded_acknowledged_fixture.h"
#include "recorded_queue_independence_fixture.h"
#include "process_support.h"
#include "session_controller.h"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <variant>

#include <unistd.h>

namespace
{
  using namespace angaria;
  namespace recorded = angaria::detail::recorded;
  namespace mapped = angaria::detail::mapped;
  namespace support = angaria::test_support;
  using support::require_value;

  recorded::ProgressCheckpoint progress_point(const std::string& name)
  {
    if (name == "partial-image")
    {
      return recorded::ProgressCheckpoint::PartialImage;
    }
    if (name == "image")
    {
      return recorded::ProgressCheckpoint::ImageWritten;
    }
    if (name == "before-selector")
    {
      return recorded::ProgressCheckpoint::BeforeSelector;
    }
    if (name == "selector")
    {
      return recorded::ProgressCheckpoint::SelectorPublished;
    }
    if (name == "partial-receipt")
    {
      return recorded::ProgressCheckpoint::PartialReceipt;
    }
    if (name == "receipt")
    {
      return recorded::ProgressCheckpoint::ReceiptInstalled;
    }
    if (name == "partial-source")
    {
      return recorded::ProgressCheckpoint::PartialSource;
    }
    if (name == "source")
    {
      return recorded::ProgressCheckpoint::SourceInstalled;
    }
    if (name == "before-applied")
    {
      return recorded::ProgressCheckpoint::BeforeApplied;
    }
    if (name == "applied")
    {
      return recorded::ProgressCheckpoint::AppliedPublished;
    }
    throw std::runtime_error("Unknown progress stop boundary");
  }

  class PausedProgress final : public recorded::ProgressObserver
  {
  public:
    explicit PausedProgress(recorded::ProgressCheckpoint point) : _point(point)
    {
    }

    OperationResult checkpoint(recorded::ProgressCheckpoint point) override
    {
      if (_armed && !_paused && point == _point)
      {
        _paused = true;
        support::report('R');
        support::wait_for_parent();
      }
      return std::monostate{};
    }

    bool _armed{};

  private:
    recorded::ProgressCheckpoint _point;
    bool _paused{};
  };

  class PausedHint final : public recorded::RecordedQueueObserver
  {
  public:
    explicit PausedHint(recorded::RecordedQueueCheckpoint point) : _point(point)
    {
    }

    OperationResult checkpoint(recorded::RecordedQueueCheckpoint point) override
    {
      if (_armed && !_paused && point == _point)
      {
        _paused = true;
        support::report('R');
        support::wait_for_parent();
      }
      return std::monostate{};
    }

    bool _armed{};

  private:
    recorded::RecordedQueueCheckpoint _point;
    bool _paused{};
  };

  void require(bool condition, const char* message)
  {
    if (!condition)
    {
      throw std::runtime_error(message);
    }
  }

  void yield_before(std::chrono::steady_clock::time_point deadline)
  {
    require(std::chrono::steady_clock::now() < deadline, "Acknowledged connection made no progress");
    std::this_thread::yield();
  }

  void session_peer(const std::filesystem::path& directory, const std::string& mode, unsigned connection)
  {
    auto controller = require_value(mapped::SessionController::open_existing(directory));
    if (mode == "session-gate")
    {
      auto guard = require_value(controller->acquire_mutation());
      require_value(guard.check_owner());
      support::report('G');
      support::wait_for_parent();
      require_value(guard.check_owner());
      return;
    }
    auto options = support::recorded_independence_options(directory, controller->session_id(), connection);
    options._acknowledged = true;
    const auto codec = support::recorded_queue_codec(options);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{3};
    if (mode == "session-pause-ack")
    {
      PausedProgress progress{recorded::ProgressCheckpoint::SelectorPublished};
      auto receiver = require_value(recorded::RecordedAcknowledgedConsumer::open(options, nullptr, &progress));
      const auto delivery = require_value(receiver.try_receive(support::recorded_test_time(-23)));
      require(bool(delivery), "Expected peer message before paused acknowledgment");
      support::report('O');
      support::wait_for_parent();
      progress._armed = true;
      require_value(receiver.acknowledge(delivery->_token, support::recorded_test_time(-17)));
      throw std::runtime_error("Acknowledgment failed to pause after its selector");
    }
    if (mode == "session-produce")
    {
      auto sender = require_value(recorded::RecordedProducer::open(options));
      support::report('O');
      support::wait_for_parent();
      for (std::uint64_t sequence = 1; sequence <= support::independence_message_count; ++sequence)
      {
        const auto id = require_value(sender.next_publication_id());
        const auto bytes = support::recorded_queue_payload(codec, sequence);
        for (;;)
        {
          const auto sent = sender.try_send(id, support::recorded_queue_message(options, bytes));
          if (std::holds_alternative<recorded::QueueCreditFull>(sent))
          {
            yield_before(deadline);
            continue;
          }
          const auto accepted = support::require_recorded_send(sent);
          require(accepted._queueSequence == sequence && !accepted._duplicate, "Wrong acknowledged Queue sequence");
          const auto lookup = require_value(sender.lookup(id));
          require(lookup && lookup->_reference == accepted._reference, "Missing accepted-ID outcome");
          break;
        }
      }
      while (require_value(sender.acknowledged_sequence()) != support::independence_message_count)
      {
        yield_before(deadline);
      }
    }
    else
    {
      auto receiver = require_value(recorded::RecordedAcknowledgedConsumer::open(options));
      support::report('O');
      support::wait_for_parent();
      for (std::uint64_t sequence = 1; sequence <= support::independence_message_count; ++sequence)
      {
        for (;;)
        {
          const auto delivery = require_value(receiver.try_receive());
          if (!delivery)
          {
            yield_before(deadline);
            continue;
          }
          require(delivery->_token._queue == options._queue && delivery->_token._sequence == sequence &&
                      delivery->_message._header._publication._producer._session == options._log._header._session &&
                      delivery->_message._header._publication._producer._task == options._log._header._producer &&
                      delivery->_message._header._queueSequence == sequence,
                  "Wrong acknowledged delivery identity");
          const auto fields = require_value(codec.decode(delivery->_message._payload));
          require(std::get<std::uint64_t>(fields.at(0)) == sequence, "Wrong acknowledged payload");
          const auto repeated = require_value(receiver.try_receive());
          require(repeated && repeated->_token == delivery->_token &&
                      repeated->_message._header == delivery->_message._header,
                  "Pinned receive changed its issuance");
          require_value(receiver.acknowledge(delivery->_token));
          break;
        }
      }
      require(!require_value(receiver.try_receive()), "Acknowledged delivery repeated after terminal progress");
    }
    support::report('A');
    support::report(support::independence_message_count);
  }
}

int main(int argc, char** argv)
{
  try
  {
    if (argc != 4)
    {
      throw std::runtime_error("usage: recorded_acknowledged_queue_worker directory operation checkpoint");
    }
    ::alarm(15);
    const std::filesystem::path directory{argv[1]};
    const std::string mode{argv[2]};
    if (mode.starts_with("session-"))
    {
      session_peer(directory, mode, static_cast<unsigned>(std::stoul(argv[3])));
      return 0;
    }
    const auto options = support::recorded_acknowledged_options(directory);
    const bool hint = mode == "credit" || mode == "ack-hint";
    PausedProgress progress{hint ? recorded::ProgressCheckpoint::PartialImage : progress_point(argv[3])};
    PausedHint queue{mode == "ack-hint" ? recorded::RecordedQueueCheckpoint::AcknowledgmentBeforeHint :
                                        recorded::RecordedQueueCheckpoint::DeliveryBeforeCredit};
    progress._armed = mode == "recover" || mode == "attach";
    auto receiver = require_value(recorded::RecordedAcknowledgedConsumer::open(options, &queue, &progress));
    if (mode == "recover" || mode == "attach")
    {
      throw std::runtime_error("Recovery failed to visit the requested checkpoint");
    }
    support::report('O');
    support::report(require_value(receiver.claim())._incarnation);
    std::optional<Delivery> existing;
    if (mode == "ack" || mode == "release" || mode == "close" || mode == "ack-hint")
    {
      existing = require_value(receiver.try_receive(support::recorded_test_time(-23)));
      require(bool(existing), "Expected message before interrupted progress");
      support::report('D');
      support::report(&existing->_token, sizeof(existing->_token));
    }
    support::report('P');
    support::report(require_value(receiver.progress())._generation);
    support::wait_for_parent();
    progress._armed = !hint;
    queue._armed = hint;
    if (mode == "issue" || mode == "credit")
    {
      require(bool(require_value(receiver.try_receive(support::recorded_test_time(-23)))), "Expected interrupted issuance");
    }
    else if (mode == "ack" || mode == "ack-hint")
    {
      require_value(receiver.acknowledge(existing->_token, support::recorded_test_time(-17)));
    }
    else if (mode == "release")
    {
      require_value(receiver.release(existing->_token));
    }
    else if (mode == "close")
    {
      require_value(receiver.close());
    }
    else
    {
      throw std::runtime_error("Unknown acknowledged worker operation");
    }
    throw std::runtime_error("Operation failed to visit the requested checkpoint");
  }
  catch (const std::exception& error)
  {
    std::cerr << "acknowledged Queue worker: " << error.what() << '\n';
    return 1;
  }
}
