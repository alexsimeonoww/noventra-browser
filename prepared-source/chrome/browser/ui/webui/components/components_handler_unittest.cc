// Copyright 2019 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/ui/webui/components/components_handler.h"

#include "components/component_updater/mock_component_updater_service.h"
#include "content/public/test/test_web_ui.h"
#include "testing/gtest/include/gtest/gtest.h"

class TestComponentsHandler : public ComponentsHandler {
 public:
  explicit TestComponentsHandler(
      component_updater::ComponentUpdateService* component_update_service,
      component_updater::ComponentUpdateService* widevine = nullptr)
      : ComponentsHandler(component_update_service, widevine) {
    set_web_ui(&test_web_ui_);
  }

  const content::TestWebUI& test_web_ui() const { return test_web_ui_; }

 private:
  content::TestWebUI test_web_ui_;
};

TEST(ComponentsHandlerTest, RemovesObserver) {
  testing::NiceMock<component_updater::MockComponentUpdateService> mock_service;
  EXPECT_CALL(mock_service, AddObserver(testing::_)).Times(1);
  EXPECT_CALL(mock_service, RemoveObserver(testing::_)).Times(1);

  {
    TestComponentsHandler handler(&mock_service);
    base::ListValue args;
    args.Append("unused");
    handler.HandleRequestComponentsData(args);
  }
}

TEST(ComponentsHandlerTest, ObservesBothIndependentServices) {
  testing::NiceMock<component_updater::MockComponentUpdateService> generic;
  testing::NiceMock<component_updater::MockComponentUpdateService> widevine;
  EXPECT_CALL(generic, AddObserver(testing::_)).Times(1);
  EXPECT_CALL(generic, RemoveObserver(testing::_)).Times(1);
  EXPECT_CALL(widevine, AddObserver(testing::_)).Times(1);
  EXPECT_CALL(widevine, RemoveObserver(testing::_)).Times(1);
  TestComponentsHandler handler(&generic, &widevine);
  base::ListValue args;
  args.Append("callback");
  handler.HandleRequestComponentsData(args);
}

TEST(ComponentsHandlerTest, RejectsMalformedWidevinePreferenceRequest) {
  testing::NiceMock<component_updater::MockComponentUpdateService> generic;
  TestComponentsHandler handler(&generic);
  base::ListValue args;
  args.Append("invalid-widevine-preference");
  args.Append("not a boolean");
  handler.HandleSetWidevineDownloadsEnabled(args);
  ASSERT_EQ(1u, handler.test_web_ui().call_data().size());
  const auto& call = handler.test_web_ui().call_data().back();
  EXPECT_EQ("cr.webUIResponse", call->function_name());
  EXPECT_EQ("invalid-widevine-preference", call->arg1()->GetString());
  EXPECT_FALSE(call->arg2()->GetBool());
}

TEST(ComponentsHandlerTest, MissingCallbackDoesNotCrashOrSendJavascript) {
  testing::NiceMock<component_updater::MockComponentUpdateService> generic;
  TestComponentsHandler handler(&generic);
  handler.HandleRequestComponentsData(base::ListValue());
  handler.HandleSetWidevineDownloadsEnabled(base::ListValue());
  EXPECT_TRUE(handler.test_web_ui().call_data().empty());
}
