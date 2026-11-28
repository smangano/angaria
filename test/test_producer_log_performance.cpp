// Private codec cost study, not a live append, messaging or nanomsg benchmark.
// The same logical publication is encoded/decoded through the new caller-buffer
// and borrowed-view API and the current format-2 owning API. Their byte layouts
// and ownership contracts differ: the timing ratio is not a transport speedup.
// Deterministic allocation/lifetime guards fail CI; noisy wall times are reports.

#include "producer_log_format.h"
#include "publication_metadata.h"

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
  // This executable owns its replacement operators. Keep the counter separate
  // from optional runtime diagnostics so the guard also works in ordinary builds.
  thread_local bool count_allocations{};
  thread_local std::uint64_t captured_allocations{};

  void* allocate(std::size_t bytes, std::size_t alignment = 0)
  {
    for (;;)
    {
      void* result{};
      if (alignment == 0)
      {
        result = std::malloc(bytes == 0 ? 1 : bytes);
      }
      else if (::posix_memalign(&result, std::max(alignment, alignof(void*)), bytes == 0 ? 1 : bytes) != 0)
      {
        result = nullptr;
      }
      if (result)
      {
        if (count_allocations)
        {
          ++captured_allocations;
        }
        return result;
      }
      const auto handler = std::get_new_handler();
      if (!handler)
      {
        throw std::bad_alloc{};
      }
      handler();
    }
  }

  void* allocate_nothrow(std::size_t bytes, std::size_t alignment = 0) noexcept
  {
    try
    {
      return allocate(bytes, alignment);
    }
    catch (...)
    {
      return nullptr;
    }
  }
}

void* operator new(std::size_t bytes)
{
  return allocate(bytes);
}
void* operator new[](std::size_t bytes)
{
  return allocate(bytes);
}
void* operator new(std::size_t bytes, std::align_val_t alignment)
{
  return allocate(bytes, static_cast<std::size_t>(alignment));
}
void* operator new[](std::size_t bytes, std::align_val_t alignment)
{
  return allocate(bytes, static_cast<std::size_t>(alignment));
}
void* operator new(std::size_t bytes, const std::nothrow_t&) noexcept
{
  return allocate_nothrow(bytes);
}
void* operator new[](std::size_t bytes, const std::nothrow_t&) noexcept
{
  return allocate_nothrow(bytes);
}
void* operator new(std::size_t bytes, std::align_val_t alignment, const std::nothrow_t&) noexcept
{
  return allocate_nothrow(bytes, static_cast<std::size_t>(alignment));
}
void* operator new[](std::size_t bytes, std::align_val_t alignment, const std::nothrow_t&) noexcept
{
  return allocate_nothrow(bytes, static_cast<std::size_t>(alignment));
}
void operator delete(void* address) noexcept
{
  std::free(address);
}
void operator delete[](void* address) noexcept
{
  std::free(address);
}
void operator delete(void* address, std::size_t) noexcept
{
  std::free(address);
}
void operator delete[](void* address, std::size_t) noexcept
{
  std::free(address);
}
void operator delete(void* address, std::align_val_t) noexcept
{
  std::free(address);
}
void operator delete[](void* address, std::align_val_t) noexcept
{
  std::free(address);
}
void operator delete(void* address, std::size_t, std::align_val_t) noexcept
{
  std::free(address);
}
void operator delete[](void* address, std::size_t, std::align_val_t) noexcept
{
  std::free(address);
}
void operator delete(void* address, const std::nothrow_t&) noexcept
{
  std::free(address);
}
void operator delete[](void* address, const std::nothrow_t&) noexcept
{
  std::free(address);
}
void operator delete(void* address, std::align_val_t, const std::nothrow_t&) noexcept
{
  std::free(address);
}
void operator delete[](void* address, std::align_val_t, const std::nothrow_t&) noexcept
{
  std::free(address);
}

namespace
{
  using namespace angaria;
  namespace recorded = angaria::detail::recorded;
  namespace mapped = angaria::detail::mapped;

  constexpr std::uint64_t owned_offset = 4096;
  constexpr std::uint64_t legacy_offset = 8192;
  constexpr std::uint64_t record_limit = 1048576;
  constexpr std::size_t allocation_cycles = 128;
  constexpr std::size_t sample_cycles = 2048;
  constexpr std::size_t sample_count = 9;

  // Each cycle contributes an observed payload byte and decoded identity. The
  // volatile publication occurs outside timing/capture and prevents dead work.
  volatile std::uint64_t observed_checksum{};

  class AllocationCapture
  {
  public:
    AllocationCapture()
    {
      captured_allocations = 0;
      count_allocations = true;
    }

    ~AllocationCapture()
    {
      count_allocations = false;
    }

    std::uint64_t stop() noexcept
    {
      count_allocations = false;
      return captured_allocations;
    }
  };

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

  CycleResult legacy_cycles(const mapped::PublicationDefinition& publication, std::size_t count)
  {
    CycleResult result;
    for (std::size_t index = 0; index != count; ++index)
    {
      const auto encoded = mapped::encode_publication_body(publication, legacy_offset, record_limit);
      const auto* bytes = std::get_if<std::vector<std::byte>>(&encoded);
      if (!bytes)
      {
        result._valid = false;
        break;
      }
      const auto decoded = mapped::decode_publication_body(*bytes, legacy_offset, record_limit);
      const auto* message = std::get_if<mapped::PublicationDefinition>(&decoded);
      if (!message || message->_payload.size() != publication._payload.size() || message->_destinations.size() != 1)
      {
        result._valid = false;
        break;
      }
      result._checksum += std::to_integer<unsigned char>(message->_payload[index % message->_payload.size()]) +
                          message->_publicationSequence + message->_destinations.front()._queueSequence;
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

  double median(std::array<double, sample_count> values)
  {
    std::sort(values.begin(), values.end());
    return values[values.size() / 2];
  }

  void write_samples(const std::array<double, sample_count>& values)
  {
    std::cout << '[';
    for (std::size_t index = 0; index != values.size(); ++index)
    {
      if (index != 0)
      {
        std::cout << ',';
      }
      std::cout << values[index];
    }
    std::cout << ']';
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

    mapped::PublicationDefinition legacy;
    legacy._producer = header._producer;
    legacy._schema = publication._header._schema;
    legacy._format = publication._header._format;
    legacy._flags = publication._header._flags;
    legacy._producerIncarnation = 1;
    legacy._publicationSequence = 1;
    legacy._publishedAt = publishedAt;
    legacy._topologyRevision = 1;
    legacy._schemaReference = 4096;
    legacy._payload = payload;
    const auto legacyPlanned = mapped::plan_publication_layout(legacy_offset, 1, payload.size(), record_limit);
    ASSERT_TRUE(std::holds_alternative<mapped::PublicationLayout>(legacyPlanned));
    mapped::DestinationDefinition legacyDestination;
    legacyDestination._queue = QueueId{4};
    legacyDestination._queueSequence = 1;
    legacyDestination._enqueuedAt = publishedAt;
    legacyDestination._receiptReference = std::get<mapped::PublicationLayout>(legacyPlanned)._receiptsOffset;
    legacy._destinations.push_back(legacyDestination);

    // Validate meaningful contents independently of the later checksums. The
    // new decoded payload must point inside the caller's encoded record, not a
    // hidden owned copy. Format-2 intentionally returns its own owning values.
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
    const auto legacyEncoded = mapped::encode_publication_body(legacy, legacy_offset, record_limit);
    ASSERT_TRUE(std::holds_alternative<std::vector<std::byte>>(legacyEncoded));
    const auto legacyDecoded = mapped::decode_publication_body(
        std::get<std::vector<std::byte>>(legacyEncoded), legacy_offset, record_limit);
    ASSERT_TRUE(std::holds_alternative<mapped::PublicationDefinition>(legacyDecoded));
    EXPECT_EQ(std::get<mapped::PublicationDefinition>(legacyDecoded), legacy);

    const auto ownedWarmup = owned_cycles(publication, header, buffer, allocation_cycles);
    const auto legacyWarmup = legacy_cycles(legacy, allocation_cycles);
    ASSERT_TRUE(ownedWarmup._valid && legacyWarmup._valid);
    ASSERT_EQ(ownedWarmup._checksum, legacyWarmup._checksum);

    CycleResult ownedCaptured;
    std::uint64_t ownedAllocations{};
    {
      AllocationCapture capture;
      ownedCaptured = owned_cycles(publication, header, buffer, allocation_cycles);
      ownedAllocations = capture.stop();
    }
    CycleResult legacyCaptured;
    std::uint64_t legacyAllocations{};
    {
      AllocationCapture capture;
      legacyCaptured = legacy_cycles(legacy, allocation_cycles);
      legacyAllocations = capture.stop();
    }
    ASSERT_TRUE(ownedCaptured._valid && legacyCaptured._valid);
    EXPECT_EQ(ownedCaptured._checksum, legacyCaptured._checksum);
    EXPECT_EQ(ownedAllocations, 0u);
    // The owning control also establishes that this executable's counter sees
    // allocation requests; zero on the new path is not a disabled collector.
    EXPECT_GT(legacyAllocations, 0u);

    bool valid = true;
    std::uint64_t expectedChecksum{};
    for (std::size_t index = 0; index != sample_cycles; ++index)
    {
      expectedChecksum += std::to_integer<unsigned char>(payload[index % payload.size()]) + 2;
    }
    std::array<double, sample_count> ownedTimes{};
    std::array<double, sample_count> legacyTimes{};
    for (std::size_t sample = 0; sample != sample_count; ++sample)
    {
      const auto measureOwned = [&]
      {
        ownedTimes[sample] = measure_cycle_ns(
            [&]
            {
              return owned_cycles(publication, header, buffer, sample_cycles);
            }, expectedChecksum, valid);
      };
      const auto measureLegacy = [&]
      {
        legacyTimes[sample] = measure_cycle_ns(
            [&]
            {
              return legacy_cycles(legacy, sample_cycles);
            }, expectedChecksum, valid);
      };
      // Alternate adjacent blocks to reduce a fixed first/second ordering bias.
      if (sample % 2 == 0)
      {
        measureOwned();
        measureLegacy();
      }
      else
      {
        measureLegacy();
        measureOwned();
      }
    }
    ASSERT_TRUE(valid);
    const auto ownedNs = median(ownedTimes);
    const auto legacyNs = median(legacyTimes);
    ASSERT_GT(ownedNs, 0.0);
    ASSERT_GT(legacyNs, 0.0);
    const auto ownedRate = 1e9 / ownedNs;
    const auto legacyRate = 1e9 / legacyNs;
    RecordProperty("payload_bytes", static_cast<int>(payloadBytes));
    RecordProperty("owned_borrowed_median_cycle_ns", std::to_string(ownedNs));
    RecordProperty("legacy_owning_median_cycle_ns", std::to_string(legacyNs));
    RecordProperty("owned_borrowed_cycles_per_second", std::to_string(ownedRate));
    RecordProperty("legacy_owning_cycles_per_second", std::to_string(legacyRate));
    RecordProperty("owned_borrowed_allocations_per_cycle", std::to_string(double(ownedAllocations) / allocation_cycles));
    RecordProperty("legacy_owning_allocations_per_cycle", std::to_string(double(legacyAllocations) / allocation_cycles));
    std::cout << std::fixed << std::setprecision(3)
              << "CODEC_PERFORMANCE {\"payload_bytes\":" << payloadBytes
              << ",\"samples\":" << sample_count << ",\"cycles_per_sample\":" << sample_cycles
              << ",\"owned_record_bytes\":" << layout._bytes
              << ",\"legacy_body_bytes\":" << std::get<std::vector<std::byte>>(legacyEncoded).size()
              << ",\"legacy_full_record_bytes\":" << std::get<mapped::PublicationLayout>(legacyPlanned)._totalBytes
              << ",\"owned_borrowed\":{\"median_cycle_ns\":" << ownedNs
              << ",\"cycles_per_second\":" << ownedRate
              << ",\"allocations_per_cycle\":" << double(ownedAllocations) / allocation_cycles
              << ",\"cycle_ns_samples\":";
    write_samples(ownedTimes);
    std::cout << "},\"legacy_owning\":{\"median_cycle_ns\":" << legacyNs
              << ",\"cycles_per_second\":" << legacyRate
              << ",\"allocations_per_cycle\":" << double(legacyAllocations) / allocation_cycles
              << ",\"cycle_ns_samples\":";
    write_samples(legacyTimes);
    std::cout << "}}\n";
  }

  INSTANTIATE_TEST_SUITE_P(PayloadSizes, ProducerLogPerformance,
                          ::testing::Values(std::size_t{64}, std::size_t{256}, std::size_t{1024}));
}
