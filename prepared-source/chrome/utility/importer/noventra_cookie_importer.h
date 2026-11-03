// Copyright 2026 Noventra contributors
// SPDX-License-Identifier: GPL-3.0-only
#ifndef CHROME_UTILITY_IMPORTER_NOVENTRA_COOKIE_IMPORTER_H_
#define CHROME_UTILITY_IMPORTER_NOVENTRA_COOKIE_IMPORTER_H_

#include <cstdint>
#include <vector>

#include "base/functional/callback.h"
#include "base/types/expected.h"
#include "chrome/utility/importer/chrome_password_importer.h"
#include "net/cookies/canonical_cookie.h"

namespace noventra {
enum class CookieImportError {
  kReadFailed,
  kUnsupportedSchema,
  kUnlockFailed,
  kUnsupportedEncryption,
  kDecryptFailed,
  kCancelled,
  kTooLarge,
};
struct CookieImportBatch {
  std::vector<net::CanonicalCookie> cookies;
  uint32_t skipped = 0;
};
using CookieImportResult = base::expected<CookieImportBatch, CookieImportError>;
CookieImportResult ReadChromeCookies(const base::FilePath& profile,
                                     SourcePasswordKeyLookup source_key,
                                     base::RepeatingCallback<bool()> cancelled);
CookieImportResult ReadFirefoxCookies(
    const base::FilePath& profile,
    base::RepeatingCallback<bool()> cancelled);
const char* CookieImportErrorMessage(CookieImportError error);
}  // namespace noventra
#endif
