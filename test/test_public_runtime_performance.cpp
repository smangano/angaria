// Current public API cost and deterministic hot-work guards. These same-process
// cycles explain wrapper cost; independent-process public/nanomsg qualification
// supplies application delivery latency and completed-throughput results.
#include "allocation_capture.h"
#include "performance_diagnostics.h"
#include "public_runtime_fixture.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <optional>
#include <variant>

namespace
{
  using namespace angaria;
  namespace support = angaria::test_support;
  namespace diagnostics = angaria::detail::diagnostics;
  using support::require_value;
  constexpr std::size_t cycles = 256;
  constexpr std::size_t repetitions = 5;

  struct CycleReport
  {
    bool _valid{true};
    std::uint64_t _completed{};
    std::uint64_t _checksum{};
  };

  CycleReport roundtrips(Task& producer, QueueWriter& writer, SourceQueueSet& source,
                          MessageView outgoing, bool acknowledged, std::size_t count)
  {
    CycleReport result;
    std::array<DestinationOutcome, 1> destinations;
    const auto queue = writer.id();
    for (std::size_t index = 0; index < count; ++index)
    {
      const auto issuance = producer.next_publication_id();
      const auto* publication = std::get_if<PublicationId>(&issuance);
      if (!publication)
      {
        result._valid = false;
        break;
      }
      const auto sent = writer.send(*publication, outgoing, destinations);
      const auto known = producer.publication_status(*publication, destinations);
      if (sent._status != PublicationStatus::Accepted || sent._error != ErrorCode::None ||
          known._status != PublicationStatus::Accepted || known._error != ErrorCode::None ||
          known._destinations.size() != 1 || known._destinations.front()._queue != queue)
      {
        result._valid = false;
        break;
      }
      const auto received = source.receive();
      const auto* delivery = std::get_if<Delivery>(&received);
      const auto* message = std::get_if<MessageView>(&received);
      if (acknowledged ? delivery == nullptr : message == nullptr)
      {
        result._valid = false;
        break;
      }
      const auto& view = acknowledged ? delivery->_message : *message;
      if (view._header._publication != *publication || view._header._schema != outgoing._header._schema ||
          view._header._flags != outgoing._header._flags || view._payload.size() != outgoing._payload.size() ||
          !std::equal(view._payload.begin(), view._payload.end(), outgoing._payload.begin()))
      {
        result._valid = false;
        break;
      }
      if (acknowledged && std::holds_alternative<Error>(source.acknowledge(delivery->_token)))
      {
        result._valid = false;
        break;
      }
      ++result._completed;
      result._checksum += publication->_sequence;
    }
    return result;
  }

  TEST(PublicRuntimePerformance, BufferedMessagingKeepsGateFileAndAllocationWorkOutOfBothHotPaths)
  {
    for (const bool acknowledged : {false, true})
    {
      for (const std::size_t history : {0, 128})
      {
        support::PublicRuntimeConfiguration configuration;
        configuration._mode = acknowledged ? AcknowledgmentMode::Required : AcknowledgmentMode::None;
        configuration._publications = 4096;
        support::PublicRuntimeFixture fixture(configuration);
        auto& producer = fixture.created()._creator;
        auto writer = require_value(producer.open_writer(fixture.queues().front()));
        auto consumer = require_value(fixture.created()._session.attach_task(fixture.consumer_ids().front()));
        auto source = require_value(consumer.open_source_set(fixture.sources().front()));
        const auto payload = fixture.payload(123);
        const auto outgoing = fixture.message(payload);
        const auto warm = roundtrips(producer, writer, source, outgoing, acknowledged, history + 32);
        ASSERT_TRUE(warm._valid);
        ASSERT_EQ(warm._completed, history + 32);
        std::array<std::uint64_t, repetitions> elapsed{};
        for (std::size_t repetition = 0; repetition < repetitions; ++repetition)
        {
          diagnostics::Sample work;
          CycleReport result;
          std::uint64_t allocations{};
          const auto start = std::chrono::steady_clock::now();
          {
            support::AllocationCapture allocationCapture;
            diagnostics::Capture capture(work, diagnostics::Collection::Counts);
            result = roundtrips(producer, writer, source, outgoing, acknowledged, cycles);
            allocations = allocationCapture.stop();
          }
          elapsed[repetition] = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
              std::chrono::steady_clock::now() - start).count());
          ASSERT_TRUE(result._valid);
          ASSERT_EQ(result._completed, cycles);
          ASSERT_GT(result._checksum, 0u);
          EXPECT_EQ(allocations, 0u);
          for (const auto counter : {diagnostics::Counter::GateAttempts, diagnostics::Counter::GateAcquisitions,
              diagnostics::Counter::PrefixScans, diagnostics::Counter::TopologyBuilds,
              diagnostics::Counter::NativeReadCalls, diagnostics::Counter::NativeMapCalls,
              diagnostics::Counter::NativeUnmapCalls, diagnostics::Counter::FileStatCalls,
              diagnostics::Counter::FileGrowCalls})
          {
            EXPECT_EQ(work._counters[static_cast<std::size_t>(counter)], 0u);
          }
        }
        std::sort(elapsed.begin(), elapsed.end());
        const auto median = elapsed[repetitions / 2];
        std::cout << "PUBLIC_RUNTIME_PERFORMANCE {\"mode\":\"" << (acknowledged ? "required" : "none")
                  << "\",\"prior_history\":" << history << ",\"warmup_cycles\":" << history + 32
                  << ",\"cycles_per_run\":" << cycles << ",\"runs\":" << repetitions
                  << ",\"median_cycle_ns\":" << static_cast<double>(median) / cycles
                  << ",\"min_cycle_ns\":" << static_cast<double>(elapsed.front()) / cycles
                  << ",\"max_cycle_ns\":" << static_cast<double>(elapsed.back()) / cycles
                  << ",\"completed_cycles_per_second\":" << cycles * 1e9 / static_cast<double>(median)
                  << ",\"scope\":\"same-process ID/send/status/receive/selectedACK; not IPC latency\"}\n";
      }
    }
  }

  TEST(PublicRuntimePerformance, RepeatedEmptyPublicReceiveDoesNoHeapFileOrAdministrativeWork)
  {
    for (const auto mode : {AcknowledgmentMode::Required, AcknowledgmentMode::None})
    {
      support::PublicRuntimeFixture fixture({._mode = mode});
      auto consumer = require_value(fixture.created()._session.attach_task(fixture.consumer_ids().front()));
      auto source = require_value(consumer.open_source_set(fixture.sources().front()));
      ASSERT_TRUE(std::holds_alternative<ReceiveTimeout>(source.receive()));
      diagnostics::Sample work;
      std::size_t completed{};
      std::uint64_t allocations{};
      {
        support::AllocationCapture allocationCapture;
        diagnostics::Capture capture(work, diagnostics::Collection::Counts);
        for (std::size_t index = 0; index < 10000; ++index)
        {
          completed += std::holds_alternative<ReceiveTimeout>(source.receive()) ? 1 : 0;
        }
        allocations = allocationCapture.stop();
      }
      EXPECT_EQ(completed, 10000u);
      EXPECT_EQ(allocations, 0u);
      for (const auto counter : {diagnostics::Counter::GateAttempts, diagnostics::Counter::PrefixScans,
          diagnostics::Counter::NativeReadCalls, diagnostics::Counter::NativeMapCalls,
          diagnostics::Counter::FileStatCalls, diagnostics::Counter::FileGrowCalls})
      {
        EXPECT_EQ(work._counters[static_cast<std::size_t>(counter)], 0u);
      }
    }
  }
}
