// Public receive deadlines are checked again after recovery/metadata work.
// The private immutable-read observer delays that work while the gate is owned;
// it does not simulate a message, token, or delivery-state transition.
#include "source_fixture.h"

#include "native_fixture.h"
#include "result_assertions.h"
#include "source_runtime_state.h"

#include <gtest/gtest.h>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <system_error>
#include <thread>
#include <variant>

namespace
{
  using namespace angaria;
  namespace runtime = angaria::detail;
  namespace mapped = angaria::detail::mapped;
  namespace support = angaria::test_support;
  namespace fs = std::filesystem;
  using support::require_value;
  using namespace std::chrono_literals;

  class DelayMetadataOnce final : public runtime::RuntimeAccessReadObserver
  {
  public:
    OperationResult before_read(std::uint64_t, std::uint64_t) override
    {
      ++_reads;
      if (!_delayed)
      {
        _delayed = true;
        // An absolute monotonic target ensures the deliberately delayed read
        // exceeds the 5ms receive deadline, independent of wakeup precision.
        const auto target = std::chrono::steady_clock::now() + 25ms;
        while (std::chrono::steady_clock::now() < target)
        {
          std::this_thread::sleep_until(target);
        }
      }
      return std::monostate{};
    }

    bool _delayed{};
    unsigned _reads{};
  };

  class SourceDeadlineTest : public testing::Test
  {
  protected:
    void SetUp() override
    {
      auto pattern = (fs::temp_directory_path() / "angaria-source-deadlines-XXXXXX").string();
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

  TEST_F(SourceDeadlineTest, ExpiredPositiveWaitAfterRecoveryDoesNotIssueAnAvailableMessage)
  {
    auto scenario = support::create_source_scenario(_directory);
    auto writer = require_value(scenario._created._creator.open_writer(scenario._queue));
    support::send_source_value(scenario, writer, 42);
    auto consumer = require_value(scenario._created._session.attach_task(scenario._consumer));
    DelayMetadataOnce observer;
    runtime::SourceOperationHooks hooks;
    hooks._reads = &observer;
    auto source = require_value(consumer.open_source_set(scenario._source));
    const auto before = support::recovered_image(_directory);
    runtime::SourceAccess::set_hooks(source, &hooks);
    const auto result = source.receive(5ms);
    runtime::SourceAccess::set_hooks(source, nullptr);

    EXPECT_TRUE(observer._delayed);
    EXPECT_GT(observer._reads, 0u);
    EXPECT_TRUE(std::holds_alternative<ReceiveTimeout>(result));
    EXPECT_EQ(support::recovered_image(_directory), before);
    // A timeout allocated no issuance. The next zero-time poll still receives
    // this first message using issuance 1, rather than skipping or consuming it.
    const auto available = source.receive();
    ASSERT_TRUE(std::holds_alternative<Delivery>(available));
    EXPECT_EQ(std::get<Delivery>(available)._token._issuance, 1u);
    require_value(source.acknowledge(std::get<Delivery>(available)._token));
  }

  TEST_F(SourceDeadlineTest, ExistingPinWinsEvenWhenMetadataWorkExceedsThePositiveDeadline)
  {
    auto scenario = support::create_source_scenario(_directory);
    auto writer = require_value(scenario._created._creator.open_writer(scenario._queue));
    support::send_source_value(scenario, writer, 42);
    auto consumer = require_value(scenario._created._session.attach_task(scenario._consumer));
    DelayMetadataOnce observer;
    runtime::SourceOperationHooks hooks;
    hooks._reads = &observer;
    auto source = require_value(consumer.open_source_set(scenario._source));
    const auto first = source.receive();
    ASSERT_TRUE(std::holds_alternative<Delivery>(first));
    const auto& original = std::get<Delivery>(first);
    const auto before = support::recovered_image(_directory);
    runtime::SourceAccess::set_hooks(source, &hooks);
    const auto repeated = source.receive(5ms);
    runtime::SourceAccess::set_hooks(source, nullptr);

    EXPECT_TRUE(observer._delayed);
    ASSERT_TRUE(std::holds_alternative<Delivery>(repeated));
    const auto& current = std::get<Delivery>(repeated);
    EXPECT_EQ(current._token, original._token);
    EXPECT_EQ(current._message._header, original._message._header);
    EXPECT_EQ(current._message._payload.data(), original._message._payload.data());
    EXPECT_EQ(support::recovered_image(_directory), before);
    require_value(source.acknowledge(current._token));
  }

  TEST_F(SourceDeadlineTest, ZeroPollCanIssueAfterItsSingleAvailabilityAttemptDoesMetadataWork)
  {
    auto scenario = support::create_source_scenario(_directory);
    auto writer = require_value(scenario._created._creator.open_writer(scenario._queue));
    support::send_source_value(scenario, writer, 42);
    auto consumer = require_value(scenario._created._session.attach_task(scenario._consumer));
    DelayMetadataOnce observer;
    runtime::SourceOperationHooks hooks;
    hooks._reads = &observer;
    auto source = require_value(consumer.open_source_set(scenario._source));
    runtime::SourceAccess::set_hooks(source, &hooks);
    const auto result = source.receive();
    runtime::SourceAccess::set_hooks(source, nullptr);

    EXPECT_TRUE(observer._delayed);
    ASSERT_TRUE(std::holds_alternative<Delivery>(result));
    EXPECT_EQ(std::get<Delivery>(result)._token._issuance, 1u);
    require_value(source.acknowledge(std::get<Delivery>(result)._token));
  }

  TEST_F(SourceDeadlineTest, UnsupportedLegacyDeliveryDoesNotWaitForAnOccupiedGate)
  {
    for (const auto minor : {std::uint16_t{0}, std::uint16_t{1}})
    {
      const auto directory = _root / ("legacy-" + std::to_string(minor));
      SourceQueueSetId source_id;
      QueueId queue_id;
      {
        auto scenario = support::create_source_scenario(directory);
        source_id = scenario._source;
        queue_id = scenario._queue;
      }
      // Install a predecessor profile only in a pristine, publication-free
      // offline fixture. This is not an application migration operation.
      {
        auto file = require_value(mapped::NativeFile::open_existing(directory / "session.ang",
                                                                     mapped::FileAccess::ReadWrite));
        const std::array<std::byte, 2> format{static_cast<std::byte>(minor)};
        const std::array<std::byte, 8> features{minor == 0 ? std::byte{1} : std::byte{3}};
        require_value(file.write_exact(10, format));
        require_value(file.write_exact(56, features));
      }
      auto session = require_value(Session::attach(directory));
      auto consumer = require_value(session.attach_task("Consumer"));
      auto source = require_value(consumer.open_source_set(source_id));
      const DeliveryToken token{consumer.claim(), source_id, queue_id, 1, 1};
      const auto before = support::recovered_image(directory);
      auto controller = require_value(mapped::SessionController::open_existing(directory));
      {
        auto guard = require_value(controller->acquire_mutation());
        const auto result = source.receive(5ms);
        ASSERT_TRUE(std::holds_alternative<Error>(result));
        EXPECT_EQ(std::get<Error>(result)._code, ErrorCode::UnsupportedPolicy);
        support::expect_error(source.acknowledge(token), ErrorCode::UnsupportedPolicy);
        support::expect_error(source.release(token), ErrorCode::UnsupportedPolicy);
      }
      EXPECT_EQ(support::recovered_image(directory), before);
      require_value(source.close());
    }
  }
} // namespace
