// Private acknowledged endpoint component costs. The separate IPC benchmark
// measures independently launched peers against nanomsg. Here assertions guard
// allocations and native/Session work; clock samples have no CI thresholds.
#include "allocation_capture.h"
#include "performance_diagnostics.h"
#include "performance_samples.h"
#include "recorded_queue_fixture.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <optional>
#include <variant>

namespace
{
  using namespace angaria;
  namespace recorded = angaria::detail::recorded;
  namespace diagnostics = angaria::detail::diagnostics;
  namespace support = angaria::test_support;
  using support::AllocationCapture;
  using support::require_value;

  constexpr std::size_t samples = 5;
  constexpr std::size_t cycles = 64;
  volatile std::uint64_t observed_checksum{};

  struct CycleResult
  {
    bool _valid{true};
    std::size_t _count{};
    std::uint64_t _checksum{};
  };

  bool same_message(const MessageView& message, const MessageView& outgoing)
  {
    return message._header._schema == outgoing._header._schema &&
           message._header._format == outgoing._header._format &&
           message._header._flags == outgoing._header._flags &&
           message._header._payloadBytes == outgoing._payload.size() &&
           std::equal(message._payload.begin(), message._payload.end(), outgoing._payload.begin(), outgoing._payload.end());
  }

  // Payload preparation, test assertions and output formatting are excluded.
  // Every measured message is accepted once, looked up, byte-verified and
  // explicitly acknowledged using the exact token returned with its view.
  CycleResult acknowledged_cycles(recorded::RecordedProducer& sender,
      recorded::RecordedAcknowledgedConsumer& receiver, const MessageView& outgoing,
      TaskClaim consumer, std::uint64_t firstSequence, std::size_t count)
  {
    CycleResult result;
    for (std::size_t index = 0; index != count; ++index)
    {
      const auto issued = sender.next_publication_id();
      const auto* publication = std::get_if<PublicationId>(&issued);
      if (!publication)
      {
        result._valid = false;
        break;
      }
      const auto sent = sender.try_send(*publication, outgoing);
      const auto* accepted = std::get_if<recorded::ProducerAppendResult>(&sent);
      if (!accepted || accepted->_duplicate || accepted->_queueSequence != firstSequence + index)
      {
        result._valid = false;
        break;
      }
      const auto lookup = sender.lookup(*publication);
      const auto* known = std::get_if<std::optional<recorded::ProducerAppendResult>>(&lookup);
      if (!known || !*known || (*known)->_reference != accepted->_reference ||
          (*known)->_queueSequence != accepted->_queueSequence || (*known)->_recordOrdinal != accepted->_recordOrdinal)
      {
        result._valid = false;
        break;
      }
      const auto received = receiver.try_receive();
      const auto* found = std::get_if<std::optional<Delivery>>(&received);
      if (!found || !*found)
      {
        result._valid = false;
        break;
      }
      const auto& delivery = **found;
      const auto& header = delivery._message._header;
      const auto& token = delivery._token;
      if (!same_message(delivery._message, outgoing) || header._publication != *publication ||
          header._queue != QueueId{4} || header._queueSequence != accepted->_queueSequence ||
          header._consumer != consumer || !header._enqueuedAt || !header._dequeuedAt ||
          token._consumer != consumer || token._queue != QueueId{4} || token._sourceSet != SourceQueueSetId{2} ||
          token._sequence != accepted->_queueSequence || token._issuance == 0 ||
          std::holds_alternative<Error>(receiver.acknowledge(token)))
      {
        result._valid = false;
        break;
      }
      result._checksum += publication->_sequence + token._sequence + token._issuance;
      ++result._count;
    }
    return result;
  }

  void expect_no_native_work(const diagnostics::Sample& sample)
  {
    EXPECT_TRUE(sample._valid);
    EXPECT_FALSE(sample._overflow);
    for (const auto counter : {diagnostics::Counter::NativeReadCalls, diagnostics::Counter::NativeMapCalls,
                               diagnostics::Counter::NativeUnmapCalls, diagnostics::Counter::FileStatCalls,
                               diagnostics::Counter::FileGrowCalls, diagnostics::Counter::GateAttempts,
                               diagnostics::Counter::GateAcquisitions})
    {
      EXPECT_EQ(sample._counters[static_cast<std::size_t>(counter)], 0u)
          << diagnostics::counter_names[static_cast<std::size_t>(counter)];
    }
  }

  TEST(RecordedAcknowledgedAllocationCapture, CountsSuccessfulAllocationRequests)
  {
    std::uint64_t allocations{};
    {
      AllocationCapture capture;
      auto* control = ::operator new(16);
      ::operator delete(control);
      allocations = capture.stop();
    }
    EXPECT_EQ(allocations, 1u);
  }

  TEST(RecordedAcknowledgedPerformance, MessagingPinRepeatsAndRedeliveryKeepBoundedLocalCosts)
  {
    support::ProducerLogDirectory directory;
    auto options = support::recorded_queue_options(directory.path(), 2, 1024);
    options._acknowledged = true;
    const auto codec = support::recorded_queue_codec(options);
    const auto bytes = support::recorded_queue_payload(codec, 42);
    const auto outgoing = support::recorded_queue_message(options, bytes);
    require_value(recorded::create_recorded_queue(options));
    auto sender = require_value(recorded::RecordedProducer::open(options));
    recorded::RecordedAcknowledgedConsumer receiver;
    diagnostics::Sample openSample;
    {
      diagnostics::Capture capture(openSample, diagnostics::Collection::Counts);
      receiver = require_value(recorded::RecordedAcknowledgedConsumer::open(options));
    }
    if constexpr (diagnostics::instrumentation_enabled)
    {
      // The real cold open is a positive control; a missing counter backend
      // must not make every hot-path zero below look like evidence.
      EXPECT_GT(openSample._counters[static_cast<std::size_t>(diagnostics::Counter::NativeReadCalls)], 0u);
      EXPECT_GT(openSample._counters[static_cast<std::size_t>(diagnostics::Counter::NativeMapCalls)], 0u);
      EXPECT_GT(openSample._counters[static_cast<std::size_t>(diagnostics::Counter::FileStatCalls)], 0u);
    }
    const auto consumer = require_value(receiver.claim());
    std::array<double, samples> roundtripNs{}, pinRepeatNs{}, releaseRedeliveryNs{};
    std::array<std::uint64_t, samples> allocations{}, pinAllocations{}, redeliveryAllocations{};
    for (std::size_t sample = 0; sample != samples; ++sample)
    {
      CycleResult result;
      {
        AllocationCapture capture;
        const auto start = std::chrono::steady_clock::now();
        result = acknowledged_cycles(sender, receiver, outgoing, consumer, sample * cycles + 1, cycles);
        roundtripNs[sample] = std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - start).count() / cycles;
        allocations[sample] = capture.stop();
      }
      observed_checksum = result._checksum;
      ASSERT_TRUE(result._valid);
      ASSERT_EQ(result._count, cycles);
      EXPECT_EQ(allocations[sample], 0u);
    }
    diagnostics::Sample messagingSample;
    CycleResult diagnostic;
    {
      diagnostics::Capture capture(messagingSample, diagnostics::Collection::Counts);
      diagnostic = acknowledged_cycles(sender, receiver, outgoing, consumer, samples * cycles + 1, cycles);
    }
    ASSERT_TRUE(diagnostic._valid);
    ASSERT_EQ(diagnostic._count, cycles);
    expect_no_native_work(messagingSample);
    EXPECT_EQ(require_value(sender.acknowledged_sequence()), (samples + 1) * cycles);

    const auto publication = require_value(sender.next_publication_id());
    support::require_recorded_send(sender.try_send(publication, outgoing));
    auto pending = require_value(receiver.try_receive(UtcTimestamp{}));
    ASSERT_TRUE(pending);
    Delivery delivery = *pending;
    const auto originalGeneration = require_value(receiver.progress())._generation;
    const auto repeat = [&]
    {
      bool valid = true;
      for (std::size_t index = 0; index != cycles; ++index)
      {
        const auto received = receiver.try_receive();
        const auto* found = std::get_if<std::optional<Delivery>>(&received);
        valid = valid && found && *found && (**found)._token == delivery._token &&
            (**found)._message._header == delivery._message._header && same_message((**found)._message, outgoing);
      }
      return valid;
    };
    for (std::size_t sample = 0; sample != samples; ++sample)
    {
      bool valid{};
      {
        AllocationCapture capture;
        const auto start = std::chrono::steady_clock::now();
        valid = repeat();
        pinRepeatNs[sample] = std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - start).count() / cycles;
        pinAllocations[sample] = capture.stop();
      }
      EXPECT_TRUE(valid);
      EXPECT_EQ(pinAllocations[sample], 0u);
    }
    EXPECT_EQ(require_value(receiver.progress())._generation, originalGeneration);
    diagnostics::Sample repeatSample;
    {
      diagnostics::Capture capture(repeatSample, diagnostics::Collection::Counts);
      EXPECT_TRUE(repeat());
    }
    expect_no_native_work(repeatSample);

    const auto redeliver = [&]
    {
      bool valid = true;
      for (std::size_t index = 0; index != cycles; ++index)
      {
        const auto previous = delivery._token;
        if (std::holds_alternative<Error>(receiver.release(previous)))
        {
          return false;
        }
        const auto received = receiver.try_receive(UtcTimestamp{});
        const auto* found = std::get_if<std::optional<Delivery>>(&received);
        if (!found || !*found)
        {
          return false;
        }
        delivery = **found;
        valid = valid && delivery._message._header._publication == publication &&
            delivery._message._header._queue == previous._queue &&
            delivery._message._header._queueSequence == previous._sequence &&
            delivery._message._header._consumer == previous._consumer &&
            delivery._token._consumer == previous._consumer && delivery._token._sequence == previous._sequence &&
            delivery._token._queue == previous._queue && delivery._token._sourceSet == previous._sourceSet &&
            delivery._token._issuance == previous._issuance + 1 && same_message(delivery._message, outgoing);
      }
      return valid;
    };
    for (std::size_t sample = 0; sample != samples; ++sample)
    {
      bool valid{};
      {
        AllocationCapture capture;
        const auto start = std::chrono::steady_clock::now();
        valid = redeliver();
        releaseRedeliveryNs[sample] = std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - start).count() / cycles;
        redeliveryAllocations[sample] = capture.stop();
      }
      EXPECT_TRUE(valid);
      EXPECT_EQ(redeliveryAllocations[sample], 0u);
    }
    diagnostics::Sample redeliverySample;
    {
      diagnostics::Capture capture(redeliverySample, diagnostics::Collection::Counts);
      EXPECT_TRUE(redeliver());
    }
    expect_no_native_work(redeliverySample);
    require_value(receiver.acknowledge(delivery._token));
    EXPECT_FALSE(require_value(receiver.try_receive()));
    EXPECT_EQ(require_value(sender.acknowledged_sequence()), (samples + 1) * cycles + 1);

    std::cout << std::fixed << std::setprecision(3)
              << "{\"study\":\"recorded_acknowledged_queue_same_process_cost\",\"payload_bytes\":8,\"ring_capacity\":2,"
              << "\"cycles_per_block\":" << cycles << ",\"id_send_lookup_receive_ack_ns\":";
    support::write_samples(roundtripNs);
    std::cout << ",\"verified_acknowledged_publications_per_second\":" << 1e9 / support::median(roundtripNs)
              << ",\"pinned_repeat_ns_per_call\":";
    support::write_samples(pinRepeatNs);
    std::cout << ",\"release_redelivery_ns_per_cycle\":";
    support::write_samples(releaseRedeliveryNs);
    std::cout << ",\"messaging_allocations\":";
    support::write_samples(allocations);
    std::cout << ",\"pinned_repeat_allocations\":";
    support::write_samples(pinAllocations);
    std::cout << ",\"release_redelivery_allocations\":";
    support::write_samples(redeliveryAllocations);
    std::cout << ",\"diagnostics_enabled\":" << (diagnostics::instrumentation_enabled ? "true" : "false") << "}\n";
  }
}
