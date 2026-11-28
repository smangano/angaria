// Current-format codec cost study, not live append or transport performance.
// Caller-buffer encoding and borrowed decoding retain zero-allocation guards;
// explicit allocation calibration proves the counter is active. Historical
// format comparisons remain reproducible from their pinned source revisions.

#include "allocation_capture.h"
#include "performance_samples.h"
#include "producer_log_format.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <new>
#include <span>
#include <string>
#include <variant>
#include <vector>

namespace
{
  using namespace angaria;
  using angaria::test_support::AllocationCapture;
  using angaria::test_support::median;
  using angaria::test_support::write_samples;
  namespace recorded = angaria::detail::recorded;

  constexpr std::uint64_t owned_offset = 4096;
  constexpr std::uint64_t record_limit = 1048576;
  constexpr std::size_t allocation_cycles = 128;
  constexpr std::size_t sample_cycles = 2048;
  constexpr std::size_t sample_count = 9;

  // Each cycle contributes an observed payload byte and decoded identity. The
  // volatile publication occurs outside timing/capture and prevents dead work.
  volatile std::uint64_t observed_checksum{};

  struct CycleResult
  {
    bool _valid{true};
    std::uint64_t _checksum{};
  };

  CycleResult owned_cycles(const recorded::PublicationRecordInput& publication,
                           const recorded::ProducerLogHeader& header,
                           std::span<std::byte> buffer, std::size_t count)
  {
    CycleResult result;
    for (std::size_t index = 0; index != count; ++index)
    {
      const auto encoded = recorded::encode_publication_record(publication, header, owned_offset, buffer);
      const auto* reference = std::get_if<recorded::LogReference>(&encoded);
      if (!reference)
      {
        result._valid = false;
        break;
      }
      const auto decoded = recorded::decode_publication_record(buffer, header, *reference);
      const auto* message = std::get_if<recorded::PublicationRecordView>(&decoded);
      if (!message || message->_payload.size() != publication._payload.size() || message->destination_count() != 1)
      {
        result._valid = false;
        break;
      }
      const auto destination = message->destination_at(0);
      const auto* row = std::get_if<recorded::DestinationRecord>(&destination);
      if (!row)
      {
        result._valid = false;
        break;
      }
      result._checksum += std::to_integer<unsigned char>(message->_payload[index % message->_payload.size()]) +
                          message->_header._publication._sequence + row->_queueSequence;
    }
    return result;
  }

  template <typename Operation>
  double measure_cycle_ns(Operation&& operation, std::uint64_t expectedChecksum, bool& valid)
  {
    const auto start = std::chrono::steady_clock::now();
    const auto result = operation();
    const auto stop = std::chrono::steady_clock::now();
    observed_checksum = result._checksum;
    valid = valid && result._valid && result._checksum == expectedChecksum;
    return std::chrono::duration<double, std::nano>(stop - start).count() / sample_cycles;
  }

  class ProducerLogPerformance : public ::testing::TestWithParam<std::size_t>
  {
  };

  TEST_P(ProducerLogPerformance, BorrowedRoundTripHasNoAllocationsAndReportsCodecCost)
  {
    // All fixture and result buffers are established before warmup, capture or
    // timing. No GoogleTest assertion or formatting occurs inside those regions.
    const auto payloadBytes = GetParam();
    std::vector<std::byte> payload(payloadBytes);
    for (std::size_t index = 0; index != payload.size(); ++index)
    {
      payload[index] = static_cast<std::byte>((index * 131 + 17) & 0xff);
    }
    recorded::ProducerLogHeader header;
    header._session = SessionId{1, 2};
    header._producer = TaskId{3};
    header._catalogRevision = 1;
    header._fileBytes = record_limit;
    header._publicationCapacity = 1024;
    // This is an offline byte-codec fixture, not native profile admission.
    header._liveAbiProfile = 0x20002;
    const auto publishedAt = UtcTimestamp{std::chrono::nanoseconds{-7}};
    const std::array destinations{recorded::DestinationRecord{
        QueueId{4}, DestinationStatus::Accepted, ErrorCode::None, 1, publishedAt}};
    recorded::PublicationRecordInput publication;
    publication._header._schema = SchemaId{MessageType{1}, 1};
    publication._header._format = PayloadFormat::Binary;
    publication._header._publication = PublicationId{TaskClaim{header._session, header._producer, 1}, 1};
    publication._header._publishedAt = publishedAt;
    publication._header._payloadBytes = payload.size();
    publication._header._flags = 0x35;
    publication._recordOrdinal = 1;
    publication._schemaReference = 4096;
    publication._topologyRevision = 1;
    publication._destinations = destinations;
    publication._payload = payload;
    const auto planned = recorded::plan_publication_record(publication, header, owned_offset);
    ASSERT_TRUE(std::holds_alternative<recorded::PublicationRecordLayout>(planned));
    const auto layout = std::get<recorded::PublicationRecordLayout>(planned);
    std::vector<std::byte> buffer(static_cast<std::size_t>(layout._bytes));

    // The decoded payload must borrow the caller's record buffer, while its
    // complete observed header and exact destination remain unchanged.
    const auto encoded = recorded::encode_publication_record(publication, header, owned_offset, buffer);
    ASSERT_TRUE(std::holds_alternative<recorded::LogReference>(encoded));
    const auto decoded = recorded::decode_publication_record(buffer, header, std::get<recorded::LogReference>(encoded));
    ASSERT_TRUE(std::holds_alternative<recorded::PublicationRecordView>(decoded));
    const auto& view = std::get<recorded::PublicationRecordView>(decoded);
    EXPECT_EQ(view._header, publication._header);
    ASSERT_EQ(view._payload.size(), payload.size());
    EXPECT_TRUE(std::equal(view._payload.begin(), view._payload.end(), payload.begin()));
    EXPECT_EQ(view._payload.data(), buffer.data() + layout._payloadOffset);
    const auto row = view.destination_at(0);
    ASSERT_TRUE(std::holds_alternative<recorded::DestinationRecord>(row));
    EXPECT_EQ(std::get<recorded::DestinationRecord>(row), destinations.front());
    const auto warmup = owned_cycles(publication, header, buffer, allocation_cycles);
    ASSERT_TRUE(warmup._valid);

    CycleResult ownedCaptured;
    std::uint64_t ownedAllocations{};
    {
      AllocationCapture capture;
      ownedCaptured = owned_cycles(publication, header, buffer, allocation_cycles);
      ownedAllocations = capture.stop();
    }
    ASSERT_TRUE(ownedCaptured._valid);
    EXPECT_EQ(ownedCaptured._checksum, warmup._checksum);
    EXPECT_EQ(ownedAllocations, 0u);
    // This explicit control uses the allocator itself, not a superseded codec.
    // Allocating outside timed work keeps codec measurements current-format only.
    void* allocationControl{};
    std::uint64_t controlAllocations{};
    {
      AllocationCapture capture;
      allocationControl = ::operator new(payloadBytes);
      controlAllocations = capture.stop();
    }
    ::operator delete(allocationControl);
    EXPECT_GT(controlAllocations, 0u);

    bool valid = true;
    std::uint64_t expectedChecksum{};
    for (std::size_t index = 0; index != sample_cycles; ++index)
    {
      expectedChecksum += std::to_integer<unsigned char>(payload[index % payload.size()]) + 2;
    }
    std::array<double, sample_count> ownedTimes{};
    for (std::size_t sample = 0; sample != sample_count; ++sample)
    {
      ownedTimes[sample] = measure_cycle_ns(
          [&]
          {
            return owned_cycles(publication, header, buffer, sample_cycles);
          }, expectedChecksum, valid);
    }
    ASSERT_TRUE(valid);
    const auto ownedNs = median(ownedTimes);
    ASSERT_GT(ownedNs, 0.0);
    const auto ownedRate = 1e9 / ownedNs;
    RecordProperty("payload_bytes", static_cast<int>(payloadBytes));
    RecordProperty("owned_borrowed_median_cycle_ns", std::to_string(ownedNs));
    RecordProperty("owned_borrowed_cycles_per_second", std::to_string(ownedRate));
    RecordProperty("owned_borrowed_allocations_per_cycle", std::to_string(double(ownedAllocations) / allocation_cycles));
    std::cout << std::fixed << std::setprecision(3)
              << "CODEC_PERFORMANCE {\"payload_bytes\":" << payloadBytes
              << ",\"format_major\":3,\"allocation_control_count\":" << controlAllocations
              << ",\"samples\":" << sample_count << ",\"cycles_per_sample\":" << sample_cycles
              << ",\"owned_record_bytes\":" << layout._bytes
              << ",\"owned_borrowed\":{\"median_cycle_ns\":" << ownedNs
              << ",\"cycles_per_second\":" << ownedRate
              << ",\"allocations_per_cycle\":" << double(ownedAllocations) / allocation_cycles
              << ",\"cycle_ns_samples\":";
    write_samples(ownedTimes);
    std::cout << "}}\n";
  }

  INSTANTIATE_TEST_SUITE_P(PayloadSizes, ProducerLogPerformance,
                          ::testing::Values(std::size_t{64}, std::size_t{256}, std::size_t{1024}));
}
