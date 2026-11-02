// Copyright 2026 The Noventra Authors. All rights reserved.
#include "chrome/browser/ui/noventra/workspace_sync_service.h"

#include <algorithm>
#include <limits>
#include <optional>
#include <set>
#include <utility>

#include "base/base64url.h"
#include "base/files/file.h"
#include "base/files/file_util.h"
#include "base/files/important_file_writer.h"
#include "base/functional/bind.h"
#include "base/i18n/time_formatting.h"
#include "base/json/json_reader.h"
#include "base/json/json_writer.h"
#include "base/no_destructor.h"
#include "base/rand_util.h"
#include "base/strings/string_number_conversions.h"
#include "base/task/thread_pool.h"
#include "base/uuid.h"
#include "build/build_config.h"
#if BUILDFLAG(IS_POSIX)
#include <unistd.h>

#include "base/posix/eintr_wrapper.h"
#endif
#include "chrome/browser/browser_process.h"
#include "chrome/browser/profiles/noventra_account_spaces.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/profiles/profile_keyed_service_factory.h"
#include "chrome/browser/ui/browser_window/public/browser_window_interface.h"
#include "chrome/browser/ui/browser_window/public/global_browser_collection.h"
#include "chrome/browser/ui/noventra/workspace_sync_document.h"
#include "chrome/browser/ui/noventra/workspace_sync_transport.h"
#include "chrome/browser/ui/noventra_commands.h"
#include "chrome/browser/ui/tabs/tab_strip_model.h"
#include "chrome/common/chrome_constants.h"
#include "components/os_crypt/async/browser/os_crypt_async.h"
#include "components/os_crypt/async/common/encryptor.h"
#include "components/prefs/pref_service.h"
#include "components/sync/engine/cancelation_signal.h"
#include "components/noventra_sync_vault/sync_vault_store.h"

namespace noventra {
namespace {
constexpr char kStateFilename[] = "Noventra Workspace Sync";
constexpr size_t kMaximumProtectedStateBytes = 16 * 1024 * 1024;
constexpr const char* kModelPrefs[] = {kSpacesPref, kSpaceOrderPref,
                                       kArchivedSpacesPref, kDockLayoutPref};
constexpr auto kPollInterval = base::Seconds(15);
constexpr auto kOperationDeadline = base::Seconds(90);

std::string Encode(base::span<const uint8_t> bytes) {
  std::string result;
  base::Base64UrlEncode(bytes, base::Base64UrlEncodePolicy::OMIT_PADDING,
                        &result);
  return result;
}
std::optional<std::vector<uint8_t>> Decode(std::string_view text) {
  auto result = base::Base64UrlDecode(
      text, base::Base64UrlDecodePolicy::DISALLOW_PADDING);
  if (!result || Encode(*result) != text) {
    return std::nullopt;
  }
  return result;
}

// ImportantFileWriter flushes the temporary file before atomic replacement.
// On POSIX the directory must also be flushed before a checkpoint can succeed.
// This is deliberately independent of PrefService's asynchronous writer.
bool FlushDirectory(const base::FilePath& directory) {
#if BUILDFLAG(IS_POSIX)
  base::File file(directory, base::File::FLAG_OPEN | base::File::FLAG_READ);
  return file.IsValid() && HANDLE_EINTR(fsync(file.GetPlatformFile())) == 0;
#else
  return true;
#endif
}

// Inspect the ciphertext provider prefix because the pinned Encryptor API
// exposes availability, but not the selected provider. Linux v10 is the public
// PosixKeyProvider "peanuts" fallback, not protected key storage. These tags
// are pinned by FreedesktopSecretKeyProvider (v11) and SecretPortalKeyProvider
// (v12). Do not apply Linux tag meanings to Windows or macOS.
bool HasProtectedProviderTag(base::span<const uint8_t> ciphertext) {
#if BUILDFLAG(IS_LINUX)
  const auto value = base::as_string_view(ciphertext);
  return value.starts_with("v11") || value.starts_with("v12");
#else
  return !ciphertext.empty();
#endif
}

bool DeleteStateFile(const base::FilePath& path) {
  return base::DeleteFile(path) && FlushDirectory(path.DirName());
}

base::DictValue EmptyStatus() {
  return base::DictValue()
      .Set("configured", false)
      .Set("syncing", false)
      .Set("conflict", false)
      .Set("state", "unconfigured")
      .Set("error", "")
      .Set("last_success", "")
      .Set("generation", 0.0)
      .Set("origin", "")
      .Set("vault_id", "")
      .Set("local_only_spaces", 0)
      .Set("scope", "workspace-settings-v1");
}

std::string StoreError(noventra_sync::SyncVaultStoreError error) {
  using E = noventra_sync::SyncVaultStoreError;
  switch (error) {
    case E::kAuthenticationRequired:
      return "sync_auth_required";
    case E::kAccessDenied:
      return "sync_access_denied";
    case E::kConflict:
      return "sync_cas_conflict";
    case E::kNetworkError:
      return "sync_network_error";
    case E::kTemporaryError:
      return "sync_temporarily_unavailable";
    case E::kQuotaExceeded:
      return "sync_quota_exceeded";
    case E::kAborted:
      return "sync_aborted";
    case E::kInvalidVaultKey:
      return "invalid_recovery_key";
    case E::kAuthenticationFailed:
      return "sync_authentication_failed";
    case E::kRollback:
      return "sync_rollback";
    case E::kFork:
      return "sync_fork";
    case E::kIncompleteHistory:
      return "sync_incomplete_history";
    case E::kCheckpointPersistenceFailed:
      return "sync_storage_error";
    case E::kUnsupported:
    case E::kUnsupportedVersion:
      return "sync_unsupported_version";
    case E::kInvalidData:
    case E::kMalformedObject:
    case E::kObjectTooLarge:
      return "sync_invalid_data";
  }
}

base::DictValue ReadModelPrefs(PrefService& prefs) {
  base::DictValue result;
  for (const auto* key : kModelPrefs) {
    result.Set(key, prefs.GetValue(key).Clone());
  }
  return result;
}

bool CanWriteModel(Profile* profile) {
  if (!profile || !profile->IsRegularProfile() || profile->IsOffTheRecord()) {
    return false;
  }
  for (const auto* key : kModelPrefs) {
    const auto* pref = profile->GetPrefs()->FindPreference(key);
    if (!pref || !pref->IsUserModifiable()) {
      return false;
    }
  }
  return true;
}

// Recovery accepts only complete before/after values for each atomic pref.
// Unrelated edits made after an interrupted apply require explicit resolution.
bool CanReplay(const base::DictValue& current,
               const base::DictValue& before,
               const base::DictValue& after) {
  for (const auto* key : kModelPrefs) {
    const auto* value = current.Find(key);
    if (!value || ((!before.Find(key) || *value != *before.Find(key)) &&
                   (!after.Find(key) || *value != *after.Find(key)))) {
      return false;
    }
  }
  return true;
}
}  // namespace

struct WorkspaceSyncSnapshot {
  WorkspaceSyncDocument local;
  base::DictValue prefs;
  std::vector<LayoutCapability> capabilities;
};
struct WorkspaceSyncApply {
  WorkspaceSyncDocument document;
  WorkspaceSyncMapping mapping;
  base::DictValue before;
  base::DictValue after;
};
struct WorkspaceSyncReply {
  base::DictValue status;
  std::optional<WorkspaceSyncApply> apply;
  std::string recovery_key;
};

namespace {
base::expected<WorkspaceSyncSnapshot, std::string> Capture(
    Profile* profile,
    BrowserWindowInterface* browser) {
  if (!browser || browser->GetProfile() != profile ||
      browser->IsDeleteScheduled() || !browser->GetTabStripModel() ||
      browser->GetTabStripModel()->closing_all()) {
    return base::unexpected("sync_awaiting_window");
  }
  if (!CanWriteModel(profile)) {
    return base::unexpected("workspace_settings_managed");
  }
  WorkspaceSyncSnapshot snapshot;
  auto& prefs = *profile->GetPrefs();
  snapshot.capabilities = GetLayoutCapabilities(prefs);
  auto saved_layout =
      ValidateLayout(base::Value(prefs.GetDict(kDockLayoutPref).Clone()),
                     snapshot.capabilities);
  if (!saved_layout.has_value()) {
    return base::unexpected("sync_layout_not_ready");
  }
  snapshot.local.layout = std::move(*saved_layout);
  // Read without the migration writes performed by SpaceIds. Failed joins must
  // not modify model preferences, even on profiles that need migration.
  std::vector<std::string> known = {""};
  const auto add = [&](const std::string& id) {
    if ((id.empty() || IsWorkspaceSyncUuid(id)) &&
        std::ranges::find(known, id) == known.end()) {
      known.push_back(id);
    }
  };
  for (const auto [id, name] : prefs.GetDict(kSpacesPref)) {
    if (name.is_string()) {
      add(id);
    }
  }
  GlobalBrowserCollection::GetInstance()->ForEach(
      [&](BrowserWindowInterface* candidate) {
        if (candidate->GetProfile() == profile &&
            candidate->GetTabStripModel()) {
          for (int i = 0; i < candidate->GetTabStripModel()->count(); ++i) {
            add(SpaceId(candidate->GetTabStripModel()->GetWebContentsAt(i)));
          }
        }
        return true;
      });
  std::vector<std::string> ids;
  for (const auto& value : prefs.GetList(kSpaceOrderPref)) {
    if (value.is_string() &&
        std::ranges::find(known, value.GetString()) != known.end() &&
        std::ranges::find(ids, value.GetString()) == ids.end()) {
      ids.push_back(value.GetString());
    }
  }
  for (const auto& id : known) {
    if (std::ranges::find(ids, id) == ids.end()) {
      ids.push_back(id);
    }
  }
  if (ids.size() > kWorkspaceSyncMaximumLocalSpaces) {
    return base::unexpected("too_many_local_workspaces");
  }
  for (const auto& id : ids) {
    const auto* name = prefs.GetDict(kSpacesPref).FindString(id);
    snapshot.local.spaces.push_back(
        {id, name ? *name : (id.empty() ? "Personal" : "Account space"),
         IsSpaceArchived(profile, id)});
  }
  snapshot.prefs = ReadModelPrefs(prefs);
  return snapshot;
}

base::DictValue MakeApplyPrefs(const WorkspaceSyncSnapshot& local,
                               const WorkspaceSyncDocument& remote,
                               const WorkspaceSyncMapping& mapping) {
  auto result = local.prefs.Clone();
  auto names = result.FindDict(kSpacesPref)->Clone();
  auto archives = result.FindList(kArchivedSpacesPref)->Clone();
  base::ListValue order;
  std::set<std::string> remote_local_ids;
  for (const auto& space : remote.spaces) {
    const auto& id = mapping.at(space.id);
    names.Set(id, space.name);
    order.Append(id);
    remote_local_ids.insert(id);
    archives.EraseValue(base::Value(id));
    if (space.archived) {
      archives.Append(id);
    }
  }
  // Physical spaces, account partitions and local-only relative order survive
  // remote removals and both conflict resolutions.
  for (const auto& space : local.local.spaces) {
    if (!remote_local_ids.contains(space.id)) {
      order.Append(space.id);
    }
  }
  result.Set(kSpacesPref, std::move(names));
  result.Set(kSpaceOrderPref, std::move(order));
  result.Set(kArchivedSpacesPref, std::move(archives));
  if (!WorkspaceSyncLayoutsEqual(local.local.layout, remote.layout)) {
    auto layout = remote.layout;
    layout.revision = local.local.layout.revision + 1;
    result.Set(kDockLayoutPref, LayoutToValue(layout));
  }
  return result;
}
}  // namespace

// All members are confined to one MayBlock sequence. No Profile, PrefService,
// BrowserWindow or UI weak pointer is used here. The shared cancellation signal
// outlives each short-lived SyncVaultStore and its transport.
class WorkspaceSyncWorker {
 public:
  WorkspaceSyncWorker(base::FilePath profile_path,
                      scoped_refptr<os_crypt_async::Encryptor> encryptor,
                      scoped_refptr<WorkspaceSyncNetwork> network,
                      std::shared_ptr<syncer::CancelationSignal> cancelation)
      : profile_path_(std::move(profile_path)),
        path_(profile_path_.AppendASCII(kStateFilename)),
        encryptor_(std::move(encryptor)),
        network_(std::move(network)),
        cancelation_(std::move(cancelation)) {}

  WorkspaceSyncReply Load() {
    file_present_ = base::PathExists(path_);
    auto probe =
        encryptor_->EncryptString("Noventra workspace sync protection probe");
    if (!encryptor_->IsEncryptionAvailable() || !probe ||
        !HasProtectedProviderTag(*probe)) {
      return Reply("error", "sync_key_storage_unavailable");
    }
#if BUILDFLAG(IS_LINUX)
    const bool using_v11 =
        base::as_string_view(base::span(*probe)).starts_with("v11");
#endif
    if (!file_present_) {
      return Reply("unconfigured");
    }
    std::string ciphertext;
    if (!base::ReadFileToStringWithMaxSize(path_, &ciphertext,
                                           kMaximumProtectedStateBytes)) {
      return Reply("error", "sync_storage_error");
    }
    if (!HasProtectedProviderTag(base::as_byte_span(ciphertext))) {
      return Reply("error", "sync_protected_state_unavailable");
    }
    auto plain = encryptor_->DecryptData(base::as_byte_span(ciphertext));
#if BUILDFLAG(IS_LINUX)
    // The inherited CBC decoder also tries a public empty-password fallback.
    // For v11, deterministic re-encryption must match exactly with the active
    // protected key. Never accept a relabeled public-key ciphertext through
    // that compatibility path. v12 is authenticated AES-GCM.
    if (ciphertext.starts_with("v11")) {
      auto verified =
          plain && using_v11 ? encryptor_->EncryptString(*plain) : std::nullopt;
      if (!verified ||
          base::as_string_view(base::span(*verified)) != ciphertext) {
        return Reply("error", "sync_protected_state_unavailable");
      }
    }
#endif
    auto parsed = plain
                      ? base::JSONReader::ReadDict(*plain, base::JSON_PARSE_RFC)
                      : std::nullopt;
    if (!parsed || parsed->FindInt("version") != 1) {
      return Reply("error", "sync_protected_state_unavailable");
    }
    state_ = std::move(*parsed);
    const auto* origin = state_.FindString("origin");
    const auto* token = state_.FindString("token");
    const auto* id = state_.FindString("vault_id");
    const auto* key = state_.FindString("key");
    const auto* map = state_.FindDict("mapping");
    const auto* ignored = state_.FindList("ignored");
    if (!origin || !ValidateWorkspaceSyncOrigin(*origin).has_value() ||
        !token || !IsWorkspaceSyncToken(*token) || !id ||
        !IsWorkspaceSyncUuid(*id) || !key || !Decode(*key) ||
        Decode(*key)->size() != 32 || !map || !ignored ||
        ignored->size() > kWorkspaceSyncMaximumRetainedLocalIds ||
        !state_.FindBool("creating")) {
      return Reply("error", "sync_protected_state_unavailable");
    }
    auto mapping = ParseWorkspaceSyncMapping(*map);
    if (!mapping.has_value()) {
      return Reply("error", mapping.error());
    }
    mapping_ = std::move(*mapping);
    std::set<std::string> local_ids;
    for (const auto& [logical, local] : mapping_) {
      local_ids.insert(local);
    }
    for (const auto& value : *ignored) {
      if (!value.is_string() || !IsWorkspaceSyncUuid(value.GetString()) ||
          local_ids.contains(value.GetString()) ||
          !ignored_.insert(value.GetString()).second) {
        return Reply("error", "sync_protected_state_unavailable");
      }
    }
    auto capacity = ValidateWorkspaceSyncIdentityCapacity(mapping_, ignored_);
    if (!capacity.has_value()) {
      return Reply("error", capacity.error());
    }
    if (const auto* saved = state_.FindString("checkpoint")) {
      auto bytes = Decode(*saved);
      if (!bytes) {
        return Reply("error", "sync_protected_state_unavailable");
      }
      auto checkpoint = noventra_sync::ParseSyncVaultCheckpoint(*bytes);
      if (!checkpoint.has_value() || checkpoint->vault_uuid != *id) {
        return Reply("error", "sync_protected_state_unavailable");
      }
      checkpoint_ = std::move(*checkpoint);
    }
    if ((state_.FindDict("common") || state_.FindDict("pending") ||
         state_.FindDict("conflict")) &&
        (!checkpoint_ || checkpoint_->generation == 0)) {
      return Reply("error", "sync_protected_state_unavailable");
    }
    loaded_ = true;
    return Reply(state_.FindDict("conflict") ? "conflict" : "configured");
  }

  WorkspaceSyncReply Configure(bool create,
                               base::DictValue payload,
                               WorkspaceSyncSnapshot local) {
    if (file_present_ || !state_.empty()) {
      return Reply("error", "sync_already_configured");
    }
    if (create && local.local.spaces.size() > kWorkspaceSyncMaximumSpaces) {
      return Reply("error", "too_many_synced_workspaces");
    }
    const auto* origin = payload.FindString("origin");
    const auto* token = payload.FindString("token");
    if (!origin || !token) {
      return Reply("error", "invalid_sync_configuration");
    }
    auto checked_origin = ValidateWorkspaceSyncOrigin(*origin);
    if (!checked_origin.has_value()) {
      return Reply("error", checked_origin.error());
    }
    if (!IsWorkspaceSyncToken(*token)) {
      return Reply("error", "invalid_sync_token");
    }
    std::string id;
    std::string key;
    if (create) {
      id = base::Uuid::GenerateRandomV4().AsLowercaseString();
      key = Encode(base::RandBytesAsVector(32));
    } else {
      const auto* supplied_id = payload.FindString("vault_id");
      const auto* supplied_key = payload.FindString("recovery_key");
      if (!supplied_id || !IsWorkspaceSyncUuid(*supplied_id)) {
        return Reply("error", "invalid_sync_vault_id");
      }
      const auto decoded = supplied_key ? Decode(*supplied_key) : std::nullopt;
      if (!decoded || decoded->size() != 32) {
        return Reply("error", "invalid_recovery_key");
      }
      id = *supplied_id;
      key = *supplied_key;
    }
    mapping_.emplace(kWorkspaceSyncPersonalId, "");
    for (const auto& space : local.local.spaces) {
      if (space.id.empty()) {
        continue;
      }
      if (create) {
        mapping_.emplace(base::Uuid::GenerateRandomV4().AsLowercaseString(),
                         space.id);
      } else {
        ignored_.insert(space.id);
      }
    }
    auto capacity = ValidateWorkspaceSyncIdentityCapacity(mapping_, ignored_);
    if (!capacity.has_value()) {
      const auto error = capacity.error();
      Clear();
      return Reply("error", error);
    }
    state_ = base::DictValue()
                 .Set("version", 1)
                 .Set("origin", checked_origin->spec())
                 .Set("token", *token)
                 .Set("vault_id", id)
                 .Set("key", key)
                 .Set("creating", create)
                 .Set("last_success", "");
    loaded_ = true;
    if (!Save()) {
      if (!file_present_) {
        Clear();
      }
      auto reply = Reply("error", "sync_storage_error");
      if (create && file_present_) {
        reply.recovery_key = key;
      }
      return reply;
    }
    file_present_ = true;
    auto reply = Run("sync", std::move(local));
    // Even if the first network attempt fails, a durably configured creator
    // gets its recovery key exactly once. A later Sync can finish setup.
    if (create) {
      reply.recovery_key = std::move(key);
    }
    return reply;
  }

  WorkspaceSyncReply Run(std::string action, WorkspaceSyncSnapshot local) {
    auto reply = RunInternal(std::move(action), std::move(local));
    // This also covers a restart during a tentative join. No authenticated
    // settings or replay intent have been accepted yet, so credentials can be
    // corrected without leaving a half-configured profile behind.
    if (loaded_ && !state_.FindBool("creating").value_or(false) &&
        !state_.FindBool("apply_staged").value_or(false) &&
        !state_.FindDict("common") && !state_.FindDict("pending") &&
        !state_.FindDict("conflict") && reply.status.FindString("error") &&
        *reply.status.FindString("error") != "sync_catching_up" &&
        !reply.status.FindString("error")->empty()) {
      const auto failure = *reply.status.FindString("error");
      if (!DeleteStateFile(path_)) {
        return Reply("error", "sync_join_cleanup_failed");
      }
      Clear();
      return Reply("unconfigured", failure);
    }
    return reply;
  }

  WorkspaceSyncReply RunInternal(std::string action,
                                 WorkspaceSyncSnapshot local) {
    if (!loaded_ || storage_failed_ || !state_.FindString("vault_id")) {
      return Reply("error", "sync_protected_state_unavailable");
    }
    if (cancelation_->IsSignalled()) {
      if (store_) {
        store_->AbortRead();
      }
      return Reply("error", "sync_aborted");
    }
    if (const auto* pending = state_.FindDict("pending")) {
      if (action != "use-local" && action != "use-remote") {
        return PendingReply(*pending, local.capabilities);
      }
    }
    // Enrollment remains a candidate until reconciliation admits a complete
    // operation. In particular, checkpoint saves during a rejected remote
    // admission must not persist an unrelated new local mapping.
    auto candidate_mapping = mapping_;
    auto local_document = MakeLocal(local, candidate_mapping);
    if (!local_document.has_value()) {
      return Reply("error", local_document.error());
    }
    if (const auto* pending = state_.FindDict("pending")) {
      std::optional<WorkspaceSyncDocument> pending_remote;
      if (action == "use-remote") {
        const auto* document = pending->FindDict("document");
        if (!document) {
          return Reply("error", "sync_protected_state_unavailable");
        }
        auto parsed =
            ValidateWorkspaceSyncDocument(*document, local.capabilities);
        if (!parsed.has_value()) {
          return Reply("error", parsed.error());
        }
        pending_remote = std::move(*parsed);
      }
      auto capacity = ValidateWorkspaceSyncApplyCapacity(
          candidate_mapping, ignored_, local.local.spaces,
          pending_remote ? pending_remote->spaces : local_document->spaces);
      if (!capacity.has_value()) {
        return Reply("error", capacity.error());
      }
      // A new explicit resolution always re-reads the authenticated head,
      // including when a previous resolution stopped with a replay intent.
      // Admit the selected identity footprint before changing that intent.
      auto conflict = pending->Clone();
      conflict.Remove("before");
      conflict.Remove("after");
      state_.Set("conflict", std::move(conflict));
      state_.Remove("pending");
      mapping_ = candidate_mapping;
      if (!Save()) {
        return Reply("error", "sync_storage_error");
      }
    }
    const auto deadline = base::TimeTicks::Now() + kOperationDeadline;
    if (!store_) {
      auto key = Decode(*state_.FindString("key"));
      if (!key) {
        return Reply("error", "sync_protected_state_unavailable");
      }
      auto transport = std::make_unique<WorkspaceSyncTransport>(
          GURL(*state_.FindString("origin")), *state_.FindString("vault_id"),
          *state_.FindString("token"), network_, deadline);
      auto* transport_pointer = transport.get();
      auto created = noventra_sync::SyncVaultStore::Create(
          std::move(transport), *key, *state_.FindString("vault_id"),
          checkpoint_,
          base::BindRepeating(&WorkspaceSyncWorker::PersistCheckpoint,
                              base::Unretained(this)));
      if (!created.has_value()) {
        return Reply("error", StoreError(created.error()));
      }
      store_ = std::move(*created);
      transport_ = transport_pointer;
      store_->SetCancelationSignal(cancelation_.get());
    }
    auto& store = *store_;
    transport_->BeginOperation(deadline);
    auto progress = store.ReadStateIncrementally(
        {WorkspaceSyncTransport::kMaximumRequestsPerOperation,
         WorkspaceSyncTransport::kMaximumBytesPerOperation, deadline,
         base::BindRepeating(&syncer::CancelationSignal::IsSignalled,
                             cancelation_)});
    if (!progress.has_value()) {
      return Reply("error", StoreError(progress.error()));
    }
    if (!progress->has_value()) {
      return Reply("catching_up", "sync_catching_up");
    }
    const bool creating = state_.FindBool("creating").value_or(false);
    // Only a locally generated, durably enrolled new vault may initialize.
    // Join, restart of a joined vault and ordinary polling never initialize.
    noventra_sync::SyncVaultStore::ReadResult remote_state =
        std::move(progress->value());
    if (creating && !remote_state->initialized) {
      remote_state = store.InitializeEmptyVaultAndReadState();
    }
    if (!remote_state.has_value()) {
      return Reply("error", StoreError(remote_state.error()));
    }
    if (!remote_state->initialized) {
      return Reply("error", "sync_vault_missing");
    }
    if (creating && remote_state->head.generation == 0) {
      return Upload(store, *remote_state, *local_document, local,
                    candidate_mapping);
    }
    auto remote = ParseWorkspaceSyncDocument(
        base::as_string_view(base::span(remote_state->data)),
        local.capabilities);
    if (!remote.has_value()) {
      return Reply("error", remote.error());
    }
    std::optional<WorkspaceSyncDocument> common;
    if (const auto* value = state_.FindDict("common")) {
      auto parsed = ValidateWorkspaceSyncDocument(*value, local.capabilities);
      if (!parsed.has_value()) {
        return Reply("error", parsed.error());
      }
      common = std::move(*parsed);
    }
    const auto* conflict = state_.FindDict("conflict");
    if (action == "use-local" || action == "use-remote") {
      if (!conflict) {
        return Reply("error", "sync_no_conflict");
      }
      if (!SameHead(*conflict, *remote_state)) {
        return Conflict(*local_document, *remote, *remote_state,
                        candidate_mapping, "sync_remote_changed");
      }
      if (action == "use-local") {
        return Upload(store, *remote_state, *local_document, local,
                      candidate_mapping);
      }
      // An explicit remote choice also commits through CAS. This closes the
      // read/resolve race and gives both resolutions the same stale-head rule.
      return Upload(store, *remote_state, *remote, local, candidate_mapping,
                    &*local_document);
    }
    auto decision = CompareWorkspaceSyncDocuments(*local_document, *remote,
                                                  common ? &*common : nullptr);
    if (conflict && !WorkspaceSyncDocumentsEqual(*local_document, *remote)) {
      return Conflict(*local_document, *remote, *remote_state,
                      candidate_mapping);
    }
    switch (decision) {
      case WorkspaceSyncDecision::kEqual:
        return Accepted(*remote, candidate_mapping);
      case WorkspaceSyncDecision::kUpload:
        return Upload(store, *remote_state, *local_document, local,
                      candidate_mapping);
      case WorkspaceSyncDecision::kApply:
        return PrepareApply(local, *remote, *remote_state, candidate_mapping);
      case WorkspaceSyncDecision::kConflict:
        return Conflict(*local_document, *remote, *remote_state,
                        candidate_mapping);
    }
  }

  WorkspaceSyncReply RejectApply(std::string error) {
    const auto* pending = state_.FindDict("pending");
    if (pending) {
      auto conflict = pending->Clone();
      conflict.Remove("before");
      conflict.Remove("after");
      state_.Set("conflict", std::move(conflict));
      state_.Remove("pending");
      if (!Save()) {
        return Reply("error", "sync_storage_error");
      }
    }
    return Reply("conflict", std::move(error));
  }

  WorkspaceSyncReply FinishApply(base::DictValue expected) {
    if (cancelation_->IsSignalled()) {
      return Reply("error", "sync_aborted");
    }
    const auto* pending = state_.FindDict("pending");
    if (!pending || !pending->FindDict("document")) {
      return Reply("error", "sync_storage_error");
    }
    // CommitPendingWrite's callback is just a sequencing barrier and has no
    // success bit. Independently read back exactly the affected persisted prefs
    // and flush their file before clearing the durable replay intent.
    const auto preferences_path =
        profile_path_.Append(chrome::kPreferencesFilename);
    std::string bytes;
    if (!base::ReadFileToStringWithMaxSize(preferences_path, &bytes,
                                           64 * 1024 * 1024)) {
      return Reply("error", "sync_storage_error");
    }
    auto disk = base::JSONReader::ReadDict(bytes, base::JSON_PARSE_RFC);
    if (!disk) {
      return Reply("error", "sync_storage_error");
    }
    for (const auto* key : kModelPrefs) {
      const auto* value = disk->FindByDottedPath(key);
      if (!value || !expected.Find(key) || *value != *expected.Find(key)) {
        return Reply("error", "sync_preferences_not_durable");
      }
    }
    base::File file(preferences_path, base::File::FLAG_OPEN |
                                          base::File::FLAG_READ |
                                          base::File::FLAG_WRITE);
    if (!file.IsValid() || !file.Flush() || !FlushDirectory(profile_path_)) {
      return Reply("error", "sync_storage_error");
    }
    auto document = pending->FindDict("document")->Clone();
    // Detach removed logical spaces only after their physical locals have
    // survived the apply. They must not be enrolled again by the next poll.
    std::set<std::string> retained;
    for (const auto& space : *document.FindList("spaces")) {
      retained.insert(*space.GetDict().FindString("id"));
    }
    DetachWorkspaceSyncMappings(retained, mapping_, ignored_);
    state_.Set("common", std::move(document));
    state_.Remove("pending");
    state_.Remove("conflict");
    state_.Set("creating", false);
    state_.Set("last_success", base::TimeFormatAsIso8601(base::Time::Now()));
    return Save() ? Reply("synced") : Reply("error", "sync_storage_error");
  }

  WorkspaceSyncReply Disconnect() {
    // This runs after the canceled operation on the same worker sequence.
    // Deletion and a directory flush finish before acknowledging disconnect.
    if (store_) {
      store_->AbortRead();
    }
    if (!DeleteStateFile(path_)) {
      return Reply("error", "sync_storage_error");
    }
    Clear();
    return Reply("unconfigured");
  }

 private:
  void Clear() {
    transport_ = nullptr;
    store_.reset();
    state_.clear();
    mapping_.clear();
    ignored_.clear();
    checkpoint_.reset();
    file_present_ = false;
    loaded_ = false;
    storage_failed_ = false;
  }

  WorkspaceSyncReply Reply(std::string state, std::string error = {}) const {
    auto status = EmptyStatus();
    status.Set("configured", file_present_);
    status.Set("state", std::move(state));
    status.Set("error", std::move(error));
    status.Set("conflict", state_.FindDict("conflict") != nullptr);
    status.Set("origin", loaded_ && state_.FindString("origin")
                             ? *state_.FindString("origin")
                             : "");
    status.Set("vault_id", loaded_ && state_.FindString("vault_id")
                               ? *state_.FindString("vault_id")
                               : "");
    status.Set("last_success", state_.FindString("last_success")
                                   ? *state_.FindString("last_success")
                                   : "");
    status.Set("generation", checkpoint_
                                 ? static_cast<double>(checkpoint_->generation)
                                 : 0.0);
    status.Set("local_only_spaces", static_cast<int>(ignored_.size()));
    return {std::move(status), std::nullopt, {}};
  }

  bool Save() {
    if (cancelation_->IsSignalled() || !encryptor_->IsEncryptionAvailable()) {
      return false;
    }
    // Defensive persistence invariant. Admission paths check this before any
    // model mutation, and detach only transfers already reserved identities.
    if (!ValidateWorkspaceSyncIdentityCapacity(mapping_, ignored_)
             .has_value()) {
      storage_failed_ = true;
      return false;
    }
    state_.Set("mapping", WorkspaceSyncMappingToValue(mapping_));
    base::ListValue ignored;
    for (const auto& id : ignored_) {
      ignored.Append(id);
    }
    state_.Set("ignored", std::move(ignored));
    auto json = base::WriteJson(state_);
    auto encrypted = json && json->size() <= kMaximumProtectedStateBytes - 1024
                         ? encryptor_->EncryptString(*json)
                         : std::nullopt;
    if (!encrypted || !HasProtectedProviderTag(*encrypted) ||
        !base::ImportantFileWriter::WriteFileAtomically(
            path_, base::as_string_view(base::span(*encrypted))) ||
        !FlushDirectory(profile_path_)) {
      storage_failed_ = true;
      file_present_ = base::PathExists(path_);
      return false;
    }
    file_present_ = true;
    return true;
  }

  bool PersistCheckpoint(const noventra_sync::SyncVaultCheckpoint& checkpoint) {
    auto serialized = noventra_sync::SerializeSyncVaultCheckpoint(checkpoint);
    if (!serialized.has_value()) {
      return false;
    }
    state_.Set("checkpoint", Encode(*serialized));
    if (!Save()) {
      return false;
    }
    checkpoint_ = checkpoint;
    return true;
  }

  base::expected<WorkspaceSyncDocument, std::string> MakeLocal(
      const WorkspaceSyncSnapshot& snapshot,
      WorkspaceSyncMapping& candidate_mapping) const {
    WorkspaceSyncDocument result;
    result.layout = snapshot.local.layout;
    result.layout.revision = 0;
    for (const auto& space : snapshot.local.spaces) {
      if (ignored_.contains(space.id)) {
        continue;
      }
      auto found = std::ranges::find_if(
          candidate_mapping,
          [&](const auto& pair) { return pair.second == space.id; });
      if (found == candidate_mapping.end()) {
        const auto id = base::Uuid::GenerateRandomV4().AsLowercaseString();
        found = candidate_mapping.emplace(id, space.id).first;
      }
      result.spaces.push_back({found->first, space.name, space.archived});
    }
    if (result.spaces.size() > kWorkspaceSyncMaximumSpaces) {
      return base::unexpected("too_many_synced_workspaces");
    }
    auto capacity =
        ValidateWorkspaceSyncIdentityCapacity(candidate_mapping, ignored_);
    if (!capacity.has_value()) {
      return base::unexpected(capacity.error());
    }
    auto valid = ValidateWorkspaceSyncDocument(
        WorkspaceSyncDocumentToValue(result), snapshot.capabilities);
    if (!valid.has_value()) {
      return base::unexpected(valid.error());
    }
    return valid;
  }

  static base::DictValue HeadIdentity(const noventra_sync::SyncVaultState& remote) {
    return base::DictValue()
        .Set("revision", remote.head_revision.value_or(""))
        .Set("generation", base::NumberToString(remote.head.generation))
        .Set("record", remote.head.record_id);
  }
  static bool SameHead(const base::DictValue& saved,
                       const noventra_sync::SyncVaultState& remote) {
    const auto identity = HeadIdentity(remote);
    for (const auto* key : {"revision", "generation", "record"}) {
      if (!saved.Find(key) || *saved.Find(key) != *identity.Find(key)) {
        return false;
      }
    }
    return true;
  }

  WorkspaceSyncReply Conflict(const WorkspaceSyncDocument& local,
                              const WorkspaceSyncDocument& remote,
                              const noventra_sync::SyncVaultState& head,
                              const WorkspaceSyncMapping& candidate_mapping,
                              std::string error = {}) {
    auto conflict = HeadIdentity(head);
    conflict.Set("local", WorkspaceSyncDocumentToValue(local));
    conflict.Set("document", WorkspaceSyncDocumentToValue(remote));
    mapping_ = candidate_mapping;
    state_.Set("conflict", std::move(conflict));
    return Save() ? Reply("conflict", std::move(error))
                  : Reply("error", "sync_storage_error");
  }

  WorkspaceSyncReply Accepted(const WorkspaceSyncDocument& document,
                              const WorkspaceSyncMapping& candidate_mapping) {
    mapping_ = candidate_mapping;
    state_.Set("common", WorkspaceSyncDocumentToValue(document));
    state_.Remove("conflict");
    state_.Set("creating", false);
    state_.Set("last_success", base::TimeFormatAsIso8601(base::Time::Now()));
    return Save() ? Reply("synced") : Reply("error", "sync_storage_error");
  }

  WorkspaceSyncReply Upload(
      noventra_sync::SyncVaultStore& store,
      const noventra_sync::SyncVaultState& remote,
      const WorkspaceSyncDocument& local,
      const WorkspaceSyncSnapshot& snapshot,
      const WorkspaceSyncMapping& candidate_mapping,
      const WorkspaceSyncDocument* conflict_local = nullptr) {
    // Explicit use-local/use-remote also come through here. A capacity failure
    // must leave both remote CAS state and local enrollment/replay state alone.
    auto capacity = ValidateWorkspaceSyncApplyCapacity(
        candidate_mapping, ignored_, snapshot.local.spaces, local.spaces);
    if (!capacity.has_value()) {
      return Reply("error", capacity.error());
    }
    if (mapping_ != candidate_mapping) {
      mapping_ = candidate_mapping;
      if (!Save()) {
        return Reply("error", "sync_storage_error");
      }
    }
    const auto bytes = SerializeWorkspaceSyncDocument(local);
    auto committed = store.CommitState(remote, base::as_byte_span(bytes), 1);
    if (!committed.has_value()) {
      if (committed.error() == noventra_sync::SyncVaultStoreError::kConflict) {
        auto refreshed = store.ReadState();
        if (refreshed.has_value() && refreshed->initialized) {
          auto document = ParseWorkspaceSyncDocument(
              base::as_string_view(base::span(refreshed->data)),
              snapshot.capabilities);
          if (document.has_value()) {
            return Conflict(conflict_local ? *conflict_local : local, *document,
                            *refreshed, candidate_mapping);
          }
        }
      }
      return Reply("error", StoreError(committed.error()));
    }
    return PrepareApply(snapshot, local, *committed, candidate_mapping);
  }

  WorkspaceSyncReply PrepareApply(
      const WorkspaceSyncSnapshot& local,
      const WorkspaceSyncDocument& remote,
      const noventra_sync::SyncVaultState& head,
      const WorkspaceSyncMapping& candidate_mapping) {
    auto capacity = ValidateWorkspaceSyncApplyCapacity(
        candidate_mapping, ignored_, local.local.spaces, remote.spaces);
    if (!capacity.has_value()) {
      return Reply("error", capacity.error());
    }
    if (!WorkspaceSyncLayoutsEqual(local.local.layout, remote.layout) &&
        local.local.layout.revision == std::numeric_limits<int>::max()) {
      return Reply("error", "sync_layout_revision_exhausted");
    }
    auto admitted_mapping = candidate_mapping;
    // Exclusions may be dormant; they must also prevent accidental ID reuse.
    std::set<std::string> used = ignored_;
    for (const auto& space : local.local.spaces) {
      used.insert(space.id);
    }
    for (const auto& [logical, physical] : admitted_mapping) {
      used.insert(physical);
    }
    for (const auto& space : remote.spaces) {
      if (admitted_mapping.contains(space.id)) {
        continue;
      }
      std::string local_id;
      do {
        local_id = base::Uuid::GenerateRandomV4().AsLowercaseString();
      } while (used.contains(local_id));
      used.insert(local_id);
      admitted_mapping.emplace(space.id, std::move(local_id));
    }
    mapping_ = std::move(admitted_mapping);
    auto pending = HeadIdentity(head);
    pending.Set("document", WorkspaceSyncDocumentToValue(remote));
    pending.Set("before", local.prefs.Clone());
    pending.Set("after", MakeApplyPrefs(local, remote, mapping_));
    state_.Set("pending", std::move(pending));
    state_.Set("apply_staged", true);
    if (!Save()) {
      return Reply("error", "sync_storage_error");
    }
    return PendingReply(*state_.FindDict("pending"), local.capabilities);
  }

  WorkspaceSyncReply PendingReply(
      const base::DictValue& pending,
      const std::vector<LayoutCapability>& capabilities) const {
    auto capacity = ValidateWorkspaceSyncIdentityCapacity(mapping_, ignored_);
    if (!capacity.has_value()) {
      return Reply("error", capacity.error());
    }
    const auto* document = pending.FindDict("document");
    const auto* before = pending.FindDict("before");
    const auto* after = pending.FindDict("after");
    if (!document || !before || !after) {
      return Reply("error", "sync_protected_state_unavailable");
    }
    auto parsed = ValidateWorkspaceSyncDocument(*document, capabilities);
    if (!parsed.has_value()) {
      return Reply("error", parsed.error());
    }
    for (const auto& space : parsed->spaces) {
      if (!mapping_.contains(space.id)) {
        return Reply("error", "invalid_workspace_mapping");
      }
    }
    for (const auto* values : {before, after}) {
      if (!values->FindDict(kSpacesPref) ||
          !values->FindList(kSpaceOrderPref) ||
          !values->FindList(kArchivedSpacesPref) ||
          !values->FindDict(kDockLayoutPref)) {
        return Reply("error", "sync_protected_state_unavailable");
      }
    }
    auto after_layout = ValidateLayout(
        base::Value(after->FindDict(kDockLayoutPref)->Clone()), capabilities);
    if (!after_layout.has_value() ||
        !WorkspaceSyncLayoutsEqual(*after_layout, parsed->layout)) {
      return Reply("error", "sync_protected_state_unavailable");
    }
    for (const auto& space : parsed->spaces) {
      const auto& local_id = mapping_.at(space.id);
      const auto* name = after->FindDict(kSpacesPref)->FindString(local_id);
      if (!name || *name != space.name ||
          after->FindList(kArchivedSpacesPref)->contains(local_id) !=
              space.archived ||
          !after->FindList(kSpaceOrderPref)->contains(local_id)) {
        return Reply("error", "sync_protected_state_unavailable");
      }
    }
    auto reply = Reply("syncing");
    reply.apply = WorkspaceSyncApply{std::move(*parsed), mapping_,
                                     before->Clone(), after->Clone()};
    return reply;
  }

  const base::FilePath profile_path_;
  const base::FilePath path_;
  const scoped_refptr<os_crypt_async::Encryptor> encryptor_;
  const scoped_refptr<WorkspaceSyncNetwork> network_;
  std::shared_ptr<syncer::CancelationSignal> cancelation_;
  base::DictValue state_;
  WorkspaceSyncMapping mapping_;
  std::set<std::string> ignored_;
  std::optional<noventra_sync::SyncVaultCheckpoint> checkpoint_;
  std::unique_ptr<noventra_sync::SyncVaultStore> store_;
  raw_ptr<WorkspaceSyncTransport> transport_ = nullptr;
  bool file_present_ = false;
  bool loaded_ = false;
  bool storage_failed_ = false;
};

class WorkspaceSyncServiceFactory final : public ProfileKeyedServiceFactory {
 public:
  static WorkspaceSyncServiceFactory* GetInstance() {
    static base::NoDestructor<WorkspaceSyncServiceFactory> instance;
    return instance.get();
  }

  static WorkspaceSyncService* GetForProfile(Profile* profile) {
    return static_cast<WorkspaceSyncService*>(
        GetInstance()->GetServiceForBrowserContext(profile, true));
  }

 private:
  friend class base::NoDestructor<WorkspaceSyncServiceFactory>;
  WorkspaceSyncServiceFactory()
      : ProfileKeyedServiceFactory(
            "NoventraWorkspaceSync",
            ProfileSelections::Builder()
                .WithRegular(ProfileSelection::kOriginalOnly)
                .WithGuest(ProfileSelection::kNone)
                .WithSystem(ProfileSelection::kNone)
                .WithAshInternals(ProfileSelection::kNone)
                .Build()) {}
  ~WorkspaceSyncServiceFactory() override = default;
  bool ServiceIsCreatedWithBrowserContext() const override { return true; }
  bool ServiceIsNULLWhileTesting() const override { return true; }
  std::unique_ptr<KeyedService> BuildServiceInstanceForBrowserContext(
      content::BrowserContext* context) const override {
    auto* profile = Profile::FromBrowserContext(context);
    if (!profile->IsRegularProfile() || profile->IsOffTheRecord()) {
      return nullptr;
    }
    return std::unique_ptr<KeyedService>(new WorkspaceSyncService(profile));
  }
};

// static
WorkspaceSyncService* WorkspaceSyncService::GetForProfile(Profile* profile) {
  if (!profile || !profile->IsRegularProfile() || profile->IsOffTheRecord()) {
    return nullptr;
  }
  return WorkspaceSyncServiceFactory::GetForProfile(profile);
}
// static
void WorkspaceSyncService::EnsureFactoryBuilt() {
  WorkspaceSyncServiceFactory::GetInstance();
}

WorkspaceSyncService::WorkspaceSyncService(Profile* profile)
    : profile_(profile),
      cancelation_(std::make_shared<syncer::CancelationSignal>()),
      status_(EmptyStatus()) {
  status_.Set("state", "initializing");
  prefs_registrar_.Init(profile_->GetPrefs());
  for (const auto* key : kModelPrefs) {
    prefs_registrar_.Add(
        key, base::BindRepeating(&WorkspaceSyncService::OnLocalChange,
                                 weak_factory_.GetWeakPtr()));
  }
  // Merely launching an unconfigured browser must not prompt for its keyring.
  base::ThreadPool::PostTaskAndReplyWithResult(
      FROM_HERE, {base::MayBlock(), base::TaskPriority::USER_VISIBLE},
      base::BindOnce(&base::PathExists,
                     profile_->GetPath().AppendASCII(kStateFilename)),
      base::BindOnce(&WorkspaceSyncService::OnStateFileFound,
                     weak_factory_.GetWeakPtr()));
}
WorkspaceSyncService::~WorkspaceSyncService() {
  Shutdown();
}

void WorkspaceSyncService::OnStateFileFound(bool exists) {
  if (shutdown_ || disconnecting_ || operation_ != 0) {
    return;
  }
  if (!exists) {
    ready_ = true;
    status_ = EmptyStatus();
    return;
  }
  status_.Set("configured", true);
  RequestEncryptor();
}

void WorkspaceSyncService::RequestEncryptor() {
  ready_ = false;
  const uint64_t epoch = ++initialization_epoch_;
  initialization_timer_.Start(
      FROM_HERE, base::Seconds(30),
      base::BindOnce(
          [](base::WeakPtr<WorkspaceSyncService> self, uint64_t epoch) {
            if (!self || self->initialization_epoch_ != epoch) {
              return;
            }
            ++self->initialization_epoch_;
            self->ready_ = true;
            self->setup_action_.clear();
            self->setup_payload_.clear();
            WorkspaceSyncReply reply{self->Status(), std::nullopt, {}};
            reply.status.Set("state", "error");
            reply.status.Set("error", "sync_key_storage_unavailable");
            self->Finish(std::move(reply));
          },
          weak_factory_.GetWeakPtr(), epoch));
  if (!g_browser_process->os_crypt_async()) {
    OnEncryptor(epoch, nullptr);
    return;
  }
  g_browser_process->os_crypt_async()->GetInstance(base::BindOnce(
      &WorkspaceSyncService::OnEncryptor, weak_factory_.GetWeakPtr(), epoch));
}

void WorkspaceSyncService::OnEncryptor(
    uint64_t epoch,
    scoped_refptr<os_crypt_async::Encryptor> encryptor) {
  if (shutdown_ || epoch != initialization_epoch_) {
    return;
  }
  initialization_timer_.Stop();
  if (!encryptor || !encryptor->IsEncryptionAvailable()) {
    ready_ = true;
    setup_action_.clear();
    setup_payload_.clear();
    WorkspaceSyncReply reply{Status(), std::nullopt, {}};
    reply.status.Set("state", "error");
    reply.status.Set("error", "sync_key_storage_unavailable");
    Finish(std::move(reply));
    return;
  }
  worker_.emplace(base::ThreadPool::CreateSequencedTaskRunner(
                      {base::MayBlock(), base::WithBaseSyncPrimitives(),
                       base::TaskPriority::USER_VISIBLE,
                       base::TaskShutdownBehavior::CONTINUE_ON_SHUTDOWN}),
                  profile_->GetPath(), std::move(encryptor),
                  base::MakeRefCounted<WorkspaceSyncNetwork>(
                      profile_->GetURLLoaderFactory()),
                  cancelation_);
  worker_.AsyncCall(&WorkspaceSyncWorker::Load)
      .Then(base::BindOnce(&WorkspaceSyncService::OnLoaded,
                           weak_factory_.GetWeakPtr(), epoch));
}

void WorkspaceSyncService::OnLoaded(uint64_t epoch, WorkspaceSyncReply reply) {
  if (shutdown_ || epoch != initialization_epoch_) {
    return;
  }
  ready_ = true;
  status_ = reply.status.Clone();
  if (const auto* error = status_.FindString("error");
      error && !error->empty()) {
    setup_action_.clear();
    setup_payload_.clear();
    if (!status_.FindBool("configured").value_or(false)) {
      worker_.Reset();
    }
    Finish(std::move(reply));
    return;
  }
  if (!setup_action_.empty()) {
    auto action = std::exchange(setup_action_, {});
    auto payload = std::move(setup_payload_);
    auto callback = std::move(callback_);
    busy_ = false;
    if ((action == "create" || action == "join") &&
        status_.FindBool("configured").value_or(false)) {
      if (callback) {
        std::move(callback).Run(base::unexpected("sync_already_configured"));
      }
      return;
    }
    Start(FindBrowser(), action, payload, std::move(callback));
    return;
  }
  if (status_.FindBool("configured").value_or(false)) {
    poll_timer_.Start(FROM_HERE, kPollInterval, this,
                      &WorkspaceSyncService::Poll);
    Poll();
  }
}

base::DictValue WorkspaceSyncService::Status() const {
  auto result = status_.Clone();
  result.Set("syncing", busy_);
  result.Set("dirty", dirty_);
  return result;
}

BrowserWindowInterface* WorkspaceSyncService::FindBrowser() const {
  BrowserWindowInterface* found = nullptr;
  GlobalBrowserCollection::GetInstance()->ForEach(
      [&](BrowserWindowInterface* browser) {
        if (browser->GetProfile() == profile_ &&
            !browser->IsDeleteScheduled() && browser->GetTabStripModel() &&
            !browser->GetTabStripModel()->closing_all()) {
          found = browser;
          return false;
        }
        return true;
      });
  return found;
}

void WorkspaceSyncService::Handle(BrowserWindowInterface* browser,
                                  const std::string& action,
                                  const base::DictValue& payload,
                                  Callback callback) {
  if (shutdown_ || profile_->IsOffTheRecord() ||
      !profile_->IsRegularProfile() ||
      (browser && browser->GetProfile() != profile_)) {
    std::move(callback).Run(base::unexpected("sync_profile_unavailable"));
    return;
  }
  if (action == "status") {
    std::move(callback).Run(Status());
    return;
  }
  if (action == "disconnect") {
    if (disconnecting_) {
      std::move(callback).Run(base::unexpected("sync_busy"));
      return;
    }
    ++initialization_epoch_;
    initialization_timer_.Stop();
    setup_action_.clear();
    setup_payload_.clear();
    poll_timer_.Stop();
    debounce_timer_.Stop();
    SignalCancelationOnce(*cancelation_);
    if (callback_) {
      std::move(callback_).Run(base::unexpected("sync_aborted"));
    }
    callback_ = std::move(callback);
    recovery_once_.clear();
    busy_ = true;
    disconnecting_ = true;
    const auto operation = ++operation_;
    status_.Set("state", "disconnecting");
    if (worker_.is_null()) {
      base::ThreadPool::PostTaskAndReplyWithResult(
          FROM_HERE, {base::MayBlock(), base::TaskPriority::USER_VISIBLE},
          base::BindOnce(&DeleteStateFile,
                         profile_->GetPath().AppendASCII(kStateFilename)),
          base::BindOnce(
              [](base::WeakPtr<WorkspaceSyncService> self, uint64_t operation,
                 bool success) {
                if (!self || self->operation_ != operation) {
                  return;
                }
                auto status = success ? EmptyStatus() : self->Status();
                if (!success) {
                  status.Set("state", "error");
                  status.Set("error", "sync_storage_error");
                }
                self->OnDisconnected(operation,
                                     {std::move(status), std::nullopt, {}});
              },
              weak_factory_.GetWeakPtr(), operation));
    } else {
      worker_.AsyncCall(&WorkspaceSyncWorker::Disconnect)
          .Then(base::BindOnce(&WorkspaceSyncService::OnDisconnected,
                               weak_factory_.GetWeakPtr(), operation));
    }
    return;
  }
  if (!ready_) {
    std::move(callback).Run(base::unexpected("sync_initializing"));
    return;
  }
  if (busy_) {
    std::move(callback).Run(base::unexpected("sync_busy"));
    return;
  }
  if (action != "create" && action != "join" && action != "sync" &&
      action != "use-local" && action != "use-remote") {
    std::move(callback).Run(base::unexpected("invalid_sync_action"));
    return;
  }
  const bool setup = action == "create" || action == "join";
  if (setup == status_.FindBool("configured").value_or(false)) {
    std::move(callback).Run(base::unexpected(setup ? "sync_already_configured"
                                                   : "sync_not_configured"));
    return;
  }
  if (worker_.is_null()) {
    if (!setup) {
      // Retry acquiring the OS key for an existing locked configuration.
      callback_ = std::move(callback);
      setup_action_ = action;
      busy_ = true;
      RequestEncryptor();
      return;
    }
    const auto* origin = payload.FindString("origin");
    const auto* token = payload.FindString("token");
    if (!origin || !ValidateWorkspaceSyncOrigin(*origin).has_value() ||
        !token || !IsWorkspaceSyncToken(*token)) {
      std::move(callback).Run(base::unexpected("invalid_sync_configuration"));
      return;
    }
    callback_ = std::move(callback);
    setup_action_ = action;
    setup_payload_ = payload.Clone();
    busy_ = true;
    status_.Set("state", "initializing");
    RequestEncryptor();
    return;
  }
  Start(browser ? browser : FindBrowser(), action, payload,
        std::move(callback));
}

void WorkspaceSyncService::Start(BrowserWindowInterface* browser,
                                 const std::string& action,
                                 const base::DictValue& payload,
                                 Callback callback) {
  // SpaceIds and layout migration can emit pref callbacks. Snapshot preparation
  // is not a committed user edit and must not keep retriggering the debounce.
  applying_ = true;
  auto snapshot = Capture(profile_, browser);
  applying_ = false;
  if (!snapshot.has_value()) {
    status_.Set("state", snapshot.error() == "sync_awaiting_window"
                             ? "awaiting_window"
                             : "error");
    status_.Set("error", snapshot.error());
    if (callback) {
      std::move(callback).Run(base::unexpected(snapshot.error()));
    }
    return;
  }
  busy_ = true;
  dirty_ = false;
  status_.Set("state", "syncing");
  status_.Set("error", "");
  callback_ = std::move(callback);
  const uint64_t operation = ++operation_;
  if (action == "create" || action == "join") {
    worker_.AsyncCall(&WorkspaceSyncWorker::Configure)
        .WithArgs(action == "create", payload.Clone(), std::move(*snapshot))
        .Then(base::BindOnce(&WorkspaceSyncService::OnReply,
                             weak_factory_.GetWeakPtr(), operation));
  } else {
    worker_.AsyncCall(&WorkspaceSyncWorker::Run)
        .WithArgs(action, std::move(*snapshot))
        .Then(base::BindOnce(&WorkspaceSyncService::OnReply,
                             weak_factory_.GetWeakPtr(), operation));
  }
}

void WorkspaceSyncService::OnReply(uint64_t operation,
                                   WorkspaceSyncReply reply) {
  if (shutdown_ || operation != operation_) {
    return;
  }
  if (!reply.recovery_key.empty()) {
    recovery_once_ = std::move(reply.recovery_key);
  }
  status_ = reply.status.Clone();
  if (!reply.apply) {
    Finish(std::move(reply));
    return;
  }
  auto* browser = FindBrowser();
  if (!browser) {
    // Keep the durable apply intent and mapping. A future window resumes it;
    // neither an absent window nor network errors ever reset the vault.
    reply.status.Set("state", "awaiting_window");
    reply.status.Set("error", "sync_awaiting_window");
    reply.apply.reset();
    Finish(std::move(reply));
    return;
  }
  auto& prefs = *profile_->GetPrefs();
  auto& plan = *reply.apply;
  auto checked =
      ValidateWorkspaceSyncDocument(WorkspaceSyncDocumentToValue(plan.document),
                                    GetLayoutCapabilities(prefs));
  std::string error;
  if (!CanWriteModel(profile_) || !checked.has_value()) {
    error = "workspace_settings_managed";
  } else if (!CanReplay(ReadModelPrefs(prefs), plan.before, plan.after)) {
    error = "sync_local_changed";
  }
  if (!error.empty()) {
    worker_.AsyncCall(&WorkspaceSyncWorker::RejectApply)
        .WithArgs(error)
        .Then(base::BindOnce(&WorkspaceSyncService::OnReply,
                             weak_factory_.GetWeakPtr(), operation));
    return;
  }
  // Everything, including policy and the complete layout, has been checked
  // before the first model write. Mapping was durably reserved by the worker.
  applying_ = true;
  auto current_layout = ReadOrMigrateLayout(prefs);
  if (!WorkspaceSyncLayoutsEqual(current_layout, plan.document.layout)) {
    auto layout = plan.document.layout;
    layout.revision = current_layout.revision;
    auto result = ApplyLayout(prefs, layout, current_layout.revision);
    if (result.status != "ok") {
      error = "sync_layout_apply_failed";
    }
  }
  if (error.empty()) {
    const auto& names = *plan.after.FindDict(kSpacesPref);
    const auto& order = *plan.after.FindList(kSpaceOrderPref);
    prefs.SetDict(kSpacesPref, names.Clone());
    prefs.SetList(kSpaceOrderPref, order.Clone());
    for (const auto& space : plan.document.spaces) {
      const auto& id = plan.mapping.at(space.id);
      if (id.empty()) {
        continue;
      }
      bool active_archived = false;
      if (space.archived) {
        GlobalBrowserCollection::GetInstance()->ForEach(
            [&](BrowserWindowInterface* candidate) {
              if (candidate->GetProfile()->GetOriginalProfile() == profile_ &&
                  candidate->GetTabStripModel() &&
                  SpaceId(
                      candidate->GetTabStripModel()->GetActiveWebContents()) ==
                      id) {
                active_archived = true;
              }
              return true;
            });
      }
      if (IsSpaceArchived(profile_, id) == space.archived && !active_archived) {
        continue;
      }
      if (!SetSpaceArchived(browser, id, space.archived)) {
        error = "sync_workspace_apply_failed";
        break;
      }
    }
  }
  if (error.empty()) {
    // Persist effective empty defaults too: a missing disk entry must not be
    // mistaken for proof that a non-default write completed.
    prefs.SetList(kArchivedSpacesPref,
                  plan.after.FindList(kArchivedSpacesPref)->Clone());
    prefs.SetDict(kDockLayoutPref, prefs.GetDict(kDockLayoutPref).Clone());
  }
  applying_ = false;
  if (!error.empty()) {
    worker_.AsyncCall(&WorkspaceSyncWorker::RejectApply)
        .WithArgs(error)
        .Then(base::BindOnce(&WorkspaceSyncService::OnReply,
                             weak_factory_.GetWeakPtr(), operation));
    return;
  }
  // Native ApplyLayout advances only the local edit revision and its existing
  // pref observer delivers noventra-layout-changed to any open editor.
  prefs.CommitPendingWrite(base::BindOnce(&WorkspaceSyncService::OnPrefsWritten,
                                          weak_factory_.GetWeakPtr(), operation,
                                          ReadModelPrefs(prefs)));
}

void WorkspaceSyncService::OnPrefsWritten(uint64_t operation,
                                          base::DictValue expected) {
  if (shutdown_ || operation != operation_) {
    return;
  }
  worker_.AsyncCall(&WorkspaceSyncWorker::FinishApply)
      .WithArgs(std::move(expected))
      .Then(base::BindOnce(&WorkspaceSyncService::OnReply,
                           weak_factory_.GetWeakPtr(), operation));
}

void WorkspaceSyncService::OnDisconnected(uint64_t operation,
                                          WorkspaceSyncReply reply) {
  if (shutdown_ || operation != operation_) {
    return;
  }
  disconnecting_ = false;
  ready_ = true;
  dirty_ = false;
  if (!reply.status.FindBool("configured").value_or(false)) {
    worker_.Reset();
    cancelation_ = std::make_shared<syncer::CancelationSignal>();
  }
  Finish(std::move(reply));
}

void WorkspaceSyncService::Finish(WorkspaceSyncReply reply) {
  busy_ = false;
  status_ = std::move(reply.status);
  auto result = Status();
  const bool has_recovery_key = !recovery_once_.empty();
  if (has_recovery_key) {
    result.Set("recovery_key", std::exchange(recovery_once_, {}));
  }
  auto callback = std::move(callback_);
  if (status_.FindBool("configured").value_or(false) &&
      !cancelation_->IsSignalled()) {
    if (!poll_timer_.IsRunning()) {
      poll_timer_.Start(FROM_HERE, kPollInterval, this,
                        &WorkspaceSyncService::Poll);
    }
    if (dirty_) {
      debounce_timer_.Start(FROM_HERE, base::Seconds(2), this,
                            &WorkspaceSyncService::Poll);
    }
  }
  if (!status_.FindBool("configured").value_or(false)) {
    poll_timer_.Stop();
    debounce_timer_.Stop();
  }
  if (callback) {
    const auto* error = status_.FindString("error");
    if (error && !error->empty() && !has_recovery_key) {
      std::move(callback).Run(base::unexpected(*error));
    } else {
      std::move(callback).Run(std::move(result));
    }
  }
}

void WorkspaceSyncService::OnLocalChange() {
  if (shutdown_ || applying_ ||
      !status_.FindBool("configured").value_or(false)) {
    return;
  }
  dirty_ = true;
  if (!busy_) {
    debounce_timer_.Start(FROM_HERE, base::Seconds(2), this,
                          &WorkspaceSyncService::Poll);
  }
}
void WorkspaceSyncService::Poll() {
  if (shutdown_ || !ready_ || busy_ || worker_.is_null() ||
      !status_.FindBool("configured").value_or(false) ||
      cancelation_->IsSignalled()) {
    return;
  }
  Start(FindBrowser(), "sync", base::DictValue(), Callback());
}
void WorkspaceSyncService::Shutdown() {
  if (shutdown_) {
    return;
  }
  shutdown_ = true;
  SignalCancelationOnce(*cancelation_);
  poll_timer_.Stop();
  debounce_timer_.Stop();
  initialization_timer_.Stop();
  prefs_registrar_.RemoveAll();
  weak_factory_.InvalidateWeakPtrs();
  if (callback_) {
    std::move(callback_).Run(base::unexpected("sync_aborted"));
  }
  callback_.Reset();
  setup_payload_.clear();
  recovery_once_.clear();
  worker_.Reset();
}

// static
void WorkspaceSyncService::SignalCancelationOnce(
    syncer::CancelationSignal& signal) {
  if (!signal.IsSignalled()) {
    signal.Signal();
  }
}
}  // namespace noventra
