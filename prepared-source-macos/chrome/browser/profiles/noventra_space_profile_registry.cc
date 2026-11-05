// Copyright 2026 The Noventra Authors. All rights reserved.
#include "chrome/browser/profiles/noventra_space_profile_registry.h"

#include <limits>
#include <map>
#include <memory>
#include <set>
#include <utility>

#include "base/command_line.h"
#include "base/files/file_util.h"
#include "base/files/important_file_writer.h"
#include "base/files/scoped_temp_dir.h"
#include "base/functional/bind.h"
#include "base/functional/callback.h"
#include "base/json/json_reader.h"
#include "base/json/json_writer.h"
#include "base/memory/weak_ptr.h"
#include "base/no_destructor.h"
#include "base/run_loop.h"
#include "base/scoped_observation.h"
#include "base/supports_user_data.h"
#include "base/task/single_thread_task_runner.h"
#include "base/task/thread_pool.h"
#include "base/uuid.h"
#include "base/values.h"
#include "build/build_config.h"
#include "chrome/browser/browser_process.h"
#include "chrome/browser/profiles/keep_alive/profile_keep_alive_types.h"
#include "chrome/browser/profiles/keep_alive/scoped_profile_keep_alive.h"
#include "chrome/browser/profiles/noventra_account_spaces.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/profiles/profile_manager.h"
#include "chrome/browser/profiles/profile_manager_observer.h"
#include "components/prefs/pref_registry_simple.h"
#include "components/prefs/pref_service.h"
#include "content/public/browser/browser_thread.h"

namespace noventra {
namespace {

struct Binding {
  base::FilePath directory;
  SpaceProfileStorage storage;
  bool ui_seeded = false;
  bool initialization_started = false;
};

struct Catalog {
  int generation = 0;
  std::map<base::FilePath, std::map<std::string, Binding>> groups;
};

constexpr char kCatalogProfileLeaseKey[] = "noventra.catalog_profile_lease";

class CatalogProfileLease : public base::SupportsUserData::Data {
 public:
  explicit CatalogProfileLease(
      std::unique_ptr<ScopedProfileKeepAlive> keep_alive)
      : keep_alive_(std::move(keep_alive)) {}

  Profile* root() const { return keep_alive_->profile(); }

 private:
  // Profile UserData outlives its keyed-service shutdown, including observers
  // that still remove subscriptions from the catalog's PrefService.
  std::unique_ptr<ScopedProfileKeepAlive> keep_alive_;
};

bool IsProfileBasename(const base::FilePath& name) {
  return !name.empty() && !name.IsAbsolute() && !name.ReferencesParent() &&
         name == name.BaseName() &&
         name != base::FilePath(FILE_PATH_LITERAL(".")) &&
         name != base::FilePath(FILE_PATH_LITERAL("System Profile")) &&
         name != base::FilePath(FILE_PATH_LITERAL("Guest Profile"));
}

PrefService* LocalState() {
  auto* prefs = g_browser_process ? g_browser_process->local_state() : nullptr;
  return prefs && prefs->FindPreference(kSpaceProfilesLocalState) ? prefs
                                                                  : nullptr;
}

std::optional<Catalog> ParseCatalog(const base::DictValue& value) {
  if (value.empty()) {
    return Catalog();
  }
  if (value.FindInt("version") != 1) {
    return std::nullopt;
  }
  auto generation = value.FindInt("generation");
  const auto* groups = value.FindDict("groups");
  if (!generation || *generation < 1 || !groups) {
    return std::nullopt;
  }
  Catalog result;
  result.generation = *generation;
  std::set<base::FilePath> children;
  for (const auto [root_name, group_value] : *groups) {
    const auto root = base::FilePath::FromUTF8Unsafe(root_name);
    const auto* group = group_value.GetIfDict();
    const auto* spaces = group ? group->FindDict("spaces") : nullptr;
    if (!IsProfileBasename(root) || !spaces) {
      return std::nullopt;
    }
    auto& target_group = result.groups[root];
    for (const auto [id, space_value] : *spaces) {
      const auto* space = space_value.GetIfDict();
      const auto* directory = space ? space->FindString("directory") : nullptr;
      const auto* storage = space ? space->FindString("storage") : nullptr;
      if (!base::Uuid::ParseLowercase(id).is_valid() || !directory ||
          !storage) {
        return std::nullopt;
      }
      const auto child = base::FilePath::FromUTF8Unsafe(*directory);
      if (!IsProfileBasename(child) || child == root ||
          !children.insert(child).second) {
        return std::nullopt;
      }
      SpaceProfileStorage layout;
      if (*storage == "default") {
        layout = SpaceProfileStorage::kDefault;
      } else if (*storage == "legacy_partition") {
        layout = SpaceProfileStorage::kLegacyPartition;
      } else {
        return std::nullopt;
      }
      const auto seeded = space->FindBool("ui_seeded");
      const auto started = space->FindBool("initialization_started");
      if ((space->contains("ui_seeded") && !seeded) ||
          (space->contains("initialization_started") && !started)) {
        return std::nullopt;
      }
      target_group.emplace(id,
                           Binding{child, layout, seeded.value_or(false),
                                   started.value_or(seeded.value_or(false))});
    }
  }
  for (const auto& [root, group] : result.groups) {
    if (children.contains(root)) {
      return std::nullopt;
    }
  }
  return result;
}

std::optional<Catalog> ReadCatalog() {
  const auto* prefs = LocalState();
  return prefs ? ParseCatalog(prefs->GetDict(kSpaceProfilesLocalState))
               : std::nullopt;
}

base::DictValue CatalogValue(const Catalog& catalog) {
  base::DictValue groups;
  for (const auto& [root, bindings] : catalog.groups) {
    base::DictValue spaces;
    for (const auto& [id, binding] : bindings) {
      base::DictValue entry;
      entry.Set("directory", binding.directory.AsUTF8Unsafe());
      entry.Set("storage", binding.storage == SpaceProfileStorage::kDefault
                               ? "default"
                               : "legacy_partition");
      entry.Set("ui_seeded", binding.ui_seeded);
      entry.Set("initialization_started", binding.initialization_started);
      spaces.Set(id, std::move(entry));
    }
    base::DictValue group;
    group.Set("spaces", std::move(spaces));
    groups.Set(root.AsUTF8Unsafe(), std::move(group));
  }
  base::DictValue result;
  result.Set("version", 1);
  result.Set("generation", catalog.generation);
  result.Set("groups", std::move(groups));
  return result;
}

std::optional<int> WriteCatalog(Catalog catalog) {
  auto* prefs = LocalState();
  if (!prefs || prefs->IsManagedPreference(kSpaceProfilesLocalState) ||
      catalog.generation == std::numeric_limits<int>::max()) {
    return std::nullopt;
  }
  ++catalog.generation;
  prefs->SetDict(kSpaceProfilesLocalState, CatalogValue(catalog));
  return catalog.generation;
}

struct Identity {
  base::FilePath root;
  std::string id;
  Binding binding;
};

void SeedUserInterface(Profile* source, Profile* target) {
  constexpr const char* kPreferences[] = {
      "helium.browser.layout",
      "helium.browser.vertical_right_aligned",
      "helium.browser.zen_mode",
      "helium.browser.zen_mode_sidebar_pinned",
      "helium.browser.zen_mode_top_chrome_pinned",
      "helium.browser.centered_location_bar",
      "helium.browser.minimal_location_bar",
      "vertical_tabs.collapsed_state",
      "vertical_tabs.uncollapsed_width",
      "vertical_tabs.expand_on_hover",
      "noventra.browser.dock_layout",
      "noventra.browser.tab_density",
      "noventra.browser.glass_enabled",
      "noventra.browser.sidebar_controls",
      "noventra.browser.address_on_demand",
      "noventra.browser.show_navigation_buttons",
  };
  auto* source_prefs = source->GetPrefs();
  auto* target_prefs = target->GetPrefs();
  for (const auto* key : kPreferences) {
    const auto* source_pref = source_prefs->FindPreference(key);
    const auto* target_pref = target_prefs->FindPreference(key);
    if (source_pref && target_pref && target_pref->IsUserModifiable() &&
        !target_prefs->HasPrefPath(key)) {
      target_prefs->Set(key, *source_pref->GetValue());
    }
  }
}

std::optional<Identity> FindIdentity(const Catalog& catalog, Profile* profile) {
  if (!profile) {
    return std::nullopt;
  }
  auto* manager =
      g_browser_process ? g_browser_process->profile_manager() : nullptr;
  const auto path = profile->GetOriginalProfile()->GetPath();
  if (!manager || path.DirName() != manager->user_data_dir()) {
    return std::nullopt;
  }
  for (const auto& [root, group] : catalog.groups) {
    for (const auto& [id, binding] : group) {
      if (binding.directory == path.BaseName()) {
        return Identity{root, id, binding};
      }
    }
  }
  return std::nullopt;
}

constexpr char kProfileIdentityMarker[] = "NoventraSpaceProfile.json";

std::optional<Identity> IdentityForPath(const Catalog& catalog,
                                        const base::FilePath& user_data,
                                        const base::FilePath& path) {
  if (path.DirName() != user_data) {
    return std::nullopt;
  }
  for (const auto& [root, group] : catalog.groups) {
    for (const auto& [id, binding] : group) {
      if (binding.directory == path.BaseName()) {
        return Identity{root, id, binding};
      }
    }
  }
  return std::nullopt;
}

bool SameBinding(const Identity& left, const Identity& right) {
  return left.root == right.root && left.id == right.id &&
         left.binding.directory == right.binding.directory &&
         left.binding.storage == right.binding.storage;
}

base::DictValue DefaultProfileMarker(const Identity& identity) {
  base::DictValue value;
  value.Set("version", 1);
  value.Set("root", identity.root.AsUTF8Unsafe());
  value.Set("space", identity.id);
  value.Set("directory", identity.binding.directory.AsUTF8Unsafe());
  return value;
}

std::optional<base::DictValue> ReadDictionary(const base::FilePath& path) {
  std::string data;
  if (base::IsLink(path) || !base::ReadFileToString(path, &data)) {
    return std::nullopt;
  }
  return base::JSONReader::ReadDict(data, base::JSON_PARSE_RFC);
}

bool IsOwnedDirectory(const base::FilePath& user_data,
                      const Identity& identity) {
  const auto path = user_data.Append(identity.binding.directory);
  if (base::IsLink(path) || !base::DirectoryExists(path)) {
    return false;
  }
  const bool legacy =
      identity.binding.storage == SpaceProfileStorage::kLegacyPartition;
  auto marker = ReadDictionary(path.AppendASCII(
      legacy ? "NoventraSpaceMigration.json" : kProfileIdentityMarker));
  if (!marker || marker->FindInt("version") != 1) {
    return false;
  }
  for (const auto [key, expected] : DefaultProfileMarker(identity)) {
    const auto* actual = marker->Find(key);
    if (!actual || *actual != expected) {
      return false;
    }
  }
  return true;
}

bool PrepareOwnedDirectory(const base::FilePath& user_data,
                           const Identity& identity) {
  const auto path = user_data.Append(identity.binding.directory);
  if (base::IsLink(path) || base::IsLink(user_data.Append(identity.root))) {
    return false;
  }
  if (base::PathExists(path)) {
    return IsOwnedDirectory(user_data, identity);
  }
  if (identity.binding.storage != SpaceProfileStorage::kDefault ||
      identity.binding.initialization_started || identity.binding.ui_seeded) {
    return false;
  }
  base::ScopedTempDir staging;
  if (!staging.CreateUniqueTempDirUnderPath(user_data)) {
    return false;
  }
  auto data = base::WriteJson(DefaultProfileMarker(identity));
  if (!data ||
      !base::ImportantFileWriter::WriteFileAtomically(
          staging.GetPath().AppendASCII(kProfileIdentityMarker), *data) ||
      base::PathExists(path) || !base::Move(staging.GetPath(), path)) {
    return false;
  }
  staging.Take();
  return IsOwnedDirectory(user_data, identity);
}

struct VerifiedBinding {
  bool valid = false;
  bool initialization_started = false;
};

VerifiedBinding VerifyPersistedBinding(const base::FilePath& user_data,
                                       Identity expected,
                                       bool require_initialization) {
  auto disk = ReadDictionary(user_data.AppendASCII("Local State"));
  const auto* value =
      disk ? disk->FindDictByDottedPath(kSpaceProfilesLocalState) : nullptr;
  auto catalog = value ? ParseCatalog(*value) : std::nullopt;
  auto actual =
      catalog ? IdentityForPath(*catalog, user_data,
                                user_data.Append(expected.binding.directory))
              : std::nullopt;
  if (!actual || !SameBinding(*actual, expected) ||
      (require_initialization && !actual->binding.initialization_started) ||
      !PrepareOwnedDirectory(user_data, *actual)) {
    return {};
  }
  return {true, actual->binding.initialization_started};
}

class SpaceProfileLoads final : public ProfileManagerObserver {
 public:
  using Callback =
      base::OnceCallback<void(bool, std::unique_ptr<ScopedProfileKeepAlive>)>;

  static SpaceProfileLoads& Get() {
    static base::NoDestructor<SpaceProfileLoads> loads;
    return *loads;
  }

  void Prepare(base::WeakPtr<ProfileManager> manager,
               Profile* root,
               const base::FilePath& path,
               Identity identity,
               Callback callback) {
    auto lease = root && !root->ShutdownStarted()
                     ? ScopedProfileKeepAlive::TryAcquire(
                           root, ProfileKeepAliveOrigin::kProfileCreationFlow)
                     : nullptr;
    if (!manager || !lease ||
        (observation_.IsObserving() &&
         observation_.GetSource() != manager.get())) {
      std::move(callback).Run(false, nullptr);
      return;
    }
    if (!observation_.IsObserving()) {
      observation_.Observe(manager.get());
    }
    auto [it, inserted] = pending_.try_emplace(path, identity);
    if (!SameBinding(it->second.identity, identity)) {
      std::move(callback).Run(false, nullptr);
      return;
    }
    it->second.requests.push_back(
        {manager, root->GetWeakPtr(), std::move(lease), std::move(callback)});
    if (inserted) {
      Flush(path);
    }
  }

  void OnProfileManagerDestroying() override {
    observation_.Reset();
    pending_.clear();
  }

 private:
  struct Request {
    base::WeakPtr<ProfileManager> manager;
    base::WeakPtr<Profile> root;
    std::unique_ptr<ScopedProfileKeepAlive> lease;
    Callback callback;
  };

  struct Pending {
    explicit Pending(Identity identity) : identity(std::move(identity)) {}
    Identity identity;
    bool verifying_initialization = false;
    std::vector<Request> requests;
  };

  void Flush(const base::FilePath& path) {
    if (!pending_.contains(path)) {
      return;
    }
    auto* prefs = LocalState();
    if (!prefs || (g_browser_process && g_browser_process->IsShuttingDown())) {
      Finish(path, false);
      return;
    }
    prefs->CommitPendingWrite(base::BindOnce(
        &SpaceProfileLoads::ReadPersistence, base::Unretained(this), path));
  }

  void ReadPersistence(const base::FilePath& path) {
    auto found = pending_.find(path);
    if (found == pending_.end() || found->second.requests.empty()) {
      return;
    }
    auto manager = found->second.requests.front().manager;
    if (!manager) {
      Finish(path, false);
      return;
    }
    if (!base::ThreadPool::PostTaskAndReplyWithResult(
            FROM_HERE,
            {base::MayBlock(), base::TaskPriority::USER_BLOCKING,
             base::TaskShutdownBehavior::SKIP_ON_SHUTDOWN},
            base::BindOnce(&VerifyPersistedBinding, manager->user_data_dir(),
                           found->second.identity,
                           found->second.verifying_initialization),
            base::BindOnce(&SpaceProfileLoads::Verified, base::Unretained(this),
                           path))) {
      Finish(path, false);
    }
  }

  void Verified(const base::FilePath& path, VerifiedBinding verified) {
    auto found = pending_.find(path);
    if (found == pending_.end()) {
      return;
    }
    auto catalog = ReadCatalog();
    auto manager = found->second.requests.front().manager;
    auto actual =
        catalog && manager
            ? IdentityForPath(*catalog, manager->user_data_dir(), path)
            : std::nullopt;
    if (!verified.valid || !actual ||
        !SameBinding(*actual, found->second.identity)) {
      Finish(path, false);
      return;
    }
    if (verified.initialization_started &&
        actual->binding.initialization_started) {
      Finish(path, true);
      return;
    }
    found->second.verifying_initialization = true;
    // Queue before notifying prefs observers, which can synchronously load
    // the same Profile and pump a nested native creation loop.
    base::SingleThreadTaskRunner::GetCurrentDefault()->PostTask(
        FROM_HERE, base::BindOnce(&SpaceProfileLoads::Flush,
                                  base::Unretained(this), path));
    if (!actual->binding.initialization_started) {
      catalog->groups[actual->root][actual->id].initialization_started = true;
      if (!WriteCatalog(std::move(*catalog))) {
        Finish(path, false);
      }
    }
  }

  void Finish(const base::FilePath& path, bool valid) {
    auto found = pending_.find(path);
    if (found == pending_.end()) {
      return;
    }
    auto requests = std::move(found->second.requests);
    pending_.erase(found);
    for (auto& request : requests) {
      const bool ready =
          valid && request.manager && request.root &&
          !request.root->ShutdownStarted() &&
          !(g_browser_process && g_browser_process->IsShuttingDown());
      std::move(request.callback)
          .Run(ready, ready ? std::move(request.lease) : nullptr);
    }
  }

  std::map<base::FilePath, Pending> pending_;
  base::ScopedObservation<ProfileManager, ProfileManagerObserver> observation_{
      this};
};

void SpaceProfileLoaded(base::WeakPtr<Profile> root,
                        std::unique_ptr<ScopedProfileKeepAlive> root_keep_alive,
                        std::string id,
                        base::FilePath path,
                        base::OnceCallback<void(Profile*)> callback,
                        Profile* profile) {
  auto keep_alive =
      profile && profile->IsRegularProfile() && !profile->ShutdownStarted() &&
              profile->GetPath() == path
          ? ScopedProfileKeepAlive::TryAcquire(
                profile, ProfileKeepAliveOrigin::kProfileCreationFlow)
          : nullptr;
  auto target = profile ? profile->GetWeakPtr() : base::WeakPtr<Profile>();
  auto catalog = ReadCatalog();
  auto identity = catalog && keep_alive && target
                      ? FindIdentity(*catalog, target.get())
                      : std::nullopt;
  bool valid = root_keep_alive && root && !root->ShutdownStarted() && target &&
               !target->ShutdownStarted() && identity && identity->id == id &&
               identity->root == root->GetPath().BaseName() &&
               identity->binding.initialization_started &&
               AttachSpaceCatalogProfileLease(target.get());
  if (valid && !identity->binding.ui_seeded) {
    catalog->groups[identity->root][identity->id].ui_seeded = true;
    valid = WriteCatalog(std::move(*catalog)).has_value();
    if (valid && root && target && !target->ShutdownStarted()) {
      SeedUserInterface(root.get(), target.get());
    }
  }
  valid = valid && root && !root->ShutdownStarted() && target &&
          !target->ShutdownStarted();
  std::move(callback).Run(valid ? target.get() : nullptr);
}

}  // namespace

bool ProfileSpacesEnabled() {
#if BUILDFLAG(IS_LINUX) && defined(USE_AURA) && !BUILDFLAG(IS_CHROMEOS)
  return !base::CommandLine::ForCurrentProcess()->HasSwitch(
      "disable-noventra-profile-spaces");
#else
  return false;
#endif
}

void RegisterSpaceProfileLocalState(PrefRegistrySimple* registry) {
  registry->RegisterDictionaryPref(kSpaceProfilesLocalState);
}

Profile* SpaceCatalogProfile(Profile* profile) {
  if (!profile) {
    return nullptr;
  }
  auto* original = profile->GetOriginalProfile();
  if (!ProfileSpacesEnabled()) {
    return original;
  }
  const auto catalog = ReadCatalog();
  if (!catalog) {
    return nullptr;
  }
  const auto identity = FindIdentity(*catalog, original);
  if (!identity) {
    return original;
  }
  auto* manager = g_browser_process->profile_manager();
  return manager->GetProfileByPath(
      manager->user_data_dir().Append(identity->root));
}

std::string ProfileSpaceId(Profile* profile) {
  if (!ProfileSpacesEnabled()) {
    return {};
  }
  const auto catalog = ReadCatalog();
  const auto identity =
      catalog ? FindIdentity(*catalog, profile) : std::nullopt;
  return identity ? identity->id : std::string();
}

bool IsProfileSpace(Profile* profile) {
  return !ProfileSpaceId(profile).empty();
}

std::vector<std::string> RegisteredSpaceIds(Profile* profile) {
  auto* root = SpaceCatalogProfile(profile);
  const auto catalog = ReadCatalog();
  if (!ProfileSpacesEnabled() || !root || !catalog) {
    return {};
  }
  const auto found = catalog->groups.find(root->GetPath().BaseName());
  std::vector<std::string> result;
  if (found != catalog->groups.end()) {
    for (const auto& [id, binding] : found->second) {
      result.push_back(id);
    }
  }
  return result;
}

std::optional<SpaceProfileStorage> ProfileSpaceStorage(Profile* profile) {
  if (!ProfileSpacesEnabled()) {
    return std::nullopt;
  }
  const auto catalog = ReadCatalog();
  const auto identity =
      catalog ? FindIdentity(*catalog, profile) : std::nullopt;
  return identity ? std::make_optional(identity->binding.storage)
                  : std::nullopt;
}

void AcquireSpaceProfile(Profile* source,
                         const std::string& id,
                         base::OnceCallback<void(Profile*)> callback) {
  DCHECK_CURRENTLY_ON(content::BrowserThread::UI);
  auto* root = ProfileSpacesEnabled() ? SpaceCatalogProfile(source) : nullptr;
  auto* manager =
      g_browser_process ? g_browser_process->profile_manager() : nullptr;
  auto catalog = ReadCatalog();
  if (!root || !manager || !catalog || !root->IsRegularProfile() ||
      root->ShutdownStarted() ||
      root->GetPath().DirName() != manager->user_data_dir()) {
    std::move(callback).Run(nullptr);
    return;
  }
  if (id.empty()) {
    auto keep_alive = ScopedProfileKeepAlive::TryAcquire(
        root, ProfileKeepAliveOrigin::kChromeViewsDelegate);
    std::move(callback).Run(keep_alive ? root : nullptr);
    return;
  }
  auto* prefs = root->GetPrefs();
  if (!base::Uuid::ParseLowercase(id).is_valid() ||
      !prefs->FindPreference(kSpacesPref)) {
    std::move(callback).Run(nullptr);
    return;
  }
  auto& group = catalog->groups[root->GetPath().BaseName()];
  auto binding = group.find(id);
  if (binding == group.end()) {
    if (!prefs->GetDict(kSpacesPref).FindString(id)) {
      std::move(callback).Run(nullptr);
      return;
    }
    const auto path = manager->GenerateNextProfileDirectoryPath();
    if (path.DirName() != manager->user_data_dir() ||
        !IsProfileBasename(path.BaseName())) {
      std::move(callback).Run(nullptr);
      return;
    }
    for (const auto& [existing_root, existing_group] : catalog->groups) {
      if (path.BaseName() == existing_root) {
        std::move(callback).Run(nullptr);
        return;
      }
      for (const auto& [existing_id, existing_binding] : existing_group) {
        if (path.BaseName() == existing_binding.directory) {
          std::move(callback).Run(nullptr);
          return;
        }
      }
    }
    binding = group
                  .emplace(id, Binding{path.BaseName(),
                                       SpaceProfileStorage::kDefault})
                  .first;
    if (!WriteCatalog(*catalog)) {
      std::move(callback).Run(nullptr);
      return;
    }
  }
  const auto path = manager->user_data_dir().Append(binding->second.directory);
  auto root_keep_alive = ScopedProfileKeepAlive::TryAcquire(
      root, ProfileKeepAliveOrigin::kChromeViewsDelegate);
  if (!root_keep_alive) {
    std::move(callback).Run(nullptr);
    return;
  }
  manager->CreateProfileAsync(
      path, base::BindOnce(&SpaceProfileLoaded, root->GetWeakPtr(),
                           std::move(root_keep_alive), id, path,
                           std::move(callback)));
}

std::optional<std::vector<SpaceProfileBindingForStartup>>
SpaceProfileBindingsForStartup() {
  const auto catalog = ReadCatalog();
  if (!catalog) {
    return std::nullopt;
  }
  std::vector<SpaceProfileBindingForStartup> result;
  for (const auto& [root, group] : catalog->groups) {
    for (const auto& [id, binding] : group) {
      result.push_back({root, id, binding.directory, binding.storage,
                        binding.ui_seeded, binding.initialization_started});
    }
  }
  return result;
}

bool IsSpaceProfileDirectoryOwned(
    const base::FilePath& user_data_directory,
    const SpaceProfileBindingForStartup& binding) {
  return IsOwnedDirectory(
      user_data_directory,
      Identity{binding.root_basename, binding.id,
               Binding{binding.profile_basename, binding.storage,
                       binding.ui_seeded, binding.initialization_started}});
}

bool AttachSpaceCatalogProfileLease(Profile* profile) {
  if (!ProfileSpacesEnabled()) {
    return true;
  }
  auto* root = SpaceCatalogProfile(profile);
  if (!root || !profile || profile->IsOffTheRecord()) {
    return false;
  }
  if (root == profile) {
    return true;
  }
  auto* lease = static_cast<CatalogProfileLease*>(
      profile->GetUserData(kCatalogProfileLeaseKey));
  if (lease) {
    return lease->root() == root;
  }
  auto keep_alive = ScopedProfileKeepAlive::TryAcquire(
      root, ProfileKeepAliveOrigin::kChromeViewsDelegate);
  if (!keep_alive) {
    return false;
  }
  profile->SetUserData(
      kCatalogProfileLeaseKey,
      std::make_unique<CatalogProfileLease>(std::move(keep_alive)));
  return true;
}

void PrepareSpaceProfileLoadAsync(
    base::WeakPtr<ProfileManager> manager,
    const base::FilePath& profile_path,
    base::OnceCallback<void(bool, std::unique_ptr<ScopedProfileKeepAlive>)>
        callback) {
  DCHECK_CURRENTLY_ON(content::BrowserThread::UI);
  if (!manager || (g_browser_process && g_browser_process->IsShuttingDown())) {
    std::move(callback).Run(false, nullptr);
    return;
  }
  if (!ProfileSpacesEnabled()) {
    std::move(callback).Run(true, nullptr);
    return;
  }
  auto catalog = ReadCatalog();
  if (!catalog) {
    std::move(callback).Run(false, nullptr);
    return;
  }
  auto identity =
      IdentityForPath(*catalog, manager->user_data_dir(), profile_path);
  if (!identity) {
    std::move(callback).Run(true, nullptr);
    return;
  }
  const auto root_path = manager->user_data_dir().Append(identity->root);
  auto ready = base::BindOnce(
      [](base::WeakPtr<ProfileManager> manager, base::FilePath path,
         Identity identity, SpaceProfileLoads::Callback callback,
         Profile* root) {
        if (!manager || !root || root->ShutdownStarted()) {
          std::move(callback).Run(false, nullptr);
          return;
        }
        SpaceProfileLoads::Get().Prepare(
            manager, root, path, std::move(identity), std::move(callback));
      },
      manager, profile_path, std::move(*identity), std::move(callback));
  if (auto* root = manager->GetProfileByPath(root_path)) {
    std::move(ready).Run(root);
  } else {
    manager->CreateProfileAsync(root_path, std::move(ready));
  }
}

bool PrepareSpaceProfileLoad(
    base::WeakPtr<ProfileManager> manager,
    const base::FilePath& profile_path,
    std::unique_ptr<ScopedProfileKeepAlive>* catalog_keep_alive) {
  DCHECK_CURRENTLY_ON(content::BrowserThread::UI);
  if (!ProfileSpacesEnabled()) {
    return true;
  }
  auto catalog = ReadCatalog();
  if (!manager || !catalog ||
      (g_browser_process && g_browser_process->IsShuttingDown())) {
    return false;
  }
  auto identity =
      IdentityForPath(*catalog, manager->user_data_dir(), profile_path);
  if (!identity) {
    return true;
  }
  // Synchronous startup can run before deferred async loads are released.
  // Resolve its parent through the same native synchronous entry point.
  auto* root =
      manager->GetProfile(manager->user_data_dir().Append(identity->root));
  auto scope = root && !root->ShutdownStarted()
                   ? ScopedProfileKeepAlive::TryAcquire(
                         root, ProfileKeepAliveOrigin::kProfileCreationFlow)
                   : nullptr;
  if (!manager || !scope) {
    return false;
  }
  bool allowed = false;
  base::RunLoop loop(base::RunLoop::Type::kNestableTasksAllowed);
  SpaceProfileLoads::Get().Prepare(
      manager, root, profile_path, std::move(*identity),
      base::BindOnce(
          [](bool* allowed, std::unique_ptr<ScopedProfileKeepAlive>* output,
             base::OnceClosure quit, bool valid,
             std::unique_ptr<ScopedProfileKeepAlive> lease) {
            *allowed = valid;
            *output = std::move(lease);
            std::move(quit).Run();
          },
          &allowed, catalog_keep_alive, loop.QuitClosure()));
  loop.Run();
  return allowed && manager;
}

std::optional<int> RegisterMigratedSpaceProfiles(
    const base::FilePath& root_basename,
    const std::vector<MigratedSpaceProfileBinding>& bindings) {
  DCHECK_CURRENTLY_ON(content::BrowserThread::UI);
  auto catalog = ReadCatalog();
  if (!ProfileSpacesEnabled() || !catalog ||
      !IsProfileBasename(root_basename)) {
    return std::nullopt;
  }
  std::set<base::FilePath> used;
  for (const auto& [root, group] : catalog->groups) {
    used.insert(root);
    for (const auto& [id, binding] : group) {
      used.insert(binding.directory);
      if (binding.directory == root_basename) {
        return std::nullopt;
      }
    }
  }
  auto& group = catalog->groups[root_basename];
  bool changed = false;
  std::set<std::string> ids;
  for (const auto& incoming : bindings) {
    if (!base::Uuid::ParseLowercase(incoming.id).is_valid() ||
        !ids.insert(incoming.id).second ||
        !IsProfileBasename(incoming.profile_basename) ||
        incoming.profile_basename == root_basename) {
      return std::nullopt;
    }
    const auto existing = group.find(incoming.id);
    if (existing != group.end()) {
      if (existing->second.directory != incoming.profile_basename ||
          existing->second.storage != SpaceProfileStorage::kLegacyPartition) {
        return std::nullopt;
      }
      continue;
    }
    if (!used.insert(incoming.profile_basename).second) {
      return std::nullopt;
    }
    group.emplace(incoming.id, Binding{incoming.profile_basename,
                                       SpaceProfileStorage::kLegacyPartition});
    changed = true;
  }
  return changed ? WriteCatalog(std::move(*catalog))
                 : std::make_optional(catalog->generation);
}

}  // namespace noventra
