// Copyright 2026 The Noventra Authors. All rights reserved.
#ifndef CHROME_BROWSER_UI_NOVENTRA_WORKSPACE_SYNC_SERVICE_H_
#define CHROME_BROWSER_UI_NOVENTRA_WORKSPACE_SYNC_SERVICE_H_

#include <cstdint>
#include <map>
#include <memory>
#include <string>

#include "base/files/file_path.h"
#include "base/functional/callback.h"
#include "base/memory/raw_ptr.h"
#include "base/memory/scoped_refptr.h"
#include "base/memory/weak_ptr.h"
#include "base/threading/sequence_bound.h"
#include "base/timer/timer.h"
#include "base/types/expected.h"
#include "base/values.h"
#include "components/keyed_service/core/keyed_service.h"
#include "components/prefs/pref_change_registrar.h"

class BrowserWindowInterface;
class Profile;
namespace os_crypt_async {
class Encryptor;
}
namespace syncer {
class CancelationSignal;
}

namespace noventra {
using WorkspaceSyncPrefWrites = std::map<base::FilePath, base::DictValue>;

class WorkspaceSyncWorker;
struct WorkspaceSyncReply;
class WorkspaceSyncServiceFactory;

// UI-sequence regular-profile service. Network, vault crypto and durable file
// operations run off UI. Polling Status() never waits for an active operation.
class WorkspaceSyncService final : public KeyedService {
 public:
  using Result = base::expected<base::DictValue, std::string>;
  using Callback = base::OnceCallback<void(Result)>;

  static WorkspaceSyncService* GetForProfile(Profile* profile);
  static void EnsureFactoryBuilt();
  ~WorkspaceSyncService() override;

  base::DictValue Status() const;
  // status/create/join/sync/use-local/use-remote/disconnect. Mutation callbacks
  // complete after the operation, not after queueing. Disconnect cancels even
  // while busy; other overlapping mutations fail with sync_busy.
  void Handle(BrowserWindowInterface* browser,
              const std::string& action,
              const base::DictValue& payload,
              Callback callback);
  void Shutdown() override;

 private:
  friend class WorkspaceSyncServiceFactory;
  friend class WorkspaceSyncServiceTestPeer;
  // All callers run on UI; disconnect, its retry and shutdown may share a
  // signal until durable disconnect succeeds.
  static void SignalCancelationOnce(syncer::CancelationSignal& signal);
  explicit WorkspaceSyncService(Profile* profile);
  void OnStateFileFound(bool exists);
  void RequestEncryptor();
  void OnEncryptor(uint64_t epoch,
                   scoped_refptr<os_crypt_async::Encryptor> encryptor);
  void OnLoaded(uint64_t epoch, WorkspaceSyncReply reply);
  void Start(BrowserWindowInterface* browser,
             const std::string& action,
             const base::DictValue& payload,
             Callback callback);
  void OnReply(uint64_t operation, WorkspaceSyncReply reply);
  void OnPrefsWritten(uint64_t operation, WorkspaceSyncPrefWrites expected);
  void OnDisconnected(uint64_t operation, WorkspaceSyncReply reply);
  void Finish(WorkspaceSyncReply reply);
  void OnLocalChange();
  void Poll();
  BrowserWindowInterface* FindBrowser() const;

  const raw_ptr<Profile> profile_;
  base::SequenceBound<WorkspaceSyncWorker> worker_;
  std::shared_ptr<syncer::CancelationSignal> cancelation_;
  PrefChangeRegistrar prefs_registrar_;
  PrefChangeRegistrar catalog_prefs_registrar_;
  base::RepeatingTimer poll_timer_;
  base::OneShotTimer debounce_timer_;
  base::OneShotTimer initialization_timer_;
  base::DictValue status_;
  Callback callback_;
  uint64_t operation_ = 0;
  bool ready_ = false;
  bool busy_ = false;
  bool applying_ = false;
  bool dirty_ = false;
  bool shutdown_ = false;
  bool disconnecting_ = false;
  std::string recovery_once_;
  uint64_t initialization_epoch_ = 0;
  std::string setup_action_;
  base::DictValue setup_payload_;
  base::WeakPtrFactory<WorkspaceSyncService> weak_factory_{this};
};
}  // namespace noventra
#endif  // CHROME_BROWSER_UI_NOVENTRA_WORKSPACE_SYNC_SERVICE_H_
