// Copyright 2026 Noventra contributors
// SPDX-License-Identifier: GPL-3.0-only
#include "chrome/utility/importer/noventra_cookie_importer.h"

#include <algorithm>
#include <array>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "base/containers/span.h"
#include "base/files/file_util.h"
#include "base/files/scoped_temp_dir.h"
#include "base/strings/cstring_view.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_util.h"
#include "base/time/time.h"
#include "components/os_crypt/async/common/algorithm.mojom.h"
#include "crypto/hash.h"
#include "crypto/secure_util.h"
#include "net/cookies/cookie_util.h"
#include "sql/database.h"
#include "sql/statement.h"

namespace noventra {
namespace {
constexpr size_t kMaximumRows = 100000;
constexpr size_t kMaximumCookies = 10000;
constexpr size_t kMaximumDecodedBytes = 64 * 1024 * 1024;
constexpr int64_t kMaximumWindowsMicroseconds = 265046774399999999;
constexpr int64_t kMaximumUnixMicroseconds = 253402300799999999;
constexpr auto kV10Key =
    std::to_array<uint8_t>({0xfd, 0x62, 0x1f, 0xe5, 0xa2, 0xb4, 0x02, 0x53,
                            0x9d, 0xfa, 0x14, 0x7c, 0xa9, 0x27, 0x27, 0x78});

bool IntegerColumns(sql::Statement& row, std::initializer_list<int> columns) {
  return std::ranges::all_of(columns, [&](int column) {
    return row.GetColumnType(column) == sql::ColumnType::kInteger;
  });
}
bool BooleanColumn(sql::Statement& row, int column) {
  return row.GetColumnType(column) == sql::ColumnType::kInteger &&
         (row.ColumnInt(column) == 0 || row.ColumnInt(column) == 1);
}

bool TextColumns(sql::Statement& row, std::initializer_list<int> columns) {
  return std::ranges::all_of(columns, [&](int column) {
    return row.GetColumnType(column) == sql::ColumnType::kText;
  });
}
bool HasColumns(sql::Database& db,
                base::cstring_view table,
                std::initializer_list<base::cstring_view> names) {
  return std::ranges::all_of(names, [&](base::cstring_view name) {
    return db.DoesColumnExist(table, name);
  });
}
std::string Column(sql::Database& db,
                   base::cstring_view table,
                   base::cstring_view name,
                   base::cstring_view fallback) {
  return std::string(db.DoesColumnExist(table, name) ? name : fallback);
}
std::optional<base::Time> WindowsTime(int64_t value) {
  if (value < 0 || value > kMaximumWindowsMicroseconds) {
    return std::nullopt;
  }
  return value == 0 ? base::Time()
                    : base::Time::FromDeltaSinceWindowsEpoch(
                          base::Microseconds(value));
}
std::optional<base::Time> UnixTime(int64_t value, int64_t multiplier) {
  if (value < 0 || value > kMaximumUnixMicroseconds / multiplier) {
    return std::nullopt;
  }
  return base::Time::UnixEpoch() + base::Microseconds(value * multiplier);
}
std::unique_ptr<net::CanonicalCookie> MakeCookie(const std::string& host,
                                                 const std::string& name,
                                                 const std::string& value,
                                                 const std::string& path,
                                                 base::Time creation,
                                                 base::Time expiration,
                                                 base::Time access,
                                                 bool secure,
                                                 bool http_only,
                                                 int same_site,
                                                 int priority,
                                                 int source_scheme,
                                                 int source_port) {
  if (host.empty() || host.size() > 254 || path.size() > 4096 ||
      !base::IsStringUTF8AllowingNoncharacters(name) ||
      !base::IsStringUTF8AllowingNoncharacters(value) ||
      !base::IsStringUTF8AllowingNoncharacters(host) ||
      !base::IsStringUTF8AllowingNoncharacters(path) ||
      name.size() + value.size() > 4096 || path.empty() || path[0] != '/' ||
      creation.is_null() || same_site < -1 || same_site > 2 ||
      (same_site == 0 && !secure) || priority < 0 || priority > 2 ||
      source_scheme < 0 || source_scheme > 2 || source_port < -1 ||
      source_port > 65535) {
    return nullptr;
  }
  const GURL url = net::cookie_util::CookieDomainAndPathToURL(
      host, path, secure || source_scheme == 2);
  if (!url.is_valid() || !url.SchemeIsHTTPOrHTTPS()) {
    return nullptr;
  }
  auto cookie = net::CanonicalCookie::CreateSanitizedCookie(
      url, name, value, host[0] == '.' ? host : std::string(), path, creation,
      expiration, access, secure, http_only,
      static_cast<net::CookieSameSite>(same_site),
      static_cast<net::CookiePriority>(priority), std::nullopt, nullptr);
  if (!cookie || cookie->Domain() != host || cookie->Path() != path ||
      cookie->ExpiryDate() != expiration) {
    return nullptr;
  }
  cookie->SetSourceScheme(static_cast<net::CookieSourceScheme>(source_scheme));
  cookie->SetSourcePort(source_port);
  if (!cookie->IsCanonical()) {
    return nullptr;
  }
  return cookie;
}
bool Append(CookieImportBatch& batch,
            size_t& bytes,
            std::unique_ptr<net::CanonicalCookie> cookie) {
  if (!cookie || (!cookie->ExpiryDate().is_null() &&
                  cookie->ExpiryDate() <= base::Time::Now())) {
    ++batch.skipped;
    return true;
  }
  bytes += cookie->Name().size() + cookie->Value().size() +
           cookie->Domain().size() + cookie->Path().size();
  if (batch.cookies.size() >= kMaximumCookies || bytes > kMaximumDecodedBytes) {
    return false;
  }
  batch.cookies.push_back(std::move(*cookie));
  return true;
}
}  // namespace

CookieImportResult ReadChromeCookies(
    const base::FilePath& profile,
    SourcePasswordKeyLookup source_key,
    base::RepeatingCallback<bool()> cancelled) {
  const auto modern = profile.AppendASCII("Network").AppendASCII("Cookies");
  const auto source =
      base::PathExists(modern) ? modern : profile.AppendASCII("Cookies");
  base::ScopedTempDir temporary;
  if (!temporary.CreateUniqueTempDir() ||
      !SnapshotBrowserDatabase(source,
                               temporary.GetPath().AppendASCII("Cookies"))) {
    return base::unexpected(CookieImportError::kReadFailed);
  }
  sql::Database db(sql::Database::Tag("Cookie"));
  if (!db.Open(temporary.GetPath().AppendASCII("Cookies")) ||
      !db.DoesTableExist("cookies") || !db.DoesTableExist("meta") ||
      !HasColumns(
          db, "cookies",
          {"creation_utc", "host_key", "name", "value", "path", "expires_utc",
           "is_secure", "is_httponly", "last_access_utc", "encrypted_value"})) {
    return base::unexpected(CookieImportError::kReadFailed);
  }
  sql::Statement version(
      db.GetUniqueStatement("SELECT value FROM meta WHERE key='version'"));
  if (!version.Step()) {
    return base::unexpected(CookieImportError::kUnsupportedSchema);
  }
  const int schema = version.ColumnInt(0);
  if (schema < 10 || schema > 24 ||
      version.ColumnString(0) != base::NumberToString(schema)) {
    return base::unexpected(CookieImportError::kUnsupportedSchema);
  }
  std::string query =
      "SELECT creation_utc,host_key,name,value,path,expires_utc,is_secure,"
      "is_httponly,last_access_utc,encrypted_value," +
      Column(db, "cookies", "samesite", "-1") + "," +
      Column(db, "cookies", "priority", "1") + "," +
      Column(db, "cookies", "top_frame_site_key", "''") + "," +
      Column(db, "cookies", "source_scheme", "0") + "," +
      Column(db, "cookies", "source_port", "-1") + "," +
      Column(db, "cookies", "has_expires", "1") + " FROM cookies";
  sql::Statement rows(db.GetUniqueStatement(query));
  if (!rows.is_valid()) {
    return base::unexpected(CookieImportError::kReadFailed);
  }
  CookieImportBatch result;
  size_t count = 0, bytes = 0;
  os_crypt_async::Encryptor::Key legacy_key(
      kV10Key, os_crypt_async::mojom::Algorithm::kAES128CBC);
  std::optional<os_crypt_async::Encryptor::Key> system_key;
  bool key_requested = false;
  while (rows.Step()) {
    if (cancelled.Run()) {
      return base::unexpected(CookieImportError::kCancelled);
    }
    if (++count > kMaximumRows) {
      return base::unexpected(CookieImportError::kTooLarge);
    }
    if (!TextColumns(rows, {1, 2, 3, 4, 12}) ||
        !IntegerColumns(rows, {0, 5, 6, 7, 8, 10, 11, 13, 14, 15}) ||
        !BooleanColumn(rows, 6) || !BooleanColumn(rows, 7) ||
        !BooleanColumn(rows, 15)) {
      ++result.skipped;
      continue;
    }
    if (!rows.ColumnString(12).empty()) {
      ++result.skipped;
      continue;
    }
    const std::string host = rows.ColumnString(1);
    std::string value = rows.ColumnString(3);
    const auto cipher = rows.ColumnBlob(9);
    if (cipher.size() > 8192) {
      return base::unexpected(CookieImportError::kTooLarge);
    }
    if (!cipher.empty()) {
      if (cipher.size() < 3) {
        return base::unexpected(CookieImportError::kUnsupportedEncryption);
      }
      const std::string_view tag(reinterpret_cast<const char*>(cipher.data()),
                                 3);
      const os_crypt_async::Encryptor::Key* key = nullptr;
      if (tag == "v10") {
        key = &legacy_key;
      } else if (tag == "v11") {
        if (!key_requested) {
          key_requested = true;
          if (source_key) {
            system_key = std::move(source_key).Run();
          }
        }
        if (cancelled.Run()) {
          return base::unexpected(CookieImportError::kCancelled);
        }
        if (!system_key) {
          return base::unexpected(CookieImportError::kUnlockFailed);
        }
        key = &*system_key;
      } else {
        return base::unexpected(CookieImportError::kUnsupportedEncryption);
      }
      auto plaintext = key->DecryptForImport(cipher.subspan(3u));
      if (!plaintext) {
        return base::unexpected(CookieImportError::kDecryptFailed);
      }
      size_t offset = 0;
      if (schema >= 24) {
        const auto digest = crypto::hash::Sha256(host);
        if (plaintext->size() < digest.size() ||
            !std::equal(digest.begin(), digest.end(), plaintext->begin())) {
          crypto::SecureZeroBuffer(*plaintext);
          return base::unexpected(CookieImportError::kDecryptFailed);
        }
        offset = digest.size();
      }
      value.assign(reinterpret_cast<const char*>(plaintext->data()) + offset,
                   plaintext->size() - offset);
      crypto::SecureZeroBuffer(*plaintext);
    }
    const auto creation = WindowsTime(rows.ColumnInt64(0));
    const auto expiration =
        WindowsTime(rows.ColumnBool(15) ? rows.ColumnInt64(5) : 0);
    const auto access = WindowsTime(rows.ColumnInt64(8));
    std::unique_ptr<net::CanonicalCookie> cookie;
    if (creation && expiration && access) {
      cookie =
          MakeCookie(host, rows.ColumnString(2), value, rows.ColumnString(4),
                     *creation, *expiration, *access, rows.ColumnBool(6),
                     rows.ColumnBool(7), rows.ColumnInt(10), rows.ColumnInt(11),
                     rows.ColumnInt(13), rows.ColumnInt(14));
    }
    crypto::SecureZeroBuffer(base::as_writable_byte_span(value));
    if (!Append(result, bytes, std::move(cookie))) {
      return base::unexpected(CookieImportError::kTooLarge);
    }
  }
  if (!rows.Succeeded()) {
    return base::unexpected(CookieImportError::kReadFailed);
  }
  return result;
}

CookieImportResult ReadFirefoxCookies(
    const base::FilePath& profile,
    base::RepeatingCallback<bool()> cancelled) {
  base::ScopedTempDir temporary;
  if (!temporary.CreateUniqueTempDir() ||
      !SnapshotBrowserDatabase(
          profile.AppendASCII("cookies.sqlite"),
          temporary.GetPath().AppendASCII("cookies.sqlite"))) {
    return base::unexpected(CookieImportError::kReadFailed);
  }
  sql::Database db(sql::Database::Tag("FirefoxImporter"));
  if (!db.Open(temporary.GetPath().AppendASCII("cookies.sqlite")) ||
      !db.DoesTableExist("moz_cookies") ||
      !HasColumns(db, "moz_cookies",
                  {"host", "name", "value", "path", "expiry", "creationTime",
                   "lastAccessed", "isSecure", "isHttpOnly", "sameSite",
                   "originAttributes"})) {
    return base::unexpected(CookieImportError::kReadFailed);
  }
  sql::Statement version(db.GetUniqueStatement("PRAGMA user_version"));
  if (!version.Step()) {
    return base::unexpected(CookieImportError::kUnsupportedSchema);
  }
  const int schema = version.ColumnInt(0);
  if (schema != 15 && schema != 16) {
    return base::unexpected(CookieImportError::kUnsupportedSchema);
  }
  std::string query =
      "SELECT host,name,value,path,expiry,creationTime,lastAccessed,"
      "isSecure,isHttpOnly,sameSite,originAttributes," +
      Column(db, "moz_cookies", "isPartitionedAttributeSet", "0") +
      " FROM moz_cookies";
  sql::Statement rows(db.GetUniqueStatement(query));
  if (!rows.is_valid()) {
    return base::unexpected(CookieImportError::kReadFailed);
  }
  CookieImportBatch result;
  size_t count = 0, bytes = 0;
  while (rows.Step()) {
    if (cancelled.Run()) {
      return base::unexpected(CookieImportError::kCancelled);
    }
    if (++count > kMaximumRows) {
      return base::unexpected(CookieImportError::kTooLarge);
    }
    if (!TextColumns(rows, {0, 1, 2, 3, 10}) ||
        !IntegerColumns(rows, {4, 5, 6, 7, 8, 9, 11}) ||
        !BooleanColumn(rows, 7) || !BooleanColumn(rows, 8) ||
        !BooleanColumn(rows, 11)) {
      ++result.skipped;
      continue;
    }
    if (!rows.ColumnString(10).empty() || rows.ColumnBool(11)) {
      ++result.skipped;
      continue;
    }
    const auto creation = UnixTime(rows.ColumnInt64(5), 1);
    const auto expiration =
        UnixTime(rows.ColumnInt64(4), schema == 16 ? 1000 : 1000000);
    const auto access = UnixTime(rows.ColumnInt64(6), 1);
    const int same_site = rows.ColumnInt(9) == 256 ? -1 : rows.ColumnInt(9);
    std::unique_ptr<net::CanonicalCookie> cookie;
    if (creation && expiration && access) {
      cookie = MakeCookie(rows.ColumnString(0), rows.ColumnString(1),
                          rows.ColumnString(2), rows.ColumnString(3), *creation,
                          *expiration, *access, rows.ColumnBool(7),
                          rows.ColumnBool(8), same_site, 1,
                          rows.ColumnBool(7) ? 2 : 1, -1);
    }
    if (!Append(result, bytes, std::move(cookie))) {
      return base::unexpected(CookieImportError::kTooLarge);
    }
  }
  if (!rows.Succeeded()) {
    return base::unexpected(CookieImportError::kReadFailed);
  }
  return result;
}

const char* CookieImportErrorMessage(CookieImportError error) {
  switch (error) {
    case CookieImportError::kReadFailed:
      return "Cookies could not be read safely. Close the source browser and "
             "try again.";
    case CookieImportError::kUnsupportedSchema:
      return "This browser's cookie format is not supported yet.";
    case CookieImportError::kUnlockFailed:
      return "Unlock the source browser's credential store and try cookie "
             "import again.";
    case CookieImportError::kUnsupportedEncryption:
      return "The source browser uses unsupported cookie protection. Sign in "
             "to those sites in Noventra.";
    case CookieImportError::kDecryptFailed:
      return "The source browser's cookies could not be authenticated. No "
             "cookies were imported.";
    case CookieImportError::kCancelled:
      return "Cookie import was cancelled. Some cookies may already have been "
             "copied.";
    case CookieImportError::kTooLarge:
      return "The source cookie database is too large to import safely.";
  }
}
}  // namespace noventra
