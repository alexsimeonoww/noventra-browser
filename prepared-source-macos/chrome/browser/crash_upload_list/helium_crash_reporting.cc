// Copyright 2026 The Helium Authors
// You can use, redistribute, and/or modify this source code under
// the terms of the GPL-3.0 license that can be found in the LICENSE file.

#include "chrome/browser/crash_upload_list/helium_crash_reporting.h"

#include <vector>

#include "base/command_line.h"
#include "base/environment.h"
#include "base/functional/bind.h"
#include "build/build_config.h"
#include "base/logging.h"
#include "base/no_destructor.h"
#include "chrome/browser/browser_process.h"
#include "chrome/common/pref_names.h"
#include "components/crash/core/app/crashpad.h"
#include "components/metrics/metrics_pref_names.h"
#include "components/prefs/pref_change_registrar.h"
#include "components/prefs/pref_service.h"

#if BUILDFLAG(IS_WIN)
#include <windows.h>
#include "chrome/install_static/install_util.h"
#endif

namespace helium {

namespace {

void DeletePendingReports(bool include_explicitly_requested) {
  std::vector<crash_reporter::Report> reports;
  crash_reporter::GetReports(&reports);

  for (const auto& report : reports) {
    if (report.state != crash_reporter::ReportUploadState::Pending &&
        report.state !=
            crash_reporter::ReportUploadState::Pending_UserRequested) {
      continue;
    }
    if (!include_explicitly_requested &&
        report.state ==
            crash_reporter::ReportUploadState::Pending_UserRequested) {
      continue;
    }

    if (!crash_reporter::DeleteCrashReport(report.local_id)) {
      LOG(ERROR) << "Failed to delete crash report: " << report.local_id;
    }
  }
}

}  // namespace

CrashReportingMode GetCrashReportingMode() {
  PrefService* local_state = g_browser_process->local_state();
  CHECK(local_state);

  const int value =
      local_state->GetInteger(prefs::kHeliumCrashReportingMode);
  CHECK(IsValidCrashReportingMode(value));

  return static_cast<CrashReportingMode>(value);
}

bool UploadsDisabledByPolicy() {
  PrefService* local_state = g_browser_process->local_state();
  if (!local_state) {
    return false;
  }

  const PrefService::Preference* preference =
      local_state->FindPreference(metrics::prefs::kMetricsReportingEnabled);
  return preference && preference->IsManaged() &&
         !local_state->GetBoolean(metrics::prefs::kMetricsReportingEnabled);
}

bool IsCrashUploadConfigured() {
#if BUILDFLAG(IS_WIN)
  // Match Crashpad's environment override. Environment::GetVar treats an
  // empty Windows value as absent, so both paths then fall back to the switch.
  const auto server = base::Environment::Create()->GetVar(
      "CHROME_CRASHPAD_SERVER_URL");
  if (server.has_value()) {
    return !server->empty();
  }
  // chrome_elf resolves the first matching switch with this parser rather
  // than base::CommandLine's last-value semantics.
  const auto destination = install_static::GetCommandLineSwitch(
      ::GetCommandLine(), L"custom-crash-server-url");
  return destination.has_value() && !destination->empty();
#else
  // Match CrashReporterClient's ASCII-only value and duplicate-switch handling.
  return !base::CommandLine::ForCurrentProcess()
              ->GetSwitchValueASCII("custom-crash-server-url")
              .empty();
#endif
}

void InitializeCrashReportingMode() {
  PrefService* local_state = g_browser_process->local_state();
  if (!local_state) {
    return;
  }

  const int stored_mode =
      local_state->GetInteger(prefs::kHeliumCrashReportingMode);
  if (!IsValidCrashReportingMode(stored_mode)) {
    LOG(ERROR) << "Invalid crash reporting mode " << stored_mode
               << "; resetting to default";
    local_state->SetInteger(
        prefs::kHeliumCrashReportingMode,
        static_cast<int>(CrashReportingMode::kAsk));
  }

  static base::NoDestructor<PrefChangeRegistrar> policy_registrar;
  policy_registrar->Init(local_state);
  if (!policy_registrar->IsObserved(
          metrics::prefs::kMetricsReportingEnabled)) {
    policy_registrar->Add(
        metrics::prefs::kMetricsReportingEnabled,
        base::BindRepeating(&helium::InitializeCrashReportingMode));
  }

  crash_reporter::SetUploadConsentFromUser(false);

  if (UploadsDisabledByPolicy()) {
    // Policy overrides the stored mode without changing the user's selection.
    // Explicit requests bypass Crashpad's global upload toggle, so discard
    // those reports as well while reporting is forbidden by policy.
    DeletePendingReports(/*include_explicitly_requested=*/true);
    return;
  }

  const CrashReportingMode mode = GetCrashReportingMode();
  switch (mode) {
    case CrashReportingMode::kDisabled:
      // Disabled mode must not retain any uploadable reports.
      DeletePendingReports(/*include_explicitly_requested=*/true);
      return;
    case CrashReportingMode::kAsk:
      // Leave reports pending so the crash dialog can offer them.
      return;
    case CrashReportingMode::kAutomatic:
      // Allow Crashpad to upload pending reports without prompting.
      crash_reporter::SetUploadConsentFromUser(IsCrashUploadConfigured());
      return;
  }
}

void SetCrashReportingMode(CrashReportingMode mode) {
  PrefService* local_state = g_browser_process->local_state();
  if (!local_state) {
    return;
  }

  // Make mode changes prospective: stop uploads and discard existing
  // reports before applying the new mode. Explicitly requested reports are
  // preserved unless reporting is disabled by the user or policy.
  crash_reporter::SetUploadConsentFromUser(false);

  const bool uploads_disabled_by_policy = UploadsDisabledByPolicy();
  DeletePendingReports(
      /*include_explicitly_requested=*/
      uploads_disabled_by_policy || mode == CrashReportingMode::kDisabled);

  if (uploads_disabled_by_policy) {
    // Preserve the user's stored selection while policy is enforced.
    return;
  }

  local_state->SetInteger(prefs::kHeliumCrashReportingMode,
                          static_cast<int>(mode));
  if (mode == CrashReportingMode::kAutomatic) {
    crash_reporter::SetUploadConsentFromUser(IsCrashUploadConfigured());
  }
}

}  // namespace helium
