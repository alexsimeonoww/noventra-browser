// Copyright 2026 The Helium Authors
// You can use, redistribute, and/or modify this source code under
// the terms of the GPL-3.0 license that can be found in the LICENSE file.

#ifndef CHROME_BROWSER_CRASH_UPLOAD_LIST_HELIUM_CRASH_REPORTING_H_
#define CHROME_BROWSER_CRASH_UPLOAD_LIST_HELIUM_CRASH_REPORTING_H_

namespace helium {

enum class CrashReportingMode {
  kDisabled = -1,
  kAsk,
  kAutomatic,
};

constexpr bool IsValidCrashReportingMode(int value) {
  return value >= static_cast<int>(CrashReportingMode::kDisabled) &&
         value <= static_cast<int>(CrashReportingMode::kAutomatic);
}

CrashReportingMode GetCrashReportingMode();

// Returns true only when policy explicitly disables crash reporting. A policy
// that enables reporting does not constitute user consent.
bool UploadsDisabledByPolicy();

// Whether this process has an explicitly configured crash-upload destination.
bool IsCrashUploadConfigured();

// Applies the stored mode at startup and discards all pending reports when
// reporting is disabled by the user or policy.
void InitializeCrashReportingMode();

// Disables consent, discards existing unrequested reports, stores `mode`, and
// then enables consent only when `mode` is automatic. Explicitly requested
// reports are also discarded when reporting is disabled by the user or policy.
// If policy disables crash reporting, the stored mode is preserved.
void SetCrashReportingMode(CrashReportingMode mode);

}  // namespace helium

#endif  // CHROME_BROWSER_CRASH_UPLOAD_LIST_HELIUM_CRASH_REPORTING_H_
