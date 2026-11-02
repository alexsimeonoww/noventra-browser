// Copyright 2026 Noventra contributors
// SPDX-License-Identifier: GPL-3.0-only

#include "chrome/browser/importer/noventra_password_profile_writer.h"

#include <set>
#include <string>
#include <utility>
#include <vector>

#include "base/feature_list.h"
#include "base/functional/bind.h"
#include "base/memory/weak_ptr.h"
#include "base/scoped_observation.h"
#include "base/task/sequenced_task_runner.h"
#include "base/time/time.h"
#include "base/timer/timer.h"
#include "chrome/browser/password_manager/factories/profile_password_store_factory.h"
#include "chrome/browser/profiles/profile.h"
#include "components/password_manager/core/browser/features/password_features.h"
#include "components/password_manager/core/browser/password_form.h"
#include "components/password_manager/core/browser/password_store/password_store_change.h"
#include "components/password_manager/core/browser/password_store/password_store_consumer.h"
#include "components/password_manager/core/browser/password_store/password_form_converters.h"
#include "components/password_manager/core/browser/password_store/password_store_interface.h"
#include "components/password_manager/core/browser/password_store/stored_credential.h"
#include "components/password_manager/core/common/password_manager_pref_names.h"
#include "components/prefs/pref_service.h"
#include "content/public/browser/browser_thread.h"

namespace noventra {
namespace {

using Identity = std::pair<std::string, std::u16string>;
using IdentitySet = std::set<Identity>;
using BlockedRealms = std::set<std::string>;
using Store = password_manager::PasswordStoreInterface;
using StoredCredential = password_manager::StoredCredential;

constexpr char kInspectionFailed[] =
    "Your existing passwords could not be checked safely. Unlock Noventra's "
    "password manager and try again. No import was started.";

constexpr char kDisabledByPolicy[] =
    "Importing passwords is disabled by your administrator.";

bool IsPasswordImportDisabledByPolicy(Profile* profile) {
  const auto* prefs = profile->GetPrefs();
  return prefs->IsManagedPreference(
             password_manager::prefs::kCredentialsEnableService) &&
         !prefs->GetBoolean(password_manager::prefs::kCredentialsEnableService);
}

template <class Credential>
Identity IdentityOf(const Credential& credential) {
  return {credential.signon_realm, credential.username_value};
}

class PreservingPasswordProfileWriter final : public ProfileWriter,
                                              public Store::Observer {
 public:
  PreservingPasswordProfileWriter(Profile* profile,
                                  scoped_refptr<Store> store,
                                  IdentitySet identities,
                                  BlockedRealms blocked_realms)
      : ProfileWriter(profile),
        profile_(profile->GetWeakPtr()),
        store_(std::move(store)),
        identities_(std::move(identities)),
        blocked_realms_(std::move(blocked_realms)) {
    observation_.Observe(store_.get());
  }

  void AddPasswordForm(const password_manager::PasswordForm& form) override {
    DCHECK_CURRENTLY_ON(content::BrowserThread::UI);
    if (!profile_ || profile_->ShutdownStarted() ||
        IsPasswordImportDisabledByPolicy(profile_.get()) ||
        blocked_realms_.contains(form.signon_realm) ||
        !identities_.insert(IdentityOf(form)).second) {
      return;
    }
    if (form.blocked_by_user) {
      blocked_realms_.insert(form.signon_realm);
    }
    // The inherited writer couples writes to the offer-to-save preference.
    // Explicit imports use the inspected store without changing that setting.
    store_->AddLogin(password_manager::FromPasswordForm(form));
  }

  void OnLoginsChanged(
      Store*,
      const password_manager::PasswordStoreChangeList& changes) override {
    for (const auto& change : changes) {
      // Never unreserve an identity during import, including after deletion.
      identities_.insert(IdentityOf(change.credential()));
      if (change.credential().blocked_by_user) {
        blocked_realms_.insert(change.credential().signon_realm);
      }
    }
  }

  void OnLoginsRetained(
      Store*,
      const std::vector<StoredCredential>& credentials) override {
    for (const auto& credential : credentials) {
      identities_.insert(IdentityOf(credential));
      if (credential.blocked_by_user) {
        blocked_realms_.insert(credential.signon_realm);
      }
    }
  }

 private:
  ~PreservingPasswordProfileWriter() override = default;
  base::WeakPtr<Profile> profile_;
  scoped_refptr<Store> store_;
  IdentitySet identities_;
  BlockedRealms blocked_realms_;
  base::ScopedObservation<Store, Store::Observer> observation_{this};
};

}  // namespace

class PasswordProfileWriterPreparation::Impl final
    : public password_manager::PasswordStoreConsumer,
      public Store::Observer {
 public:
  Impl(Profile* profile, PasswordProfileWriterCallback callback)
      : profile_(profile->GetWeakPtr()), callback_(std::move(callback)) {
    DCHECK_CURRENTLY_ON(content::BrowserThread::UI);
    base::SequencedTaskRunner::GetCurrentDefault()->PostTask(
        FROM_HERE, base::BindOnce(&Impl::Start, weak_factory_.GetWeakPtr()));
  }

  void OnGetPasswordStoreResultsOrErrorFrom(
      Store* store,
      base::expected<std::vector<StoredCredential>,
                     password_manager::PasswordStoreBackendError> result)
      override {
    DCHECK_CURRENTLY_ON(content::BrowserThread::UI);
    if (!profile_ || profile_->ShutdownStarted() || store != store_.get() ||
        !result.has_value()) {
      Finish(base::unexpected(std::string(kInspectionFailed)));
      return;
    }
    if (IsPasswordImportDisabledByPolicy(profile_.get())) {
      Finish(base::unexpected(std::string(kDisabledByPolicy)));
      return;
    }
    for (const auto& credential : result.value()) {
      identities_.insert(IdentityOf(credential));
      if (credential.blocked_by_user) {
        blocked_realms_.insert(credential.signon_realm);
      }
    }
    auto writer = base::MakeRefCounted<PreservingPasswordProfileWriter>(
        profile_.get(), store_, std::move(identities_),
        std::move(blocked_realms_));
    Finish(scoped_refptr<ProfileWriter>(std::move(writer)));
  }

  void OnLoginsChanged(
      Store*,
      const password_manager::PasswordStoreChangeList& changes) override {
    for (const auto& change : changes) {
      identities_.insert(IdentityOf(change.credential()));
      if (change.credential().blocked_by_user) {
        blocked_realms_.insert(change.credential().signon_realm);
      }
    }
  }

  void OnLoginsRetained(
      Store*,
      const std::vector<StoredCredential>& credentials) override {
    for (const auto& credential : credentials) {
      identities_.insert(IdentityOf(credential));
      if (credential.blocked_by_user) {
        blocked_realms_.insert(credential.signon_realm);
      }
    }
  }

 private:
  void Start() {
    DCHECK_CURRENTLY_ON(content::BrowserThread::UI);
    // Offering to save passwords is separate from a user-requested import.
    // Keep manual imports available when saving prompts are disabled.
    if (!profile_ || profile_->ShutdownStarted() ||
        base::FeatureList::IsEnabled(
            password_manager::features::kSkipUndecryptablePasswords) ||
        base::FeatureList::IsEnabled(
            password_manager::features::kClearUndecryptablePasswords)) {
      // With partial-result mode enabled GetAllLogins can omit encrypted
      // credentials without an error, so its identity set cannot protect them.
      // The optional cleanup mode could also delete entries during inspection.
      Finish(base::unexpected(std::string(kInspectionFailed)));
      return;
    }
    if (IsPasswordImportDisabledByPolicy(profile_.get())) {
      Finish(base::unexpected(std::string(kDisabledByPolicy)));
      return;
    }
    store_ = ProfilePasswordStoreFactory::GetForProfile(
        profile_.get(), ServiceAccessType::EXPLICIT_ACCESS);
    if (!store_) {
      Finish(base::unexpected(std::string(kInspectionFailed)));
      return;
    }
    observation_.Observe(store_.get());
    timeout_.Start(
        FROM_HERE, base::Seconds(60),
        base::BindOnce(&Impl::Finish, weak_factory_.GetWeakPtr(),
                       base::unexpected(std::string(kInspectionFailed))));
    store_->GetAllLogins(weak_factory_.GetWeakPtr());
  }

  void Finish(PreparedPasswordProfileWriter result) {
    // Callback can destroy this request. Release observation and invalidate all
    // other tasks first, then make the callback the final access to members.
    timeout_.Stop();
    observation_.Reset();
    weak_factory_.InvalidateWeakPtrs();
    auto callback = std::move(callback_);
    store_.reset();
    if (callback) {
      std::move(callback).Run(std::move(result));
    }
  }

  base::WeakPtr<Profile> profile_;
  PasswordProfileWriterCallback callback_;
  scoped_refptr<Store> store_;
  IdentitySet identities_;
  BlockedRealms blocked_realms_;
  base::ScopedObservation<Store, Store::Observer> observation_{this};
  base::OneShotTimer timeout_;
  base::WeakPtrFactory<Impl> weak_factory_{this};
};

PasswordProfileWriterPreparation::PasswordProfileWriterPreparation(
    Profile* profile,
    PasswordProfileWriterCallback callback)
    : impl_(std::make_unique<Impl>(profile, std::move(callback))) {}

PasswordProfileWriterPreparation::~PasswordProfileWriterPreparation() = default;

std::unique_ptr<PasswordProfileWriterPreparation> PreparePasswordProfileWriter(
    Profile* profile,
    PasswordProfileWriterCallback callback) {
  DCHECK_CURRENTLY_ON(content::BrowserThread::UI);
  return std::unique_ptr<PasswordProfileWriterPreparation>(
      new PasswordProfileWriterPreparation(profile, std::move(callback)));
}

}  // namespace noventra
