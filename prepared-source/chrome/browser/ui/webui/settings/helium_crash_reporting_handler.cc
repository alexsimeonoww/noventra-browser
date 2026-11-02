// Copyright 2026 The Helium Authors
// You can use, redistribute, and/or modify this source code under
// the terms of the GPL-3.0 license that can be found in the LICENSE file.

#include "chrome/browser/ui/webui/settings/helium_crash_reporting_handler.h"

#include "base/functional/bind.h"
#include "base/values.h"
#include "chrome/browser/browser_process.h"
#include "chrome/browser/crash_upload_list/helium_crash_reporting.h"
#include "chrome/common/pref_names.h"
#include "components/metrics/metrics_pref_names.h"
#include "content/public/browser/web_ui.h"

namespace settings {

HeliumCrashReportingHandler::HeliumCrashReportingHandler() = default;
HeliumCrashReportingHandler::~HeliumCrashReportingHandler() = default;

void HeliumCrashReportingHandler::RegisterMessages() {
  web_ui()->RegisterMessageCallback(
      "getHeliumCrashReporting",
      base::BindRepeating(&HeliumCrashReportingHandler::HandleGetState,
                          base::Unretained(this)));
  web_ui()->RegisterMessageCallback(
      "setHeliumCrashReportingMode",
      base::BindRepeating(&HeliumCrashReportingHandler::HandleSetMode,
                          base::Unretained(this)));
}

void HeliumCrashReportingHandler::OnJavascriptAllowed() {
  pref_member_ = std::make_unique<IntegerPrefMember>();
  pref_member_->Init(
      prefs::kHeliumCrashReportingMode, g_browser_process->local_state(),
      base::BindRepeating(&HeliumCrashReportingHandler::OnPrefChanged,
                          base::Unretained(this)));
  policy_pref_member_ = std::make_unique<BooleanPrefMember>();
  policy_pref_member_->Init(
      metrics::prefs::kMetricsReportingEnabled,
      g_browser_process->local_state(),
      base::BindRepeating(&HeliumCrashReportingHandler::OnPrefChanged,
                          base::Unretained(this)));
}

void HeliumCrashReportingHandler::OnJavascriptDisallowed() {
  policy_pref_member_.reset();
  pref_member_.reset();
}

base::DictValue HeliumCrashReportingHandler::CreateState() const {
  CHECK(pref_member_);
  const bool managed = helium::UploadsDisabledByPolicy();
  const helium::CrashReportingMode mode =
      managed ? helium::CrashReportingMode::kDisabled
              : helium::GetCrashReportingMode();

  return base::DictValue()
      .Set("mode", static_cast<int>(mode))
      .Set("managed", managed)
      .Set("configured", helium::IsCrashUploadConfigured());
}

void HeliumCrashReportingHandler::HandleGetState(const base::ListValue& args) {
  AllowJavascript();
  CHECK_EQ(args.size(), 1u);
  ResolveJavascriptCallback(args[0], CreateState());
}

void HeliumCrashReportingHandler::HandleSetMode(const base::ListValue& args) {
  CHECK_EQ(args.size(), 1u);

  const int mode = args[0].GetInt();
  if (!helium::IsValidCrashReportingMode(mode)) {
    return;
  }
  helium::SetCrashReportingMode(
      static_cast<helium::CrashReportingMode>(mode));
}

void HeliumCrashReportingHandler::OnPrefChanged() {
  FireWebUIListener("helium-crash-reporting-change", CreateState());
}

}  // namespace settings
