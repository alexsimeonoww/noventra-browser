// Copyright 2026 Noventra contributors
// SPDX-License-Identifier: GPL-3.0-only

#include "chrome/browser/importer/noventra_password_profile_writer.h"

#include <memory>
#include <optional>
#include <utility>

#include "base/run_loop.h"
#include "base/test/bind.h"
#include "base/test/scoped_feature_list.h"
#include "base/test/test_future.h"
#include "base/values.h"
#include "chrome/browser/password_manager/password_manager_test_util.h"
#include "chrome/test/base/testing_profile.h"
#include "components/password_manager/core/browser/features/password_features.h"
#include "components/password_manager/core/browser/password_form.h"
#include "components/password_manager/core/browser/password_store/password_form_converters.h"
#include "components/password_manager/core/browser/password_store/password_store_backend_error.h"
#include "components/password_manager/core/browser/password_store/test_password_store.h"
#include "components/password_manager/core/browser/password_string.h"
#include "components/password_manager/core/common/password_manager_pref_names.h"
#include "components/prefs/pref_service.h"
#include "components/sync_preferences/testing_pref_service_syncable.h"
#include "content/public/test/browser_task_environment.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace noventra {
namespace {

password_manager::PasswordForm Form() {
  password_manager::PasswordForm form;
  form.url = GURL("https://example.invalid/login");
  form.signon_realm = "https://example.invalid/";
  form.username_element = u"user";
  form.username_value = u"owned-user";
  form.password_element = u"pass";
  form.password_value = password_manager::PasswordString(u"owned-existing");
  return form;
}

class PasswordProfileWriterTest : public testing::Test {
 protected:
  void SetUp() override {
    features_.InitWithFeatures(
        {}, {password_manager::features::kSkipUndecryptablePasswords,
             password_manager::features::kClearUndecryptablePasswords});
    profile_ = std::make_unique<TestingProfile>();
    store_ = CreateAndUseTestPasswordStore(profile_.get());
    task_environment_.RunUntilIdle();
  }

  scoped_refptr<ProfileWriter> Prepare() {
    base::test::TestFuture<PreparedPasswordProfileWriter> future;
    auto request =
        PreparePasswordProfileWriter(profile_.get(), future.GetCallback());
    EXPECT_FALSE(future.IsReady());
    auto result = future.Take();
    EXPECT_TRUE(result.has_value());
    return result.has_value() ? std::move(result.value()) : nullptr;
  }

  void Seed(const password_manager::PasswordForm& form) {
    store_->AddLogin(password_manager::FromPasswordForm(form));
    task_environment_.RunUntilIdle();
  }

  auto Credentials() {
    task_environment_.RunUntilIdle();
    return password_manager::GetAllLoginsSync(store_.get());
  }

  base::test::ScopedFeatureList features_;
  content::BrowserTaskEnvironment task_environment_;
  std::unique_ptr<TestingProfile> profile_;
  scoped_refptr<password_manager::TestPasswordStore> store_;
};

TEST_F(PasswordProfileWriterTest, ExistingAccountIsKeptDespiteDifferentFields) {
  const auto existing = Form();
  Seed(existing);
  auto writer = Prepare();
  ASSERT_TRUE(writer);
  auto incoming = existing;
  incoming.url = GURL("https://example.invalid/other-login");
  incoming.username_element = u"different-user-field";
  incoming.password_element = u"different-password-field";
  incoming.password_value = password_manager::PasswordString(u"owned-new");
  writer->AddPasswordForm(incoming);
  auto credentials = Credentials();
  ASSERT_EQ(credentials.at(existing.signon_realm).size(), 1u);
  const auto& kept = credentials.at(existing.signon_realm).front();
  EXPECT_EQ(kept.password_value.value(), u"owned-existing");
  EXPECT_EQ(kept.url, existing.url);
  EXPECT_EQ(kept.username_element, existing.username_element);
}

TEST_F(PasswordProfileWriterTest, SourceDuplicatesKeepFirstPassword) {
  auto writer = Prepare();
  ASSERT_TRUE(writer);
  auto first = Form();
  writer->AddPasswordForm(first);
  auto second = first;
  second.username_element = u"another-form";
  second.password_value = password_manager::PasswordString(u"owned-later");
  writer->AddPasswordForm(second);
  auto credentials = Credentials();
  ASSERT_EQ(credentials.at(first.signon_realm).size(), 1u);
  EXPECT_EQ(credentials.at(first.signon_realm).front().password_value.value(),
            u"owned-existing");
}

TEST_F(PasswordProfileWriterTest, SavingPromptsStayDisabledDuringManualImport) {
  profile_->GetPrefs()->SetBoolean(
      password_manager::prefs::kCredentialsEnableService, false);
  auto writer = Prepare();
  ASSERT_TRUE(writer);
  const auto incoming = Form();
  writer->AddPasswordForm(incoming);
  auto credentials = Credentials();
  ASSERT_EQ(credentials.at(incoming.signon_realm).size(), 1u);
  EXPECT_EQ(credentials.at(incoming.signon_realm).front().password_value.value(),
            incoming.password_value.value());
  EXPECT_FALSE(profile_->GetPrefs()->GetBoolean(
      password_manager::prefs::kCredentialsEnableService));
}

TEST_F(PasswordProfileWriterTest, EnforcedDisabledManagerRejectsImport) {
  profile_->GetTestingPrefService()->SetManagedPref(
      password_manager::prefs::kCredentialsEnableService, base::Value(false));
  base::test::TestFuture<PreparedPasswordProfileWriter> future;
  auto request =
      PreparePasswordProfileWriter(profile_.get(), future.GetCallback());
  EXPECT_FALSE(future.Take().has_value());
  EXPECT_TRUE(Credentials().empty());
}

TEST_F(PasswordProfileWriterTest, EnforcedDisableAfterPreparationPreventsWrite) {
  auto writer = Prepare();
  ASSERT_TRUE(writer);
  profile_->GetTestingPrefService()->SetManagedPref(
      password_manager::prefs::kCredentialsEnableService, base::Value(false));
  writer->AddPasswordForm(Form());
  EXPECT_TRUE(Credentials().empty());
}

TEST_F(PasswordProfileWriterTest, BlockedEntriesAreNotReplaced) {
  auto blocked = Form();
  blocked.blocked_by_user = true;
  blocked.username_value.clear();
  blocked.password_value = password_manager::PasswordString();
  Seed(blocked);
  auto writer = Prepare();
  ASSERT_TRUE(writer);
  auto incoming = Form();
  incoming.username_value = u"a-nonempty-new-account";
  incoming.password_value = password_manager::PasswordString(u"owned-new");
  writer->AddPasswordForm(incoming);
  auto credentials = Credentials();
  ASSERT_EQ(credentials.at(blocked.signon_realm).size(), 1u);
  EXPECT_TRUE(credentials.at(blocked.signon_realm).front().blocked_by_user);
}

TEST_F(PasswordProfileWriterTest, NewlyObservedBlockedRealmIsReserved) {
  auto writer = Prepare();
  ASSERT_TRUE(writer);
  auto blocked = Form();
  blocked.blocked_by_user = true;
  blocked.username_value.clear();
  blocked.password_value = password_manager::PasswordString();
  Seed(blocked);
  auto incoming = Form();
  incoming.username_value = u"nonempty-imported-account";
  writer->AddPasswordForm(incoming);
  auto credentials = Credentials();
  ASSERT_EQ(credentials.at(blocked.signon_realm).size(), 1u);
  EXPECT_TRUE(credentials.at(blocked.signon_realm).front().blocked_by_user);
}

TEST_F(PasswordProfileWriterTest, ObservedNewAccountIsReserved) {
  auto writer = Prepare();
  ASSERT_TRUE(writer);
  const auto existing = Form();
  Seed(existing);
  auto incoming = existing;
  incoming.password_value = password_manager::PasswordString(u"owned-new");
  writer->AddPasswordForm(incoming);
  auto credentials = Credentials();
  ASSERT_EQ(credentials.at(existing.signon_realm).size(), 1u);
  EXPECT_EQ(
      credentials.at(existing.signon_realm).front().password_value.value(),
      u"owned-existing");
}

TEST_F(PasswordProfileWriterTest, BackendErrorDoesNotPrepareAWriter) {
  Seed(Form());
  store_->ReturnErrorOnRequest(password_manager::PasswordStoreBackendError(
      password_manager::PasswordStoreBackendErrorType::kUncategorized));
  base::test::TestFuture<PreparedPasswordProfileWriter> future;
  auto request =
      PreparePasswordProfileWriter(profile_.get(), future.GetCallback());
  auto result = future.Take();
  EXPECT_FALSE(result.has_value());
  store_->ReturnErrorOnRequest(std::nullopt);
  auto credentials = Credentials();
  ASSERT_EQ(credentials.at(Form().signon_realm).size(), 1u);
  EXPECT_EQ(credentials.at(Form().signon_realm).front().password_value.value(),
            u"owned-existing");
}

TEST_F(PasswordProfileWriterTest, PartialResultModeRefusesPreparation) {
  features_.Reset();
  features_.InitAndEnableFeature(
      password_manager::features::kSkipUndecryptablePasswords);
  base::test::TestFuture<PreparedPasswordProfileWriter> future;
  auto request =
      PreparePasswordProfileWriter(profile_.get(), future.GetCallback());
  EXPECT_FALSE(future.Take().has_value());
}

TEST_F(PasswordProfileWriterTest, DeletionDuringInspectionModeIsRefused) {
  features_.Reset();
  features_.InitAndEnableFeature(
      password_manager::features::kClearUndecryptablePasswords);
  base::test::TestFuture<PreparedPasswordProfileWriter> future;
  auto request =
      PreparePasswordProfileWriter(profile_.get(), future.GetCallback());
  EXPECT_FALSE(future.Take().has_value());
}

TEST_F(PasswordProfileWriterTest, DestroyingPendingRequestCancelsCallback) {
  bool called = false;
  auto request = PreparePasswordProfileWriter(
      profile_.get(),
      base::BindLambdaForTesting(
          [&](PreparedPasswordProfileWriter) { called = true; }));
  request.reset();
  task_environment_.RunUntilIdle();
  EXPECT_FALSE(called);
}

TEST_F(PasswordProfileWriterTest, CallbackMayDestroyItsOwnRequest) {
  bool called = false;
  std::unique_ptr<PasswordProfileWriterPreparation> request;
  request = PreparePasswordProfileWriter(
      profile_.get(),
      base::BindLambdaForTesting([&](PreparedPasswordProfileWriter result) {
        request.reset();
        EXPECT_TRUE(result.has_value());
        called = true;
      }));
  task_environment_.RunUntilIdle();
  EXPECT_TRUE(called);
  EXPECT_FALSE(request);
}

TEST_F(PasswordProfileWriterTest, DestroyedProfileFailsWithoutDereference) {
  base::test::TestFuture<PreparedPasswordProfileWriter> future;
  auto request =
      PreparePasswordProfileWriter(profile_.get(), future.GetCallback());
  profile_.reset();
  EXPECT_FALSE(future.Take().has_value());
}

}  // namespace
}  // namespace noventra
