// Allowed loss is an explicit Queue policy. TOML and descriptor registration
// share the same effective defaults and persisted capability checks.
#include "source_fixture.h"

#include "native_fixture.h"
#include "result_assertions.h"

#include <gtest/gtest.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <variant>

namespace
{
  using namespace angaria;
  namespace mapped = angaria::detail::mapped;
  namespace support = angaria::test_support;
  namespace fs = std::filesystem;
  using support::expect_error;
  using support::require_value;

  class LossConfigurationTest : public testing::Test
  {
  protected:
    void SetUp() override
    {
      auto pattern = (fs::temp_directory_path() / "angaria-loss-configuration-XXXXXX").string();
      ASSERT_NE(::mkdtemp(pattern.data()), nullptr);
      _root = pattern;
      _directory = _root / "session";
    }

    void TearDown() override
    {
      std::error_code ignored;
      fs::remove_all(_root, ignored);
    }

    std::string configuration(std::string_view defaultDelivery, std::string_view queueDelivery)
    {
      return std::string{R"toml(config_version = 1
[session]
name = "Allowed-loss configuration"
directory = "session"
max_log_bytes = 2097152
[limits]
tasks = 8
queues = 8
source_sets = 8
publishing_sets = 0
memberships_per_set = 8
fields = 8
message_schemas = 8
symbols = 0
string_pool_bytes = 0
[defaults.queue]
ring_capacity = 2
delivery = ")toml"} + std::string{defaultDelivery} + R"toml("
[[tasks]]
name = "Producer"
[[tasks]]
name = "Consumer"
[[source_sets]]
name = "Inputs"
owner = "Consumer"
[[queues]]
name = "Updates"
producer = "Producer"
source_set = "Inputs"
)toml" + std::string{queueDelivery};
    }

    fs::path write_configuration(const std::string& text)
    {
      const auto file = _root / "configuration.toml";
      std::ofstream output(file);
      output << text;
      if (!output)
      {
        throw std::runtime_error("Cannot write allowed-loss configuration fixture");
      }
      return file;
    }

    fs::path _root;
    fs::path _directory;
  };

  TEST_F(LossConfigurationTest, ProgrammaticDefaultsAllowLossAndQueuesCanOverrideThem)
  {
    SessionOptions options;
    options._name = "Explicit delivery policies";
    options._directory = _directory;
    options._maxLogBytes = 2097152;
    options._limits = CapacityLimits::empty().tasks(4).queues(4).source_sets(2).memberships_per_set(4);
    options._defaults._ringCapacity = 2;
    options._defaults._delivery = DeliveryPolicy::AllowLoss;
    TaskDescriptor producer;
    producer._name = "Producer";
    auto created = require_value(Session::create(options, producer));
    auto registry = created._creator.registry();
    TaskDescriptor consumer;
    consumer._name = "Consumer";
    const auto consumerId = require_value(registry.register_task(consumer));
    SourceQueueSetDescriptor source;
    source._name = "Inputs";
    source._owner = consumerId;
    const auto sourceId = require_value(registry.register_source_set(source));
    QueueDescriptor queue;
    queue._name = "Disposable quotes";
    queue._producer = created._creator.claim()._task;
    queue._sourceSet = sourceId;
    const auto inherited = require_value(registry.register_queue(queue));
    EXPECT_EQ(require_value(registry.describe_queue(inherited))._delivery, DeliveryPolicy::AllowLoss);
    queue._name = "Durable orders";
    queue._delivery = DeliveryPolicy::RecoverFromLog;
    const auto overridden = require_value(registry.register_queue(queue));
    EXPECT_EQ(require_value(registry.describe_queue(overridden))._delivery, DeliveryPolicy::RecoverFromLog);
    EXPECT_EQ(require_value(created._session.options())._defaults._delivery, DeliveryPolicy::AllowLoss);
    const auto header = support::open_runtime_state(_directory)->_controller->configuration();
    EXPECT_EQ(header._formatMinor, 2u);
    EXPECT_EQ(header._requiredFeatures, 15u);
  }

  TEST_F(LossConfigurationTest, TomlDefaultAllowsLossAndExplicitRecoverOverrideSurvivesAttachment)
  {
    const auto file = write_configuration(configuration("allow_loss", "delivery = \"recover_from_log\"\n"));
    auto created = require_value(Session::create_from_file(file, "Producer"));
    EXPECT_EQ(require_value(created._session.options())._defaults._delivery, DeliveryPolicy::AllowLoss);
    const auto queue = require_value(created._session.registry().find_queue("Updates"));
    EXPECT_EQ(require_value(created._session.registry().describe_queue(queue))._delivery, DeliveryPolicy::RecoverFromLog);
    const auto before = support::recovered_image(_directory);
    auto attached = require_value(Session::attach_from_file(file));
    EXPECT_EQ(require_value(attached.registry().describe_queue(queue))._delivery, DeliveryPolicy::RecoverFromLog);
    EXPECT_EQ(support::recovered_image(_directory), before);
  }

  TEST_F(LossConfigurationTest, TomlQueueCanOptIntoLossWhileTheSessionDefaultRemainsRecovery)
  {
    const auto file = write_configuration(configuration("recover_from_log", "delivery = \"allow_loss\"\n"));
    auto created = require_value(Session::create_from_file(file, "Producer"));
    EXPECT_EQ(require_value(created._session.options())._defaults._delivery, DeliveryPolicy::RecoverFromLog);
    const auto queue = require_value(created._session.registry().find_queue("Updates"));
    EXPECT_EQ(require_value(created._session.registry().describe_queue(queue))._delivery, DeliveryPolicy::AllowLoss);
    require_value(Session::attach_from_file(file));
  }

  TEST_F(LossConfigurationTest, CoreProfileRetainsRecoveryButRejectsLossRegistrationBeforeWriting)
  {
    TaskId producer;
    SourceQueueSetId source;
    {
      auto scenario = support::create_source_scenario(_directory);
      producer = scenario._created._creator.claim()._task;
      source = scenario._source;
    }
    // This private offline fixture represents a Session created by the older
    // 2.2/core runtime. It remains usable and is never upgraded in place.
    support::set_atomic_word(_directory, 56, 7);
    auto session = require_value(Session::attach(_directory));
    auto task = require_value(session.attach_task(producer));
    QueueDescriptor queue;
    queue._name = "New queue";
    queue._producer = producer;
    queue._sourceSet = source;
    queue._delivery = DeliveryPolicy::AllowLoss;
    const auto before = support::recovered_image(_directory);
    expect_error(task.registry().register_queue(queue), ErrorCode::UnsupportedPolicy);
    EXPECT_EQ(support::recovered_image(_directory), before);
    queue._delivery = DeliveryPolicy::RecoverFromLog;
    const auto recovered = require_value(task.registry().register_queue(queue));
    EXPECT_EQ(require_value(task.registry().describe_queue(recovered))._delivery, DeliveryPolicy::RecoverFromLog);
    EXPECT_EQ(support::open_runtime_state(_directory)->_controller->configuration()._requiredFeatures, 7u);
  }

  TEST_F(LossConfigurationTest, CoreProfileRejectsTomlLossDeclarationsWithoutChangingTheSession)
  {
    {
      auto created = require_value(Session::create_from_file(
          write_configuration(configuration("recover_from_log", {})), "Producer"));
    }
    support::set_atomic_word(_directory, 56, 7);
    const auto before = support::recovered_image(_directory);
    const auto claims = support::claim_names(_directory);
    const auto file = write_configuration(configuration("recover_from_log", "delivery = \"allow_loss\"\n"));
    expect_error(Session::attach_from_file(file), ErrorCode::UnsupportedPolicy);
    EXPECT_EQ(support::recovered_image(_directory), before);
    EXPECT_EQ(support::claim_names(_directory), claims);
  }

  TEST_F(LossConfigurationTest, CoreProfileCannotStoreAllowedLossDefaults)
  {
    {
      auto created = require_value(Session::create_from_file(
          write_configuration(configuration("allow_loss", {})), "Producer"));
    }
    support::set_atomic_word(_directory, 56, 7);
    const auto before = support::recovered_image(_directory);
    expect_error(Session::attach(_directory), ErrorCode::IoError);
    EXPECT_EQ(support::recovered_image(_directory), before);
  }

  TEST_F(LossConfigurationTest, CoreProfileRejectsRequestedTomlLossDefaultsBeforeConfigurationMatching)
  {
    {
      auto created = require_value(Session::create_from_file(
          write_configuration(configuration("recover_from_log", {})), "Producer"));
    }
    support::set_atomic_word(_directory, 56, 7);
    const auto before = support::recovered_image(_directory);
    const auto file = write_configuration(configuration("allow_loss", "delivery = \"recover_from_log\"\n"));
    expect_error(Session::attach_from_file(file), ErrorCode::UnsupportedPolicy);
    EXPECT_EQ(support::recovered_image(_directory), before);
  }
} // namespace
