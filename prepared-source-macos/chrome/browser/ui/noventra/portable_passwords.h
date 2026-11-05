// Copyright 2026 The Noventra Authors. All rights reserved.
#ifndef CHROME_BROWSER_UI_NOVENTRA_PORTABLE_PASSWORDS_H_
#define CHROME_BROWSER_UI_NOVENTRA_PORTABLE_PASSWORDS_H_

#include <memory>
#include <string>
#include <vector>

#include "base/functional/callback.h"
#include "base/memory/raw_ptr.h"
#include "base/memory/scoped_refptr.h"
#include "base/memory/weak_ptr.h"
#include "base/scoped_observation.h"
#include "base/timer/timer.h"
#include "base/types/expected.h"
#include "base/values.h"
#include "chrome/browser/profiles/profile_observer.h"
#include "components/password_manager/core/browser/import/csv_password.h"
#include "components/password_manager/services/csv_password/public/mojom/csv_password_parser.mojom.h"
#include "mojo/public/cpp/bindings/remote.h"

class Profile;
namespace content {
class WebContents;
}
namespace device_reauth {
class DeviceAuthenticator;
}
namespace password_manager {
struct ImportResults;
class PasswordImporter;
class PasswordStoreInterface;
class SavedPasswordsPresenter;
}  // namespace password_manager

namespace noventra {

// UI-sequence helper owned by the transfer WebUI. Password plaintext stays in
// native memory: ExportCSV is for the native encrypted-bundle caller only.
//
// Only one operation/review can be active. PrepareImport parses in Chromium's
// sandboxed CSV service and does not write. CommitImport refreshes the stores,
// then uses the native importer, keeping every existing login and its notes.
// Notes, passkeys, blocked and federated credentials are not transferred.
//
// Cancel drops uncommitted data and pending callbacks. A commit already handed
// to a password store cannot be rolled back by destroying/canceling the helper.
// The owner must cancel/destroy on WebUI unload. Profile shutdown also cancels.
class PortablePasswords final : public ProfileObserver {
 public:
  using ExportResult = base::expected<std::string, std::string>;
  using ExportCallback = base::OnceCallback<void(ExportResult)>;
  using Result = base::expected<base::DictValue, std::string>;
  using Callback = base::OnceCallback<void(Result)>;

  explicit PortablePasswords(Profile* profile);
  PortablePasswords(const PortablePasswords&) = delete;
  PortablePasswords& operator=(const PortablePasswords&) = delete;
  ~PortablePasswords() override;

  static bool IsAvailable(Profile* profile);

  // Matches the native password-manager export gate: fresh authentication on
  // Mac/Windows/ChromeOS. The pinned Linux native exporter has no OS prompt.
  // Call only after an explicit user transfer/export request.
  void ExportCSV(content::WebContents* contents, ExportCallback callback);

  // Count-only review keys: passwords, new_passwords, kept_passwords.
  // Malformed/oversized/invalid credential data rejects the entire preparation.
  void PrepareImport(std::string csv, Callback callback);

  // Count-only outcome keys: imported, kept, failed. "imported" follows the
  // native importer's processed-success count; concurrent identical additions
  // can be counted as already present. No selected replacement IDs are used.
  void CommitImport(Callback callback);
  void Cancel();

 private:
  enum class State { kIdle, kExporting, kPreparing, kPrepared, kCommitting };

  void OnProfileWillBeDestroyed(Profile* profile) override;
  void OnAuthenticated(base::WeakPtr<content::WebContents> contents,
                       bool authenticated);
  void StartPresenter();
  void OnPresenterReady();
  void OnCSVParsed(password_manager::mojom::CSVPasswordSequencePtr sequence);
  void PrepareAgainstCurrentStore();
  void OnImportPrepared(const password_manager::ImportResults& result);
  void OnImportCommitted(const password_manager::ImportResults& result);
  void Fail(std::string message);
  void FinishExport(ExportResult result);
  void FinishImport(Result result);
  bool StoresAvailable() const;

  raw_ptr<Profile> profile_;
  base::ScopedObservation<Profile, ProfileObserver> profile_observation_{this};
  State state_ = State::kIdle;
  ExportCallback export_callback_;
  Callback callback_;
  std::unique_ptr<device_reauth::DeviceAuthenticator> authenticator_;
  scoped_refptr<password_manager::PasswordStoreInterface> profile_store_;
  scoped_refptr<password_manager::PasswordStoreInterface> account_store_;
  std::unique_ptr<password_manager::SavedPasswordsPresenter> presenter_;
  std::unique_ptr<password_manager::PasswordImporter> importer_;
  mojo::Remote<password_manager::mojom::CSVPasswordParser> parser_;
  std::vector<password_manager::CSVPassword> incoming_;
  size_t passwords_count_ = 0;
  size_t kept_count_ = 0;
  size_t prepared_count_ = 0;
  base::OneShotTimer operation_timer_;
  base::WeakPtrFactory<PortablePasswords> weak_factory_{this};
};

}  // namespace noventra

#endif  // CHROME_BROWSER_UI_NOVENTRA_PORTABLE_PASSWORDS_H_
