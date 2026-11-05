// Copyright 2015 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/component_updater/chrome_component_updater_configurator.h"

#include <memory>
#include <string>
#include <vector>

#include "base/command_line.h"
#include "base/memory/ref_counted.h"
#include "base/test/task_environment.h"
#include "base/time/time.h"
#include "components/component_updater/component_updater_command_line_config_policy.h"
#include "components/component_updater/component_updater_switches.h"
#include "components/component_updater/configurator_impl.h"
#include "components/prefs/testing_pref_service.h"
#include "components/update_client/configurator.h"
#include "components/update_client/update_client.h"
#include "components/update_client/update_query_params.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "url/gurl.h"

namespace component_updater {

class ChromeComponentUpdaterConfiguratorTest : public testing::Test {
 protected:
  // Overrides from testing::Test.
  void SetUp() override;

  TestingPrefServiceSimple* pref_service() { return pref_service_.get(); }

 private:
  base::test::TaskEnvironment environment_;
  std::unique_ptr<TestingPrefServiceSimple> pref_service_;
};

void ChromeComponentUpdaterConfiguratorTest::SetUp() {
  pref_service_ = std::make_unique<TestingPrefServiceSimple>();
  update_client::RegisterPrefs(pref_service_->registry());
  pref_service_->registry()->RegisterBooleanPref(
      kNoventraWidevineDownloadsEnabled, false);
}

TEST_F(ChromeComponentUpdaterConfiguratorTest,
       WidevineRequiresIndependentConsent) {
  base::CommandLine cmdline(*base::CommandLine::ForCurrentProcess());
  const auto config =
      MakeWidevineComponentUpdaterConfigurator(&cmdline, pref_service());
  EXPECT_TRUE(config->UpdateUrl().empty());
  EXPECT_TRUE(config->PingUrl().empty());

  pref_service()->SetBoolean(kNoventraWidevineDownloadsEnabled, true);
  EXPECT_EQ(std::vector<GURL>{GURL(
                "https://clients2.google.com/service/update2/json")},
            config->UpdateUrl());
  EXPECT_TRUE(config->PingUrl().empty());
  EXPECT_TRUE(config->EnabledCupSigning());

  pref_service()->SetBoolean(kNoventraWidevineDownloadsEnabled, false);
  EXPECT_TRUE(config->UpdateUrl().empty());
}

TEST_F(ChromeComponentUpdaterConfiguratorTest,
       WidevineIgnoresEndpointOverrides) {
  base::CommandLine cmdline(*base::CommandLine::ForCurrentProcess());
  cmdline.AppendSwitchASCII(
      switches::kComponentUpdater,
      "url-source=https://noventra-services-are-disabled.qjz9zk/");
  pref_service()->SetBoolean(kNoventraWidevineDownloadsEnabled, true);
  const auto config =
      MakeWidevineComponentUpdaterConfigurator(&cmdline, pref_service());
  EXPECT_EQ(std::vector<GURL>{GURL(
                "https://clients2.google.com/service/update2/json")},
            config->UpdateUrl());
  EXPECT_TRUE(config->PingUrl().empty());
}

TEST_F(ChromeComponentUpdaterConfiguratorTest, TestDisablePings) {
  base::CommandLine cmdline(*base::CommandLine::ForCurrentProcess());
  cmdline.AppendSwitchASCII(switches::kComponentUpdater, "disable-pings");

  const auto config(
      MakeChromeComponentUpdaterConfigurator(&cmdline, pref_service()));

  const std::vector<GURL> pingUrls = config->PingUrl();
  EXPECT_TRUE(pingUrls.empty());
}

TEST_F(ChromeComponentUpdaterConfiguratorTest, TestFastUpdate) {
  base::CommandLine cmdline(*base::CommandLine::ForCurrentProcess());
  cmdline.AppendSwitchASCII(switches::kComponentUpdater, "fast-update");

  const auto config(
      MakeChromeComponentUpdaterConfigurator(&cmdline, pref_service()));

  CHECK_EQ(base::Seconds(10), config->InitialDelay());
  CHECK_EQ(base::Hours(5), config->NextCheckDelay());
  CHECK_EQ(base::Seconds(2), config->OnDemandDelay());
  CHECK_EQ(base::Seconds(10), config->UpdateDelay());
}

TEST_F(ChromeComponentUpdaterConfiguratorTest,
       DisabledServicesCannotBeBypassedByEndpointOverride) {
  const char overrideUrl[] = "http://0.0.0.0/";

  base::CommandLine cmdline(*base::CommandLine::ForCurrentProcess());

  std::string val = "url-source";
  val.append("=");
  val.append(overrideUrl);
  cmdline.AppendSwitchASCII(switches::kComponentUpdater, val.c_str());

  const auto config(
      MakeChromeComponentUpdaterConfigurator(&cmdline, pref_service()));

  const std::vector<GURL> urls = config->UpdateUrl();

  EXPECT_TRUE(urls.empty());
  EXPECT_TRUE(config->PingUrl().empty());
}

TEST_F(ChromeComponentUpdaterConfiguratorTest, TestSwitchRequestParam) {
  base::CommandLine cmdline(*base::CommandLine::ForCurrentProcess());
  cmdline.AppendSwitchASCII(switches::kComponentUpdater, "test-request");

  const auto config(
      MakeChromeComponentUpdaterConfigurator(&cmdline, pref_service()));

  EXPECT_FALSE(config->ExtraRequestParams().empty());
}

TEST_F(ChromeComponentUpdaterConfiguratorTest,
       DisabledServicesDoNotCreatePlaceholderRequests) {
  base::CommandLine cmdline(*base::CommandLine::ForCurrentProcess());
  const auto config(
      MakeChromeComponentUpdaterConfigurator(&cmdline, pref_service()));
  const auto urls = config->UpdateUrl();

  EXPECT_TRUE(urls.empty());
  EXPECT_TRUE(config->PingUrl().empty());
}

TEST_F(ChromeComponentUpdaterConfiguratorTest, TestEnabledCupSigning) {
  base::CommandLine cmdline(*base::CommandLine::ForCurrentProcess());
  const auto config(
      MakeChromeComponentUpdaterConfigurator(&cmdline, pref_service()));

  // Generic custom service proxies retain their existing protocol. The
  // independent Google Widevine client requires CUP in its positive test.
  EXPECT_FALSE(config->EnabledCupSigning());
}

TEST_F(ChromeComponentUpdaterConfiguratorTest,
       DisabledCoreUpdaterDoesNotRestoreImplicitEndpoints) {
  base::CommandLine* cmdline = base::CommandLine::ForCurrentProcess();

  {
    const auto config(
        MakeChromeComponentUpdaterConfigurator(cmdline, pref_service()));
    const auto urls = config->UpdateUrl();
    EXPECT_TRUE(urls.empty());
    ASSERT_EQ(config->UpdateUrl(), config->PingUrl());
  }

  // The fork deliberately removes the core updater's implicit endpoints,
  // regardless of transport filtering. Acquiring Widevine uses the separate,
  // explicitly enabled HTTPS configurator tested above.
  {
    const ConfiguratorImpl config(
        ComponentUpdaterCommandLineConfigPolicy(cmdline), true);
    const auto urls = config.UpdateUrl();
    EXPECT_TRUE(urls.empty());
    ASSERT_EQ(config.UpdateUrl(), config.PingUrl());
  }

  {
    const ConfiguratorImpl config(
        ComponentUpdaterCommandLineConfigPolicy(cmdline), false);
    const auto urls = config.UpdateUrl();
    EXPECT_TRUE(urls.empty());
    ASSERT_EQ(config.UpdateUrl(), config.PingUrl());
  }
}

TEST_F(ChromeComponentUpdaterConfiguratorTest, TestProdId) {
  base::CommandLine cmdline(*base::CommandLine::ForCurrentProcess());
  const auto config(
      MakeChromeComponentUpdaterConfigurator(&cmdline, pref_service()));
  EXPECT_STREQ(update_client::UpdateQueryParams::GetProdIdString(
                   update_client::UpdateQueryParams::ProdId::CHROME),
               config->GetProdId().c_str());
}

}  // namespace component_updater
