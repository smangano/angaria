// An independently executed producer encounters real EFBIG on file growth
// while the parent consumer owns a borrowed message. Native observations only
// establish that public rollback/retry preserves accepted history and the pin.
#include "source_fixture.h"

#include "native_fixture.h"
#include "native_process.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <system_error>
#include <utility>
#include <variant>
#include <vector>

#ifndef ANGARIA_NATIVE_TRANSPORT_IO_WORKER
#error ANGARIA_NATIVE_TRANSPORT_IO_WORKER must name the independent I/O worker
#endif

namespace
{
  using namespace angaria;
  namespace runtime = angaria::detail;
  namespace support = angaria::test_support;
  namespace fs = std::filesystem;
  using support::require_value;

  class TransportIoTest : public testing::Test
  {
  protected:
    void SetUp() override
    {
      auto pattern = (fs::temp_directory_path() / "angaria-transport-io-XXXXXX").string();
      ASSERT_NE(::mkdtemp(pattern.data()), nullptr);
      _root = pattern;
      _directory = _root / "session";
    }

    void TearDown() override
    {
      std::error_code ignored;
      fs::remove_all(_root, ignored);
    }

    fs::path _root;
    fs::path _directory;
  };

  TEST_F(TransportIoTest, DeniedFileGrowthRollsBackWithoutChangingAnExistingBorrowAndTheSameIdentityCanRetry)
  {
    auto scenario = support::create_source_scenario(_directory);
    const auto codec = support::source_codec(scenario);
    PublicationId prior;
    {
      auto writer = require_value(scenario._created._creator.open_writer(scenario._queue));
      prior = support::send_source_value(scenario, writer, 42, 0xa5);
    }
    // Release the creator's producer claim before starting the independent
    // producer. The parent's consumer claim stays live throughout the test.
    {
      auto endingProducer = std::move(scenario._created._creator);
    }
    auto consumer = require_value(scenario._created._session.attach_task("Consumer"));
    auto source = require_value(consumer.open_source_set("Inputs"));
    auto incoming = source.receive();
    ASSERT_TRUE(std::holds_alternative<Delivery>(incoming));
    const auto original = std::get<Delivery>(std::move(incoming));
    const auto saved = require_value(original._message.copy_message(4));
    ASSERT_EQ(original._message._header._publication, prior);

    support::NativeProcess producer({ANGARIA_NATIVE_TRANSPORT_IO_WORKER, _directory.string(),
        std::to_string(prior._producer._incarnation), std::to_string(prior._sequence)});
    producer.expect('I');
    const auto incarnation = producer.receive<std::uint64_t>();
    const auto sequence = producer.receive<std::uint64_t>();
    const PublicationId retried{{scenario._created._session.identity(), prior._producer._task, incarnation}, sequence};
    EXPECT_GT(incarnation, prior._producer._incarnation);
    const auto before = support::journal_snapshot(_directory);
    std::uint64_t priorReference{};
    std::vector<std::byte> immutable;
    {
      runtime::RuntimeAccess access(support::open_runtime_state(_directory));
      const auto publications = access.record_references(9);
      ASSERT_EQ(publications.size(), 1u);
      priorReference = publications.front();
      immutable = access.immutable_record(priorReference, 9);
    }
    producer.proceed();
    producer.expect('F');
    EXPECT_EQ(producer.receive<std::uint64_t>(), static_cast<std::uint64_t>(ErrorCode::IoError));
    EXPECT_EQ(producer.receive<std::uint64_t>(), before._fileBytes);
    // The rejected append changed journal scratch bytes, but recovered roots,
    // file length, immutable acceptance, and the existing Source pin are exact.
    EXPECT_EQ(support::journal_snapshot(_directory), before);
    {
      runtime::RuntimeAccess access(support::open_runtime_state(_directory));
      EXPECT_EQ(access.record_references(9).size(), 1u);
      EXPECT_EQ(access.immutable_record(priorReference, 9), immutable);
    }
    const auto repeated = source.receive();
    ASSERT_TRUE(std::holds_alternative<Delivery>(repeated));
    const auto repeatedDelivery = std::get<Delivery>(repeated);
    EXPECT_EQ(repeatedDelivery._token, original._token);
    EXPECT_EQ(repeatedDelivery._message._header, original._message._header);
    EXPECT_EQ(repeatedDelivery._message._payload.data(), original._message._payload.data());
    EXPECT_EQ(std::vector<std::byte>(original._message._payload.begin(), original._message._payload.end()),
              saved._payload);

    producer.proceed();
    producer.expect('S');
    require_value(source.acknowledge(original._token));
    const auto next = source.receive();
    ASSERT_TRUE(std::holds_alternative<Delivery>(next));
    const auto delivery = std::get<Delivery>(next);
    EXPECT_EQ(delivery._message._header._publication, retried);
    EXPECT_EQ(delivery._token._sequence, 2u);
    EXPECT_EQ(delivery._message._header._flags, 0x49u);
    const auto copied = require_value(delivery._message.copy_message(4));
    EXPECT_EQ(std::get<std::uint32_t>(require_value(copied.decode(codec)).front()), 99u);
    require_value(source.acknowledge(delivery._token));
    EXPECT_TRUE(std::holds_alternative<ReceiveTimeout>(source.receive()));
    {
      runtime::RuntimeAccess access(support::open_runtime_state(_directory));
      EXPECT_EQ(access.record_references(9).size(), 2u);
      EXPECT_EQ(access.immutable_record(priorReference, 9), immutable);
    }
    require_value(source.close());
    producer.proceed();
    producer.wait_success();
    EXPECT_EQ(std::get<std::uint32_t>(require_value(saved.decode(codec)).front()), 42u);
  }
}
