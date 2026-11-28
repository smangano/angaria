// Independent exec peer. Pipes carry readiness/results only; every message is
// transferred through the actual recorded Queue, mapped ring and producer log.
#include "recorded_queue_fixture.h"
#include "process_support.h"

#include <cstdint>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <variant>

#include <unistd.h>

namespace
{
  namespace recorded = angaria::detail::recorded;
  namespace support = angaria::test_support;

  class PausedProducer final : public recorded::RecordedQueueObserver
  {
  public:
    explicit PausedProducer(recorded::RecordedQueueCheckpoint point) : _point(point)
    {
    }

    angaria::OperationResult checkpoint(recorded::RecordedQueueCheckpoint point) override
    {
      if (!_paused && point == _point)
      {
        _paused = true;
        support::report('R');
        support::wait_for_parent();
      }
      return std::monostate{};
    }

  private:
    recorded::RecordedQueueCheckpoint _point;
    bool _paused{};
  };
}

int main(int argc, char** argv)
{
  try
  {
    if (argc != 3)
    {
      throw std::runtime_error("usage: recorded_queue_worker directory consume|hold-consumer|commit|stamp|published");
    }
    ::alarm(15);
    const auto options = support::recorded_queue_options(std::filesystem::path{argv[1]});
    const auto codec = support::recorded_queue_codec(options);
    const std::string mode{argv[2]};
    if (mode == "consume" || mode == "hold-consumer")
    {
      auto consumer = support::require_value(recorded::RecordedConsumer::open(options));
      support::report('O');
      support::report(support::require_value(consumer.claim())._incarnation);
      support::wait_for_parent();
      if (mode == "consume")
      {
        const auto message = support::require_value(consumer.try_receive());
        if (!message)
        {
          throw std::runtime_error("Expected independent Queue delivery");
        }
        auto copied = support::require_value(message->copy_message(8));
        const auto fields = support::require_value(copied.decode(codec));
        support::report('A');
        support::report(std::get<std::uint64_t>(fields.at(0)));
        support::report(*message->_header._queueSequence);
      }
      return 0;
    }
    if (mode != "commit" && mode != "stamp" && mode != "published")
    {
      throw std::runtime_error("Unknown recorded Queue worker mode");
    }
    const auto point = mode == "commit" ? recorded::RecordedQueueCheckpoint::CommittedBeforeHint :
                       mode == "stamp" ? recorded::RecordedQueueCheckpoint::DescriptorBeforeStamp :
                                         recorded::RecordedQueueCheckpoint::DescriptorPublished;
    PausedProducer observer{point};
    auto producer = support::require_value(recorded::RecordedProducer::open(options, &observer));
    const auto claim = support::require_value(producer.claim());
    support::report('O');
    support::report(claim._incarnation);
    const auto id = support::require_value(producer.next_publication_id());
    const auto bytes = support::recorded_queue_payload(codec, 42);
    const auto accepted = support::require_recorded_send(producer.try_send(
        id, support::recorded_queue_message(options, bytes)));
    support::report('A');
    support::report(accepted._queueSequence);
    support::wait_for_parent();
    const auto nextBytes = support::recorded_queue_payload(codec, 84);
    const auto next = support::require_recorded_send(producer.try_send(
        support::require_value(producer.next_publication_id()), support::recorded_queue_message(options, nextBytes)));
    support::report('A');
    support::report(next._queueSequence);
  }
  catch (const std::exception& error)
  {
    std::cerr << "recorded Queue worker: " << error.what() << '\n';
    return 1;
  }
}
