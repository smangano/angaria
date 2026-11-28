// Private connection-file fixtures. Expected bytes use independent packing;
// create/open agreement alone must not define the native representation.
#include "recorded_queue_fixture.h"
#include "native_file.h"
#include "result_assertions.h"
#include "wire_fixture.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace
{
  using namespace angaria;
  namespace recorded = angaria::detail::recorded;
  namespace mapped = angaria::detail::mapped;
  namespace support = angaria::test_support;
  using support::expect_error;
  using support::put_integer;
  using support::require_value;

  class RecordedQueueFormatTest : public testing::Test
  {
  protected:
    void create()
    {
      require_value(recorded::create_recorded_queue(_options));
    }

    std::array<std::vector<std::byte>, 3> resource_images()
    {
      // Admission must not modify producer ownership, log bytes or consumer
      // control. Snapshot all three resources, not only the altered header.
      const std::array paths{_options._producerPaths._log, _options._producerPaths._owner,
                             _options._connectionPath};
      std::array<std::vector<std::byte>, 3> images;
      for (std::size_t index = 0; index != paths.size(); ++index)
      {
        auto file = require_value(mapped::NativeFile::open_existing(paths[index], mapped::FileAccess::ReadOnly));
        images[index].resize(static_cast<std::size_t>(require_value(file.size())));
        require_value(file.read_exact(0, images[index]));
      }
      return images;
    }

    void expect_rejected_without_writes(ErrorCode code)
    {
      const auto before = resource_images();
      expect_error(recorded::RecordedProducer::open(_options), code);
      expect_error(recorded::RecordedConsumer::open(_options), code);
      EXPECT_EQ(resource_images(), before);
    }

    support::ProducerLogDirectory _directory;
    recorded::RecordedQueueOptions _options{support::recorded_queue_options(_directory.path())};
  };
}

TEST_F(RecordedQueueFormatTest, OfflineHeaderAndEmptyControlsMatchIndependentExpectedBytes)
{
  create();
  auto log = require_value(mapped::NativeFile::open_existing(_options._producerPaths._log, mapped::FileAccess::ReadOnly));
  auto connection = require_value(mapped::NativeFile::open_existing(_options._connectionPath, mapped::FileAccess::ReadOnly));
  const auto logIdentity = require_value(log.identity());
  const auto connectionIdentity = require_value(connection.identity());
  std::array<std::byte, 256> expected{};
  const std::array<unsigned, 8> magic{0x41, 0x4e, 0x47, 0x41, 0x43, 0x4f, 0x4e, 0x4e};
  for (std::size_t index = 0; index != magic.size(); ++index)
  {
    expected[index] = static_cast<std::byte>(magic[index]);
  }
  put_integer(expected, 8, 3, 2);
  put_integer(expected, 10, 0, 2);
  put_integer(expected, 12, 4096, 4);
  put_integer(expected, 16, 0x0102030405060708);
  put_integer(expected, 24, 0x1112131415161718);
  put_integer(expected, 32, 4, 2);
  put_integer(expected, 34, 3, 2);
  put_integer(expected, 36, 6, 2);
  put_integer(expected, 38, 2, 2);
  put_integer(expected, 40, 9);
  put_integer(expected, 48, 1, 2);
  put_integer(expected, 52, 1, 4);
  put_integer(expected, 56, 4096);
  put_integer(expected, 64, 9);
  put_integer(expected, 72, 1);
  put_integer(expected, 80, 2);
  // Filesystem identities and the qualified ABI are native observations, not
  // production encoder output. Every field offset and other value is explicit.
  put_integer(expected, 88, logIdentity._device);
  put_integer(expected, 96, logIdentity._inode);
  put_integer(expected, 104, 1048576);
  put_integer(expected, 112, 128);
  put_integer(expected, 120, _options._log._header._liveAbiProfile, 4);
  put_integer(expected, 124, 2, 2);
  put_integer(expected, 126, 8, 2);
  put_integer(expected, 128, 8);
  put_integer(expected, 136, 1);
  put_integer(expected, 144, 4224);
  put_integer(expected, 152, connectionIdentity._device);
  put_integer(expected, 160, connectionIdentity._inode);
  std::vector<std::byte> image(4224);
  std::copy(expected.begin(), expected.end(), image.begin());
  std::vector<std::byte> actual(static_cast<std::size_t>(require_value(connection.size())));
  require_value(connection.read_exact(0, actual));
  // No endpoint has opened: credit/incarnation, slot stamps, all slot words and
  // reserved padding must be zero independently of the immutable header codec.
  EXPECT_EQ(actual, image);
}

TEST_F(RecordedQueueFormatTest, UnsupportedStoredProfilesRejectBeforeOwnershipStateChanges)
{
  create();
  auto file = require_value(mapped::NativeFile::open_existing(_options._connectionPath, mapped::FileAccess::ReadWrite));
  std::array<std::byte, 256> original{};
  require_value(file.read_exact(0, original));
  struct Mutation
  {
    std::size_t _offset;
    std::size_t _width;
    std::uint64_t _value;
  };
  const std::array mutations{Mutation{8, 2, 4}, Mutation{10, 2, 1}, Mutation{120, 4, 0},
      Mutation{124, 2, 3}, Mutation{126, 2, 4}, Mutation{136, 8, 0}, Mutation{136, 8, 3}};
  for (const auto mutation : mutations)
  {
    SCOPED_TRACE(mutation._offset);
    SCOPED_TRACE(mutation._value);
    auto header = original;
    put_integer(header, mutation._offset, mutation._value, mutation._width);
    // Offline corruption only: there are no live endpoints or atomic accesses
    // to this immutable header. Failed opens must preserve every resource byte.
    require_value(file.write_exact(0, header));
    expect_rejected_without_writes(ErrorCode::UnsupportedPolicy);
  }
}

TEST_F(RecordedQueueFormatTest, MalformedMagicAndMismatchedReservedContextRejectWithoutWrites)
{
  create();
  auto file = require_value(mapped::NativeFile::open_existing(_options._connectionPath, mapped::FileAccess::ReadWrite));
  std::array<std::byte, 256> original{};
  require_value(file.read_exact(0, original));
  auto header = original;
  header[0] = std::byte{0xff};
  require_value(file.write_exact(0, header));
  expect_rejected_without_writes(ErrorCode::IoError);
  for (const auto offset : {std::size_t{32}, std::size_t{168}, std::size_t{255}})
  {
    SCOPED_TRACE(offset);
    header = original;
    header[offset] = std::byte{0xff};
    require_value(file.write_exact(0, header));
    expect_rejected_without_writes(ErrorCode::ConfigurationMismatch);
  }
}
