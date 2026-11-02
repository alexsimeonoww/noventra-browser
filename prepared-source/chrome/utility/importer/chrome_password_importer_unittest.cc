// Copyright 2026 Noventra contributors
// SPDX-License-Identifier: GPL-3.0-only

#include "chrome/utility/importer/chrome_password_importer.h"

#include <array>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "base/containers/span.h"
#include "base/files/file_util.h"
#include "base/files/scoped_temp_dir.h"
#include "base/functional/bind.h"
#include "base/scoped_environment_variable_override.h"
#include "base/test/task_environment.h"
#include "base/test/test_future.h"
#include "base/threading/thread.h"
#include "components/dbus/thread_linux/dbus_thread_linux.h"
#include "components/os_crypt/async/common/algorithm.mojom.h"
#include "crypto/aes_cbc.h"
#include "crypto/hash.h"
#include "dbus/bus.h"
#include "sql/database.h"
#include "sql/statement.h"
#include "sql/test/test_helpers.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace noventra {
namespace {

constexpr auto kLegacyKey =
    std::to_array<uint8_t>({0xfd, 0x62, 0x1f, 0xe5, 0xa2, 0xb4, 0x02, 0x53,
                            0x9d, 0xfa, 0x14, 0x7c, 0xa9, 0x27, 0x27, 0x78});
constexpr auto kSourceKey =
    std::to_array<uint8_t>({0x31, 0x32, 0x33, 0x34, 0x35, 0x36, 0x37, 0x38,
                            0x39, 0x30, 0x31, 0x32, 0x33, 0x34, 0x35, 0x36});
constexpr std::array<uint8_t, 16> kIv{
    ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ',
    ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ',
};

std::vector<uint8_t> Cipher(base::span<const uint8_t> key,
                            std::string_view plaintext,
                            std::string_view tag) {
  auto result =
      crypto::aes_cbc::Encrypt(key, kIv, base::as_byte_span(plaintext));
  result.insert(result.begin(), tag.begin(), tag.end());
  return result;
}

class ChromePasswordImporterTest : public testing::Test {
 protected:
  void SetUp() override {
    ASSERT_TRUE(directory_.CreateUniqueTempDir());
    Open("Login Data");
  }

  void Open(const char* name, bool wal = false) {
    database_ = std::make_unique<sql::Database>(
        sql::DatabaseOptions().set_wal_mode(wal).set_exclusive_locking(false),
        sql::test::kTestTag);
    ASSERT_TRUE(database_->Open(directory_.GetPath().AppendASCII(name)));
    ASSERT_TRUE(database_->Execute(
        "CREATE TABLE logins (origin_url TEXT, action_url TEXT, "
        "username_element TEXT, username_value TEXT, password_element TEXT, "
        "password_value BLOB, signon_realm TEXT, scheme INTEGER, "
        "blacklisted_by_user INTEGER, federation_url TEXT)"));
  }

  void Add(base::span<const uint8_t> cipher,
           const char* username = "owned-user",
           bool blocked = false,
           const char* federation = "") {
    sql::Statement statement(database_->GetUniqueStatement(
        "INSERT INTO logins VALUES ('https://example.invalid/login', "
        "'https://example.invalid/submit','user',?,'pass',?, "
        "'https://example.invalid/',0,?,?)"));
    statement.BindString(0, username);
    statement.BindBlob(1, cipher);
    statement.BindBool(2, blocked);
    statement.BindString(3, federation);
    ASSERT_TRUE(statement.Run());
  }

  PasswordImportResult Read(bool unlocked = true) {
    return ReadChromePasswords(
        directory_.GetPath(),
        base::BindOnce(
            [](int* requests,
               bool unlocked) -> std::optional<os_crypt_async::Encryptor::Key> {
              ++*requests;
              if (!unlocked) {
                return std::nullopt;
              }
              return os_crypt_async::Encryptor::Key(
                  kSourceKey, os_crypt_async::mojom::Algorithm::kAES128CBC);
            },
            &key_requests_, unlocked),
        base::BindRepeating([](bool* cancelled) { return *cancelled; },
                            &cancelled_));
  }

  auto Digest(const char* name) {
    std::string bytes;
    EXPECT_TRUE(
        base::ReadFileToString(directory_.GetPath().AppendASCII(name), &bytes));
    return crypto::hash::Sha256(bytes);
  }

  base::test::TaskEnvironment task_environment_;
  base::ScopedTempDir directory_;
  std::unique_ptr<sql::Database> database_;
  int key_requests_ = 0;
  bool cancelled_ = false;
};

TEST_F(ChromePasswordImporterTest, LegacyImportDoesNotRequestSystemKey) {
  Add(Cipher(kLegacyKey, "owned-legacy-password", "v10"));
  const auto before = Digest("Login Data");
  auto result = Read(false);
  ASSERT_TRUE(result.has_value());
  ASSERT_EQ(result->size(), 1u);
  EXPECT_EQ(result->front().password_value, u"owned-legacy-password");
  EXPECT_EQ(result->front().username_value, u"owned-user");
  EXPECT_EQ(key_requests_, 0);
  EXPECT_EQ(Digest("Login Data"), before);
}

TEST_F(ChromePasswordImporterTest, SystemKeyAndUnicodeArePreserved) {
  Add(Cipher(kSourceKey, "owned-密碼-Café", "v11"));
  auto result = Read();
  ASSERT_TRUE(result.has_value());
  ASSERT_EQ(result->size(), 1u);
  EXPECT_EQ(result->front().password_value, u"owned-密碼-Café");
  EXPECT_EQ(key_requests_, 1);
}

TEST_F(ChromePasswordImporterTest, AccountStoreUsesSameSourceKeyOnce) {
  Add(Cipher(kSourceKey, "owned-profile-password", "v11"));
  database_.reset();
  Open("Login Data For Account");
  Add(Cipher(kSourceKey, "owned-account-password", "v11"), "owned-account");
  auto result = Read();
  ASSERT_TRUE(result.has_value());
  ASSERT_EQ(result->size(), 2u);
  EXPECT_EQ(result->at(0).password_value, u"owned-profile-password");
  EXPECT_EQ(result->at(1).password_value, u"owned-account-password");
  EXPECT_EQ(key_requests_, 1);
}

TEST_F(ChromePasswordImporterTest, WalRowsAreCopiedWithoutChangingSource) {
  database_.reset();
  ASSERT_TRUE(base::DeleteFile(directory_.GetPath().AppendASCII("Login Data")));
  Open("Login Data", true);
  Add(Cipher(kSourceKey, "owned-wal-password", "v11"));
  ASSERT_TRUE(
      base::PathExists(directory_.GetPath().AppendASCII("Login Data-wal")));
  const auto main_before = Digest("Login Data");
  const auto wal_before = Digest("Login Data-wal");
  auto result = Read();
  ASSERT_TRUE(result.has_value());
  ASSERT_EQ(result->size(), 1u);
  EXPECT_EQ(result->front().password_value, u"owned-wal-password");
  EXPECT_EQ(Digest("Login Data"), main_before);
  EXPECT_EQ(Digest("Login Data-wal"), wal_before);
}

TEST_F(ChromePasswordImporterTest, MissingSystemKeyFailsWithoutFallback) {
  Add(Cipher(kSourceKey, "owned-system-password", "v11"));
  auto result = Read(false);
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error(), PasswordImportError::kUnlockFailed);
}

TEST_F(ChromePasswordImporterTest, UnknownVersionRejectsWholeResult) {
  Add(Cipher(kLegacyKey, "owned-valid-password", "v10"));
  Add(Cipher(kSourceKey, "owned-new-version", "v20"), "other-owned-user");
  auto result = Read();
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error(), PasswordImportError::kUnsupportedEncryption);
}

TEST_F(ChromePasswordImporterTest, PlaintextNeverTreatedAsEncryptedPassword) {
  Add(base::as_byte_span(std::string_view("owned plaintext password")));
  auto result = Read();
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error(), PasswordImportError::kUnsupportedEncryption);
}

TEST_F(ChromePasswordImporterTest, WrongAndEmptyKeysAreNotRetried) {
  constexpr auto empty_key =
      std::to_array<uint8_t>({0xd0, 0xd0, 0xec, 0x9c, 0x7d, 0x77, 0xd4, 0x3a,
                              0xc5, 0x41, 0x87, 0xfa, 0x48, 0x18, 0xd1, 0x7f});
  Add(Cipher(empty_key, "owned-empty-key-fallback", "v11"));
  auto result = Read();
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error(), PasswordImportError::kDecryptFailed);
}

TEST_F(ChromePasswordImporterTest,
       BlockedSitesPreservedFederatedEntriesSkipped) {
  Add({}, "", true);
  Add({}, "federated-user", false, "https://identity.invalid");
  auto result = Read(false);
  ASSERT_TRUE(result.has_value());
  ASSERT_EQ(result->size(), 1u);
  EXPECT_TRUE(result->front().blocked_by_user);
  EXPECT_EQ(key_requests_, 0);
}

TEST_F(ChromePasswordImporterTest,
       EmptyDatabaseIsNotReportedAsImportedPasswords) {
  auto result = Read();
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error(), PasswordImportError::kNoPasswords);
}

TEST_F(ChromePasswordImporterTest, MalformedSchemaFails) {
  ASSERT_TRUE(database_->Execute(
      "ALTER TABLE logins RENAME COLUMN password_value TO missing_password"));
  auto result = Read();
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error(), PasswordImportError::kReadFailed);
}

TEST_F(ChromePasswordImporterTest, CancellationStopsBeforeReadingKeys) {
  Add(Cipher(kSourceKey, "owned-system-password", "v11"));
  cancelled_ = true;
  auto result = Read();
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error(), PasswordImportError::kCancelled);
  EXPECT_EQ(key_requests_, 0);
}

TEST_F(ChromePasswordImporterTest, RepeatedWorkersDoNotBorrowUiSessionBus) {
  // An unconnectable address in our private directory guarantees this test
  // cannot contact the user's session bus or read an actual source key.
  base::ScopedEnvironmentVariableOverride session_address(
      "DBUS_SESSION_BUS_ADDRESS",
      "unix:path=" + directory_.GetPath().AppendASCII("missing-bus").value());
  auto ui_bus = dbus_thread_linux::GetSharedSessionBus();
  ASSERT_TRUE(ui_bus);
  for (int run = 0; run != 2; ++run) {
    base::Thread worker("Owned password import worker");
    ASSERT_TRUE(worker.Start());
    base::test::TestFuture<std::optional<os_crypt_async::Encryptor::Key>>
        future;
    worker.task_runner()->PostTaskAndReplyWithResult(
        FROM_HERE,
        base::BindOnce(&ReadChromePasswordKey, std::u16string(u"Chromium"),
                       base::BindRepeating([] { return false; })),
        future.GetCallback());
    EXPECT_FALSE(future.Take().has_value());
    worker.Stop();
  }
  dbus_thread_linux::ShutdownOnDBusThreadAndBlock();
}

}  // namespace
}  // namespace noventra
