// Copyright 2026 The Noventra Authors. All rights reserved.
#include "chrome/browser/ui/noventra/portable_passwords.h"

#include <set>
#include <utility>

#include "base/functional/bind.h"
#include "base/location.h"
#include "base/strings/utf_string_conversions.h"
#include "base/time/time.h"
#include "build/build_config.h"
#include "chrome/browser/affiliations/affiliation_service_factory.h"
#include "chrome/browser/password_manager/factories/account_password_store_factory.h"
#include "chrome/browser/password_manager/factories/profile_password_store_factory.h"
#include "chrome/browser/profiles/profile.h"
#include "components/device_reauth/device_authenticator.h"
#include "components/password_manager/core/browser/export/password_csv_writer.h"
#include "components/password_manager/core/browser/import/import_results.h"
#include "components/password_manager/core/browser/import/password_importer.h"
#include "components/password_manager/core/browser/password_form.h"
#include "components/password_manager/core/browser/password_store/actionable_error.h"
#include "components/password_manager/core/browser/password_store/password_store_interface.h"
#include "components/password_manager/core/browser/ui/credential_ui_entry.h"
#include "components/password_manager/core/browser/ui/credential_utils.h"
#include "components/password_manager/core/browser/ui/saved_passwords_presenter.h"
#include "components/password_manager/core/common/password_manager_constants.h"
#include "components/password_manager/core/common/password_manager_pref_names.h"
#include "components/password_manager/services/csv_password/csv_password_parser_service.h"
#include "components/prefs/pref_service.h"
#include "content/public/browser/web_contents.h"

#if BUILDFLAG(IS_MAC) || BUILDFLAG(IS_WIN) || BUILDFLAG(IS_CHROMEOS)
#include "chrome/browser/device_reauth/chrome_device_authenticator_factory.h"
#include "chrome/grit/generated_resources.h"
#include "ui/base/l10n/l10n_util.h"
#endif

namespace noventra {
namespace {

constexpr size_t kMaximumCSVBytes = 1000 * 1024;
using password_manager::CSVPassword;
using password_manager::ImportResults;
using password_manager::PasswordForm;
using password_manager::PasswordImporter;

bool ValidRow(const CSVPassword& row) {
  using namespace password_manager::constants;
  const auto& url = row.GetURL();
  return row.GetParseStatus() == CSVPassword::Status::kOK && url.has_value() &&
         url->is_valid() && password_manager::IsValidPasswordURL(*url) &&
         url->spec().size() <= kMaxUrlLengthForImport &&
         !row.GetPassword().empty() &&
         row.GetPassword().size() <= kMaxPasswordLengthForImport &&
         row.GetUsername().size() <= kMaxUsernameLengthForImport;
}

}  // namespace

PortablePasswords::PortablePasswords(Profile* profile) : profile_(profile) {
  if (profile_) {
    profile_observation_.Observe(profile_);
  }
}

PortablePasswords::~PortablePasswords() {
  Cancel();
}

// static
bool PortablePasswords::IsAvailable(Profile* profile) {
  if (!profile || !profile->IsRegularProfile() || profile->IsOffTheRecord() ||
      profile->IsGuestSession()) {
    return false;
  }
  auto* prefs = profile->GetPrefs();
  return !(
      prefs->IsManagedPreference(
          password_manager::prefs::kCredentialsEnableService) &&
      !prefs->GetBoolean(password_manager::prefs::kCredentialsEnableService));
}

void PortablePasswords::ExportCSV(content::WebContents* contents,
                                  ExportCallback callback) {
  if (state_ != State::kIdle) {
    std::move(callback).Run(base::unexpected(
        "Finish or cancel the active password transfer first."));
    return;
  }
  if (!IsAvailable(profile_) || !contents ||
      contents->GetBrowserContext() != profile_) {
    std::move(callback).Run(
        base::unexpected("Password transfer is unavailable in this profile."));
    return;
  }
  state_ = State::kExporting;
  export_callback_ = std::move(callback);
  operation_timer_.Start(
      FROM_HERE, base::Minutes(2),
      base::BindOnce(&PortablePasswords::Fail, weak_factory_.GetWeakPtr(),
                     "Password authentication did not finish. Try again."));

#if BUILDFLAG(IS_MAC) || BUILDFLAG(IS_WIN) || BUILDFLAG(IS_CHROMEOS)
  // These are the same platform gate, zero cache duration and purpose as
  // PasswordsPrivateDelegateImpl::ExportPasswords/AuthenticateUser. Do not use
  // a filling authenticator, which can accept a cached authentication instead.
  device_reauth::DeviceAuthParams params(
      base::Seconds(0), device_reauth::DeviceAuthSource::kPasswordManager,
      "PasswordManager.ReauthToAccessPasswordInSettings");
  authenticator_ =
      ChromeDeviceAuthenticatorFactory::GetForProfile(profile_, params);
  if (!authenticator_) {
    Fail(
        "Password authentication is unavailable. Unlock your device and try "
        "again.");
    return;
  }
  std::u16string message;
#if BUILDFLAG(IS_MAC)
  message = l10n_util::GetStringUTF16(
      IDS_PASSWORDS_PAGE_EXPORT_AUTHENTICATION_PROMPT_BIOMETRIC_SUFFIX);
#elif BUILDFLAG(IS_WIN)
  message = l10n_util::GetStringUTF16(
      IDS_PASSWORDS_PAGE_EXPORT_AUTHENTICATION_PROMPT);
#endif
  authenticator_->AuthenticateWithMessage(
      message,
      base::BindOnce(&PortablePasswords::OnAuthenticated,
                     weak_factory_.GetWeakPtr(), contents->GetWeakPtr()));
#else
  // The pinned native password exporter has no OS reauthentication provider on
  // Linux. Keep that existing platform behavior explicit rather than inventing
  // a prompt or changing the password-saving preference.
  OnAuthenticated(contents->GetWeakPtr(), true);
#endif
}

void PortablePasswords::OnAuthenticated(
    base::WeakPtr<content::WebContents> contents,
    bool authenticated) {
  if (!authenticated || !contents || !IsAvailable(profile_) ||
      contents->GetBrowserContext() != profile_) {
    Fail("Password export was canceled or authentication failed.");
    return;
  }
  StartPresenter();
}

void PortablePasswords::PrepareImport(std::string csv, Callback callback) {
  if (state_ != State::kIdle) {
    std::move(callback).Run(base::unexpected(
        "Finish or cancel the active password transfer first."));
    return;
  }
  if (!IsAvailable(profile_) || csv.size() > kMaximumCSVBytes) {
    std::move(callback).Run(base::unexpected(
        "Password transfer is unavailable or exceeds the 1,000 KB limit."));
    return;
  }
  state_ = State::kPreparing;
  callback_ = std::move(callback);
  operation_timer_.Start(
      FROM_HERE, base::Seconds(30),
      base::BindOnce(&PortablePasswords::Fail, weak_factory_.GetWeakPtr(),
                     "The password data could not be checked. Try again."));
  parser_ = password_manager::LaunchCSVPasswordParser();
  parser_.set_disconnect_handler(
      base::BindOnce(&PortablePasswords::Fail, weak_factory_.GetWeakPtr(),
                     "The password data could not be checked. Try again."));
  parser_->ParseCSV(std::move(csv),
                    base::BindOnce(&PortablePasswords::OnCSVParsed,
                                   weak_factory_.GetWeakPtr()));
}

void PortablePasswords::OnCSVParsed(
    password_manager::mojom::CSVPasswordSequencePtr sequence) {
  parser_.reset();
  if (!sequence || sequence->csv_passwords.size() >
                       password_manager::constants::kMaxPasswordsPerCSVFile) {
    Fail(
        "The transfer contains an invalid password export or more than 3,000 "
        "entries.");
    return;
  }
  for (const auto& row : sequence->csv_passwords) {
    if (!ValidRow(row)) {
      Fail(
          "The transfer contains an invalid password entry. No passwords were "
          "imported.");
      return;
    }
    // Notes are deliberately excluded. The general CSV importer can merge
    // notes into existing logins even when no replacement IDs are selected.
    incoming_.emplace_back(row.GetURL().value(), row.GetUsername(),
                           row.GetPassword(), std::string(),
                           CSVPassword::Status::kOK);
  }
  passwords_count_ = incoming_.size();
  StartPresenter();
}

void PortablePasswords::StartPresenter() {
  if (!IsAvailable(profile_)) {
    Fail("Your profile no longer allows password transfer.");
    return;
  }
  importer_.reset();
  presenter_.reset();
  profile_store_ = ProfilePasswordStoreFactory::GetForProfile(
      profile_, ServiceAccessType::EXPLICIT_ACCESS);
  account_store_ = AccountPasswordStoreFactory::GetForProfile(
      profile_, ServiceAccessType::EXPLICIT_ACCESS);
  if (!StoresAvailable()) {
    Fail("Unlock your password store before transferring saved passwords.");
    return;
  }
  operation_timer_.Start(
      FROM_HERE, base::Seconds(30),
      base::BindOnce(&PortablePasswords::Fail, weak_factory_.GetWeakPtr(),
                     "The password store did not finish loading. Try again."));
  presenter_ = std::make_unique<password_manager::SavedPasswordsPresenter>(
      AffiliationServiceFactory::GetForProfile(profile_), profile_store_,
      account_store_);
  presenter_->Init(base::BindOnce(&PortablePasswords::OnPresenterReady,
                                  weak_factory_.GetWeakPtr()));
}

bool PortablePasswords::StoresAvailable() const {
  using password_manager::ActionableError;
  return profile_store_ &&
         profile_store_->GetError() == ActionableError::kNoError &&
         (!account_store_ ||
          account_store_->GetError() == ActionableError::kNoError);
}

void PortablePasswords::OnPresenterReady() {
  if (!IsAvailable(profile_) || !StoresAvailable()) {
    Fail(
        "The password store is locked or unavailable. No password transfer was "
        "completed.");
    return;
  }
  if (state_ != State::kExporting) {
    PrepareAgainstCurrentStore();
    return;
  }
  auto credentials = presenter_->GetSavedPasswords();
  size_t rows = 0;
  for (auto& credential : credentials) {
    rows += credential.GetAffiliatedDomains().size();
    credential.note.clear();
  }
  if (rows > password_manager::constants::kMaxPasswordsPerCSVFile) {
    Fail("Password transfer supports at most 3,000 exported entries.");
    return;
  }
  auto csv =
      password_manager::PasswordCSVWriter::SerializePasswords(credentials);
  if (csv.size() > kMaximumCSVBytes) {
    Fail("The password export exceeds the 1,000 KB transfer limit.");
    return;
  }
  FinishExport(base::ok(std::move(csv)));
}

void PortablePasswords::PrepareAgainstCurrentStore() {
  std::set<std::pair<std::string, std::u16string>> occupied;
  for (const auto& credential : presenter_->GetSavedPasswords()) {
    for (const auto& facet : credential.facets) {
      occupied.emplace(facet.signon_realm, credential.username);
    }
  }
  std::vector<CSVPassword> additions;
  kept_count_ = 0;
  for (const auto& row : incoming_) {
    const password_manager::CredentialUIEntry credential(row);
    if (!occupied.emplace(credential.GetFirstSignonRealm(), credential.username)
             .second) {
      ++kept_count_;
      continue;
    }
    additions.push_back(row);
  }
  prepared_count_ = additions.size();
  importer_ = std::make_unique<PasswordImporter>(
      *presenter_, /*user_confirmation_required=*/true);
  importer_->Import(additions, PasswordForm::Store::kProfileStore,
                    base::BindOnce(&PortablePasswords::OnImportPrepared,
                                   weak_factory_.GetWeakPtr()));
}

void PortablePasswords::OnImportPrepared(const ImportResults& result) {
  if (result.status != ImportResults::CONFLICTS ||
      !importer_->IsState(PasswordImporter::kUserInteractionRequired) ||
      result.number_to_import + result.displayed_entries.size() !=
          prepared_count_) {
    Fail(
        "The password data could not be fully checked. No passwords were "
        "imported.");
    return;
  }
  kept_count_ += result.displayed_entries.size();
  prepared_count_ = result.number_to_import;
  if (state_ == State::kCommitting) {
    // Empty IDs retain all conflicting logins. Never import replacement IDs
    // from the peer or WebUI; replacement requires the general manager's auth.
    importer_->ContinueImport(
        {}, base::BindOnce(&PortablePasswords::OnImportCommitted,
                           weak_factory_.GetWeakPtr()));
    return;
  }
  operation_timer_.Stop();
  state_ = State::kPrepared;
  auto callback = std::move(callback_);
  std::move(callback).Run(
      base::DictValue()
          .Set("passwords", static_cast<int>(passwords_count_))
          .Set("new_passwords", static_cast<int>(prepared_count_))
          .Set("kept_passwords", static_cast<int>(kept_count_)));
}

void PortablePasswords::CommitImport(Callback callback) {
  if (state_ != State::kPrepared || !IsAvailable(profile_)) {
    std::move(callback).Run(
        base::unexpected("Review a password transfer before importing it."));
    return;
  }
  state_ = State::kCommitting;
  callback_ = std::move(callback);
  // A different Settings window or a website may have added credentials after
  // review. Refresh and filter again, preserving those current logins too.
  StartPresenter();
}

void PortablePasswords::OnImportCommitted(const ImportResults& result) {
  if (result.status != ImportResults::SUCCESS || !StoresAvailable() ||
      !result.displayed_entries.empty()) {
    Fail(
        "The password import did not finish completely. Check the password "
        "manager before trying again.");
    return;
  }
  FinishImport(base::DictValue()
                   .Set("imported", static_cast<int>(result.number_imported))
                   .Set("kept", static_cast<int>(kept_count_))
                   .Set("failed", 0));
}

void PortablePasswords::Fail(std::string message) {
  if (export_callback_) {
    FinishExport(base::unexpected(std::move(message)));
  } else if (callback_) {
    FinishImport(base::unexpected(std::move(message)));
  } else {
    Cancel();
  }
}

void PortablePasswords::FinishExport(ExportResult result) {
  auto callback = std::move(export_callback_);
  Cancel();
  if (callback) {
    std::move(callback).Run(std::move(result));
  }
}

void PortablePasswords::FinishImport(Result result) {
  auto callback = std::move(callback_);
  Cancel();
  if (callback) {
    std::move(callback).Run(std::move(result));
  }
}

void PortablePasswords::Cancel() {
  weak_factory_.InvalidateWeakPtrs();
  operation_timer_.Stop();
  export_callback_.Reset();
  callback_.Reset();
#if !BUILDFLAG(IS_WIN)
  if (authenticator_) {
    authenticator_->Cancel();
  }
#endif
  authenticator_.reset();
  parser_.reset();
  importer_.reset();
  presenter_.reset();
  profile_store_.reset();
  account_store_.reset();
  incoming_.clear();
  passwords_count_ = 0;
  kept_count_ = 0;
  prepared_count_ = 0;
  state_ = State::kIdle;
}

void PortablePasswords::OnProfileWillBeDestroyed(Profile* profile) {
  Cancel();
  profile_observation_.Reset();
  profile_ = nullptr;
}

}  // namespace noventra
