#include <cpptoml.h>
#include <gtest/gtest.h>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace
{
  class ConfigurationExampleTest : public testing::Test
  {
  protected:
    void SetUp() override
    {
      _configuration = cpptoml::parse_file(ANGARIA_CONFIGURATION_EXAMPLE);
    }

    std::shared_ptr<cpptoml::table> _configuration;
  };

  TEST_F(ConfigurationExampleTest, ParsesNestedDefaultsAndTaskKinds)
  {
    EXPECT_EQ(_configuration->get_as<std::int64_t>("config_version").value_or(0), 1);
    EXPECT_EQ(_configuration->get_qualified_as<std::string>("defaults.queue.delivery").value_or(""),
              "recover_from_log");
    EXPECT_EQ(
        _configuration->get_qualified_as<std::string>("defaults.publishing_set.acceptance").value_or(""),
        "all_destinations");
    EXPECT_EQ(_configuration->get_qualified_as<std::int64_t>("limits.queues").value_or(0), 128);

    const auto tasks = _configuration->get_table_array("tasks");
    ASSERT_NE(tasks, nullptr);
    ASSERT_EQ(tasks->get().size(), 4u);
    const auto& strategy = tasks->get()[2];
    EXPECT_EQ(strategy->get_as<std::string>("name").value_or(""), "Strategy");
    EXPECT_EQ(strategy->get_as<std::string>("kind").value_or(""), "thread");
    EXPECT_EQ(strategy->get_as<std::string>("process").value_or(""), "StrategyHost");

    const auto queues = _configuration->get_table_array("queues");
    ASSERT_NE(queues, nullptr);
    ASSERT_EQ(queues->get().size(), 3u);
    EXPECT_EQ(queues->get()[0]->get_as<std::string>("delivery").value_or(""), "allow_loss");
    EXPECT_EQ(queues->get()[2]->get_as<std::int64_t>("priority").value_or(0), 200);
  }

  TEST_F(ConfigurationExampleTest, PreservesWideFieldIdsAndOrderedReferences)
  {
    const auto fields = _configuration->get_table_array("fields");
    ASSERT_NE(fields, nullptr);
    ASSERT_EQ(fields->get().size(), 2u);
    EXPECT_EQ(fields->get()[0]->get_as<std::int64_t>("id").value_or(0), 0xEE000001LL);
    EXPECT_EQ(fields->get()[1]->get_as<std::int64_t>("id").value_or(0), 0xEE000002LL);

    const auto messages = _configuration->get_table_array("messages");
    ASSERT_NE(messages, nullptr);
    ASSERT_EQ(messages->get().size(), 1u);
    const auto messageFields = messages->get()[0]->get_array_of<std::string>("fields");
    ASSERT_TRUE(messageFields);
    EXPECT_EQ(*messageFields, (std::vector<std::string>{"PriceTicks", "Quantity"}));

    const auto publishers = _configuration->get_table_array("publishing_sets");
    ASSERT_NE(publishers, nullptr);
    ASSERT_EQ(publishers->get().size(), 1u);
    const auto destinations = publishers->get()[0]->get_array_of<std::string>("destinations");
    ASSERT_TRUE(destinations);
    EXPECT_EQ(*destinations, (std::vector<std::string>{"MarketDataToStrategy", "MarketDataToRisk"}));
  }
} // namespace
