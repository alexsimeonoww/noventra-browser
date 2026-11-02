// Copyright 2026 Noventra contributors
// SPDX-License-Identifier: GPL-3.0-only

#include "chrome/utility/importer/chrome_password_importer.h"

#include <array>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "base/containers/span.h"
#include "base/files/file_util.h"
#include "base/files/scoped_temp_dir.h"
#include "base/functional/bind.h"
#include "base/message_loop/message_pump_type.h"
#include "base/run_loop.h"
#include "base/strings/string_util.h"
#include "base/strings/utf_string_conversions.h"
#include "base/threading/thread.h"
#include "base/time/time.h"
#include "base/timer/timer.h"
#include "components/os_crypt/async/browser/freedesktop_secret_key_provider.h"
#include "components/os_crypt/async/common/algorithm.mojom.h"
#include "crypto/hash.h"
#include "crypto/secure_util.h"
#include "dbus/bus.h"
#include "sql/database.h"
#include "sql/statement.h"

namespace noventra {
namespace {

constexpr size_t kMaximumDatabaseBytes = 64 * 1024 * 1024;
constexpr size_t kMaximumPasswordBytes = 1024 * 1024;
constexpr size_t kMaximumPasswords = 100000;
constexpr auto kV10Key =
    std::to_array<uint8_t>({0xfd, 0x62, 0x1f, 0xe5, 0xa2, 0xb4, 0x02, 0x53,
                            0x9d, 0xfa, 0x14, 0x7c, 0xa9, 0x27, 0x27, 0x78});

// Copy a coherent main/WAL/journal set without opening the original in SQLite,
// which could otherwise create or modify its shared-memory sidecar. A source
// that changes while being copied is rejected rather than partially imported.
bool SnapshotDatabase(const base::FilePath& source,
                      const base::FilePath& destination) {
  struct Copy {
    base::FilePath source;
    std::optional<std::array<uint8_t, crypto::hash::kSha256Size>> digest;
  };
  std::vector<Copy> copies;
  for (const char* suffix : {"", "-wal", "-journal"}) {
    const base::FilePath path(source.value() + suffix);
    const bool exists = base::PathExists(path);
    if (suffix[0] == '\0' && !exists) {
      return false;
    }
    Copy copy{path, std::nullopt};
    if (exists) {
      if (base::IsLink(path)) {
        return false;
      }
      std::string bytes;
      if (!base::ReadFileToStringWithMaxSize(path, &bytes,
                                             kMaximumDatabaseBytes)) {
        return false;
      }
      copy.digest = crypto::hash::Sha256(bytes);
      const base::FilePath target(destination.value() + suffix);
      const bool written = base::WriteFile(target, bytes) &&
                           base::SetPosixFilePermissions(target, 0600);
      crypto::SecureZeroBuffer(base::as_writable_byte_span(bytes));
      if (!written) {
        return false;
      }
    }
    copies.push_back(std::move(copy));
  }
  for (const Copy& copy : copies) {
    if (base::PathExists(copy.source) != copy.digest.has_value()) {
      return false;
    }
    if (copy.digest) {
      std::string bytes;
      if (base::IsLink(copy.source) ||
          !base::ReadFileToStringWithMaxSize(copy.source, &bytes,
                                             kMaximumDatabaseBytes)) {
        return false;
      }
      const bool same = crypto::hash::Sha256(bytes) == *copy.digest;
      crypto::SecureZeroBuffer(base::as_writable_byte_span(bytes));
      if (!same) {
        return false;
      }
    }
  }
  return true;
}

}  // namespace

std::optional<os_crypt_async::Encryptor::Key> ReadChromePasswordKey(
    const std::u16string& importer_name,
    base::RepeatingCallback<bool()> cancelled) {
  using Provider = os_crypt_async::FreedesktopSecretKeyProvider;
  std::optional<Provider::ReadOnlyImport> identity;
  if (importer_name == u"Google Chrome" ||
      importer_name == u"Google Chrome Beta" ||
      importer_name == u"Google Chrome Dev") {
    identity = Provider::ReadOnlyImport{"chrome", "Chrome Keys",
                                        "Chrome Safe Storage"};
  } else if (importer_name == u"Chromium" || importer_name == u"Helium") {
    // The pinned Helium source and installed Helium binary use Chromium's
    // non-Google Safe Storage identity; the profile's RDNS directory does not
    // select a different encryption key.
    identity = Provider::ReadOnlyImport{"chromium", "Chromium Keys",
                                        "Chromium Safe Storage"};
  } else if (importer_name == u"Brave" || importer_name == u"Brave Beta" ||
             importer_name == u"Brave Dev" ||
             importer_name == u"Brave Nightly" ||
             importer_name == u"Brave Origin" ||
             importer_name == u"Brave Origin Beta" ||
             importer_name == u"Brave Origin Dev" ||
             importer_name == u"Brave Origin Nightly") {
    // Brave's provider uses this shared key identity for Browser and Origin,
    // independently of their separate profile roots and release channels.
    identity =
        Provider::ReadOnlyImport{"brave", "Brave Keys", "Brave Safe Storage"};
  }
  if (!identity || cancelled.Run()) {
    return std::nullopt;
  }
  // A process-global session bus is UI-origin-bound and may already belong to
  // another importer worker. Use a private, worker-origin connection whose IO
  // thread and remote sessions are closed before this import returns.
  base::Thread dbus_thread("Noventra password import DBus");
  if (!dbus_thread.StartWithOptions(
          base::Thread::Options(base::MessagePumpType::IO, 0))) {
    return std::nullopt;
  }
  dbus::Bus::Options options;
  options.bus_type = dbus::Bus::SESSION;
  options.connection_type = dbus::Bus::PRIVATE;
  options.dbus_task_runner = dbus_thread.task_runner();
  auto bus = base::MakeRefCounted<dbus::Bus>(std::move(options));
  auto result = [&]() -> std::optional<os_crypt_async::Encryptor::Key> {
    Provider provider("", "Noventra password import", bus, std::move(identity));
    std::optional<os_crypt_async::Encryptor::Key> result;
    base::RunLoop run_loop(base::RunLoop::Type::kNestableTasksAllowed);
    provider.GetKey(base::BindOnce(
        [](std::optional<os_crypt_async::Encryptor::Key>* result,
           base::OnceClosure quit, const std::string& tag,
           base::expected<os_crypt_async::Encryptor::Key,
                          os_crypt_async::KeyProvider::KeyError> key) {
          if (tag == "v11" && key.has_value()) {
            *result = std::move(key.value());
          }
          std::move(quit).Run();
        },
        &result, run_loop.QuitClosure()));
    base::OneShotTimer timeout;
    timeout.Start(FROM_HERE, base::Seconds(60), run_loop.QuitClosure());
    base::RepeatingTimer cancellation;
    cancellation.Start(FROM_HERE, base::Milliseconds(100),
                       base::BindRepeating(
                           [](base::RepeatingCallback<bool()> cancelled,
                              base::RepeatingClosure quit) {
                             if (cancelled.Run()) {
                               quit.Run();
                             }
                           },
                           cancelled, run_loop.QuitClosure()));
    run_loop.Run();
    if (cancelled.Run()) {
      return std::nullopt;
    }
    return result;
  }();
  // The provider's weak callbacks and key state are gone before connection
  // shutdown. Pending worker-origin tasks cannot use destroyed stack state.
  bus->ShutdownOnDBusThreadAndBlock();
  bus.reset();
  dbus_thread.Stop();
  return result;
}

PasswordImportResult ReadChromePasswords(
    const base::FilePath& profile,
    SourcePasswordKeyLookup source_key,
    base::RepeatingCallback<bool()> cancelled) {
  std::vector<user_data_importer::ImportedPasswordForm> forms;
  os_crypt_async::Encryptor::Key legacy_key(
      kV10Key, os_crypt_async::mojom::Algorithm::kAES128CBC);
  std::optional<os_crypt_async::Encryptor::Key> system_key;
  bool key_requested = false;
  bool found_database = false;
  for (const char* name : {"Login Data", "Login Data For Account"}) {
    if (cancelled.Run()) {
      return base::unexpected(PasswordImportError::kCancelled);
    }
    const base::FilePath source = profile.AppendASCII(name);
    if (!base::PathExists(source)) {
      continue;
    }
    found_database = true;
    base::ScopedTempDir temporary;
    if (!temporary.CreateUniqueTempDir() ||
        !SnapshotDatabase(source,
                          temporary.GetPath().AppendASCII("Login Data"))) {
      return base::unexpected(PasswordImportError::kReadFailed);
    }
    // All writes, WAL recovery and locking below affect only this private copy.
    sql::Database database(sql::Database::Tag("Passwords"));
    if (!database.Open(temporary.GetPath().AppendASCII("Login Data")) ||
        !database.DoesTableExist("logins")) {
      return base::unexpected(PasswordImportError::kReadFailed);
    }
    std::string query =
        "SELECT origin_url, action_url, username_element, username_value, "
        "password_element, password_value, signon_realm, scheme, "
        "blacklisted_by_user, ";
    query += database.DoesColumnExist("logins", "federation_url")
                 ? "federation_url FROM logins"
                 : "'' FROM logins";
    sql::Statement statement(database.GetUniqueStatement(query));
    if (!statement.is_valid()) {
      return base::unexpected(PasswordImportError::kReadFailed);
    }
    while (statement.Step()) {
      if (cancelled.Run()) {
        return base::unexpected(PasswordImportError::kCancelled);
      }
      const auto cipher = statement.ColumnBlob(5);
      const bool blocked = statement.ColumnBool(8);
      // Federated and username-only entries have no saved browser password.
      if (!statement.ColumnString(9).empty() || (!blocked && cipher.empty())) {
        continue;
      }
      if (forms.size() >= kMaximumPasswords ||
          cipher.size() > kMaximumPasswordBytes) {
        return base::unexpected(PasswordImportError::kReadFailed);
      }
      user_data_importer::ImportedPasswordForm form;
      form.url = GURL(statement.ColumnString(0));
      form.action = GURL(statement.ColumnString(1));
      form.username_element = statement.ColumnString16(2);
      form.username_value = statement.ColumnString16(3);
      form.password_element = statement.ColumnString16(4);
      form.signon_realm = statement.ColumnString(6);
      form.blocked_by_user = blocked;
      const int scheme = statement.ColumnInt(7);
      if (!form.url.is_valid() || form.signon_realm.empty() || scheme < 0 ||
          scheme > 2) {
        return base::unexpected(PasswordImportError::kReadFailed);
      }
      form.scheme =
          scheme == 0
              ? user_data_importer::ImportedPasswordForm::Scheme::kHtml
              : user_data_importer::ImportedPasswordForm::Scheme::kBasic;
      if (!blocked) {
        if (cipher.size() < 3) {
          return base::unexpected(PasswordImportError::kUnsupportedEncryption);
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
            return base::unexpected(PasswordImportError::kCancelled);
          }
          if (!system_key) {
            return base::unexpected(PasswordImportError::kUnlockFailed);
          }
          key = &*system_key;
        } else {
          return base::unexpected(PasswordImportError::kUnsupportedEncryption);
        }
        auto bytes = key->DecryptForImport(cipher.subspan(3u));
        if (!bytes) {
          return base::unexpected(PasswordImportError::kDecryptFailed);
        }
        const std::string_view plaintext(
            reinterpret_cast<const char*>(bytes->data()), bytes->size());
        const bool valid = base::IsStringUTF8AllowingNoncharacters(plaintext);
        if (valid) {
          form.password_value = base::UTF8ToUTF16(plaintext);
        }
        crypto::SecureZeroBuffer(*bytes);
        if (!valid) {
          return base::unexpected(PasswordImportError::kDecryptFailed);
        }
      }
      forms.push_back(std::move(form));
    }
    if (!statement.Succeeded()) {
      return base::unexpected(PasswordImportError::kReadFailed);
    }
  }
  if (!found_database) {
    return base::unexpected(PasswordImportError::kReadFailed);
  }
  if (forms.empty()) {
    return base::unexpected(PasswordImportError::kNoPasswords);
  }
  return forms;
}

const char* PasswordImportErrorMessage(PasswordImportError error) {
  switch (error) {
    case PasswordImportError::kReadFailed:
      return "The browser password database could not be read. Close that "
             "browser, refresh the list and try again. Nothing was copied.";
    case PasswordImportError::kNoPasswords:
      return "No saved passwords were found in this browser profile.";
    case PasswordImportError::kUnlockFailed:
      return "The source browser's existing encryption key could not be "
             "unlocked. Unlock your desktop keyring and try again. Nothing "
             "was copied.";
    case PasswordImportError::kUnsupportedEncryption:
      return "This profile uses password encryption that direct import does "
             "not support. Use a password CSV export instead. Nothing was "
             "copied.";
    case PasswordImportError::kDecryptFailed:
      return "The source browser's saved passwords could not be decrypted. "
             "Nothing was copied. You can use a password CSV export instead.";
    case PasswordImportError::kCancelled:
      return "Password import was cancelled before copying any passwords.";
  }
}

}  // namespace noventra
