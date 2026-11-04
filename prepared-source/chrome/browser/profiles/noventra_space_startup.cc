// Copyright 2026 The Noventra Authors. All rights reserved.
#include "chrome/browser/profiles/noventra_space_startup.h"

#include <algorithm>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "base/command_line.h"
#include "base/files/file_util.h"
#include "base/files/important_file_writer.h"
#include "base/functional/bind.h"
#include "base/json/json_reader.h"
#include "base/json/json_writer.h"
#include "base/run_loop.h"
#include "base/strings/string_number_conversions.h"
#include "base/task/sequenced_task_runner.h"
#include "base/uuid.h"
#include "base/values.h"
#include "chrome/browser/browser_process.h"
#include "chrome/browser/profiles/noventra_space_profile_registry.h"
#include "chrome/browser/profiles/profile_manager.h"
#include "chrome/browser/sessions/session_restore.h"
#include "chrome/common/chrome_constants.h"
#include "chrome/common/chrome_switches.h"
#include "chrome/common/pref_names.h"
#include "components/os_crypt/async/browser/os_crypt_async.h"
#include "components/prefs/pref_service.h"
#include "components/sessions/core/serialized_navigation_entry.h"
#include "content/public/browser/browser_thread.h"
#include "content/public/browser/storage_partition_path.h"
#include "crypto/hash.h"
#include "ui/base/page_transition_types.h"
#include "url/gurl.h"

namespace noventra {
namespace {

constexpr char kPartitionDomain[] = "noventra-account-spaces";
constexpr char kSpaceExtraData[] = "noventra.account_space";
constexpr char kPinnedExtraData[] = "noventra.pinned_tab";
constexpr char kSpacesPath[] = "noventra.browser.account_spaces";

using Error = SpaceMigrationError;
using Code = SpaceMigrationErrorCode;

base::unexpected<Error> Fail(Code code, const base::FilePath& path) {
  return base::unexpected(Error{code, path});
}

bool IsBasename(const base::FilePath& path) {
  return !path.empty() && !path.IsAbsolute() && !path.ReferencesParent() &&
         path == path.BaseName() &&
         path != base::FilePath(FILE_PATH_LITERAL("."));
}

bool IsRegularRoot(const base::FilePath& path) {
  return IsBasename(path) &&
         path != base::FilePath(chrome::kSystemProfileDir) &&
         path != base::FilePath(chrome::kGuestProfileDir) &&
         path != base::FilePath(FILE_PATH_LITERAL("NoventraSpaceRecovery")) &&
         path != base::FilePath(FILE_PATH_LITERAL("NoventraSpaceStartup"));
}

base::expected<base::DictValue, Error> ReadDictionary(
    const base::FilePath& path) {
  std::string contents;
  if (base::IsLink(path) || !base::ReadFileToString(path, &contents)) {
    return Fail(Code::kReadFailed, path);
  }
  auto value = base::JSONReader::ReadDict(contents, base::JSON_PARSE_RFC);
  if (!value) {
    return Fail(Code::kReadFailed, path);
  }
  return std::move(*value);
}

bool WriteDictionary(const base::FilePath& path, const base::DictValue& value) {
  const auto json = base::WriteJson(value);
  return json && base::ImportantFileWriter::WriteFileAtomically(path, *json);
}

bool StringMatches(const base::DictValue& value,
                   std::string_view key,
                   const std::string& expected) {
  const auto* actual = value.FindString(key);
  return actual && *actual == expected;
}

struct CachedSession {
  base::FilePath profile_path;
  SpaceSessionSeed seed;
};

struct StartupInput {
  base::FilePath user_data_directory;
  std::set<base::FilePath> roots;
  std::vector<SpaceProfileBindingForStartup> bindings;
  scoped_refptr<os_crypt_async::Encryptor> encryptor;
};

struct StartupPrepared {
  std::vector<SpaceMigrationPrepared> migrations;
  std::vector<CachedSession> sessions;
};

base::FilePath PartitionPath(const std::string& id) {
  return content::GetStoragePartitionRelativePath(kPartitionDomain, id);
}

base::FilePath DestinationBasename(const base::FilePath& root,
                                   const std::string& id) {
  const auto root_hash = crypto::hash::Sha256(root.AsUTF8Unsafe());
  return base::FilePath::FromUTF8Unsafe(
      "NoventraSpace-" + base::HexEncode(root_hash).substr(0, 16) + "-" + id);
}

base::expected<void, Error> RecordUnopenedSpaces(
    const base::FilePath& user_data,
    const base::FilePath& root,
    const std::set<std::string>& ids) {
  if (ids.empty()) {
    return base::ok();
  }
  const auto parent = user_data.AppendASCII("NoventraSpaceStartup");
  const auto directory = parent.Append(root);
  const auto path = directory.AppendASCII("unopened.json");
  for (const auto& candidate : {parent, directory, path}) {
    if (base::IsLink(candidate)) {
      return Fail(Code::kConflict, candidate);
    }
  }
  base::DictValue record;
  if (base::PathExists(path)) {
    auto previous = ReadDictionary(path);
    if (!previous || previous->FindInt("version") != 1 ||
        !StringMatches(*previous, "root", root.AsUTF8Unsafe()) ||
        !previous->FindDict("spaces")) {
      return Fail(Code::kConflict, path);
    }
    record = std::move(*previous);
  } else {
    record.Set("version", 1);
    record.Set("root", root.AsUTF8Unsafe());
    record.Set("spaces", base::DictValue());
  }
  auto* observed = record.FindDict("spaces");
  for (const auto& id : ids) {
    observed->Set(id, base::DictValue()
                          .Set("partition", PartitionPath(id).AsUTF8Unsafe())
                          .Set("observation", "absent_without_tab_or_pin"));
  }
  if (!base::CreateDirectory(directory) || !WriteDictionary(path, record)) {
    return Fail(Code::kWriteFailed, path);
  }
  return base::ok();
}

base::expected<void, Error> ValidateLegacyDestination(
    const base::FilePath& user_data,
    const SpaceProfileBindingForStartup& binding) {
  const auto destination = user_data.Append(binding.profile_basename);
  const auto marker_path =
      destination.AppendASCII("NoventraSpaceMigration.json");
  auto marker = ReadDictionary(marker_path);
  if (!marker || marker->FindInt("version") != 1 ||
      !StringMatches(*marker, "root", binding.root_basename.AsUTF8Unsafe()) ||
      !StringMatches(*marker, "space", binding.id) ||
      !StringMatches(*marker, "directory",
                     binding.profile_basename.AsUTF8Unsafe()) ||
      !StringMatches(*marker, "partition",
                     PartitionPath(binding.id).AsUTF8Unsafe()) ||
      !base::DirectoryExists(destination.Append(PartitionPath(binding.id)))) {
    return Fail(Code::kConflict, destination);
  }
  return base::ok();
}

struct ExistingMigration {
  SpaceMigrationPlan plan;
  std::optional<int> committed_generation;
};

base::expected<std::optional<ExistingMigration>, Error> ReadExistingPlan(
    const base::FilePath& user_data,
    const base::FilePath& root) {
  const auto recovery =
      user_data.AppendASCII("NoventraSpaceRecovery").Append(root);
  if (base::IsLink(recovery.DirName())) {
    return Fail(Code::kConflict, recovery.DirName());
  }
  if (!base::PathExists(recovery)) {
    return std::optional<ExistingMigration>();
  }
  if (base::IsLink(recovery)) {
    return Fail(Code::kConflict, recovery);
  }
  const auto path = recovery.AppendASCII("migration.json");
  auto journal = ReadDictionary(path);
  const auto* raw_plan = journal ? journal->FindDict("plan") : nullptr;
  const auto* targets = raw_plan ? raw_plan->FindDict("targets") : nullptr;
  const auto* state = journal ? journal->FindString("state") : nullptr;
  if (!journal || journal->FindInt("version") != 1 || !raw_plan || !targets ||
      !state ||
      (*state != "inventory" && *state != "snapshot" && *state != "prepared" &&
       *state != "published" && *state != "committed") ||
      targets->empty() ||
      !StringMatches(*raw_plan, "root", root.AsUTF8Unsafe())) {
    return Fail(Code::kConflict, path);
  }
  SpaceMigrationPlan result;
  result.user_data_directory = user_data;
  result.root_profile_basename = root;
  for (const auto [id, value] : *targets) {
    const auto* target = value.GetIfDict();
    const auto* directory = target ? target->FindString("directory") : nullptr;
    if (!base::Uuid::ParseLowercase(id).is_valid() || !directory ||
        !IsRegularRoot(base::FilePath::FromUTF8Unsafe(*directory)) ||
        !StringMatches(*target, "partition",
                       PartitionPath(id).AsUTF8Unsafe())) {
      return Fail(Code::kConflict, path);
    }
    result.targets.push_back(
        {id, base::FilePath::FromUTF8Unsafe(*directory), PartitionPath(id)});
  }
  ExistingMigration existing;
  existing.plan = std::move(result);
  if (*state == "committed") {
    existing.committed_generation = journal->FindInt("catalog_generation");
    if (!existing.committed_generation || *existing.committed_generation < 1) {
      return Fail(Code::kConflict, path);
    }
  }
  return std::make_optional(std::move(existing));
}

base::expected<void, Error> SeedSavedPins(const base::FilePath& snapshot,
                                          const std::string& id,
                                          SpaceSessionSeed* seed) {
  const auto path = snapshot.AppendASCII("Preferences");
  if (!base::PathExists(path)) {
    return base::ok();
  }
  auto preferences = ReadDictionary(path);
  if (!preferences) {
    return base::unexpected(preferences.error());
  }
  const auto* values = preferences->FindList("pinned_tabs");
  if (!values) {
    return base::ok();
  }
  auto window = std::make_unique<sessions::SessionWindow>();
  // Profile-wide saved pins have no recorded source window. The native host
  // restorer assigns this unbound window to the host that first opens Space.
  window->window_id = SessionID::InvalidValue();
  window->selected_tab_index = 0;
  for (const auto& value : *values) {
    const auto* pin = value.GetIfDict();
    if (!pin || !StringMatches(*pin, kSpaceExtraData, id)) {
      continue;
    }
    const auto* url_text = pin->FindString("url");
    const GURL url(url_text ? *url_text : "");
    if (!url.is_valid()) {
      return Fail(Code::kInvalidSession, path);
    }
    auto tab = std::make_unique<sessions::SessionTab>();
    tab->window_id = window->window_id;
    tab->tab_visual_index = static_cast<int>(window->tabs.size());
    tab->current_navigation_index = 0;
    tab->pinned = true;
    tab->extra_data[kSpaceExtraData] = id;
    if (const auto* metadata = pin->FindString(kPinnedExtraData)) {
      tab->extra_data[kPinnedExtraData] = *metadata;
    }
    sessions::SerializedNavigationEntry navigation;
    navigation.set_index(0);
    navigation.set_virtual_url(url);
    navigation.set_original_request_url(url);
    navigation.set_transition_type(ui::PAGE_TRANSITION_AUTO_TOPLEVEL);
    tab->navigations.push_back(std::move(navigation));
    window->tabs.push_back(std::move(tab));
  }
  if (!window->tabs.empty()) {
    seed->active_window_id = SessionID::InvalidValue();
    seed->windows.push_back(std::move(window));
  }
  return base::ok();
}

base::expected<StartupPrepared, Error> PrepareOnWorker(StartupInput input) {
  StartupPrepared prepared;
  std::map<base::FilePath, std::set<std::string>> bound;
  for (const auto& binding : input.bindings) {
    bound[binding.root_basename].insert(binding.id);
  }
  for (const auto& root : input.roots) {
    const auto source = input.user_data_directory.Append(root);
    if (!base::PathExists(source)) {
      if (bound.contains(root)) {
        return Fail(Code::kReadFailed, source);
      }
      continue;
    }
    if (!base::DirectoryExists(source) || base::IsLink(source)) {
      return Fail(Code::kConflict, source);
    }
    base::DictValue preferences;
    const auto preference_path = source.AppendASCII("Preferences");
    if (base::PathExists(preference_path)) {
      auto read = ReadDictionary(preference_path);
      if (!read) {
        return base::unexpected(read.error());
      }
      preferences = std::move(*read);
    }
    auto session = ReadSpaceMigrationSessionSeed(source, "", input.encryptor);
    if (!session) {
      return base::unexpected(session.error());
    }
    if (!session->filter.invalid_space_ids.empty()) {
      return Fail(Code::kInvalidSession, source);
    }
    std::set<std::string> stored_tabs(session->observed_space_ids.begin(),
                                      session->observed_space_ids.end());
    std::set<std::string> identities = stored_tabs;
    const auto* catalog_value = preferences.FindByDottedPath(kSpacesPath);
    if (catalog_value && !catalog_value->is_dict()) {
      return Fail(Code::kInvalidPlan, preference_path);
    }
    if (const auto* names = preferences.FindDictByDottedPath(kSpacesPath)) {
      for (const auto [id, name] : *names) {
        if (!name.is_string() ||
            (!id.empty() && !base::Uuid::ParseLowercase(id).is_valid())) {
          return Fail(Code::kInvalidPlan, preference_path);
        }
        if (!id.empty()) {
          identities.insert(id);
        }
      }
    }
    if (const auto* pins_value = preferences.Find("pinned_tabs");
        pins_value && !pins_value->is_list()) {
      return Fail(Code::kInvalidPlan, preference_path);
    }
    if (const auto* pins = preferences.FindList("pinned_tabs")) {
      for (const auto& value : *pins) {
        const auto* pin = value.GetIfDict();
        const auto* id = pin ? pin->FindString(kSpaceExtraData) : nullptr;
        if (id && !id->empty()) {
          if (!base::Uuid::ParseLowercase(*id).is_valid()) {
            return Fail(Code::kInvalidSession, preference_path);
          }
          identities.insert(*id);
          stored_tabs.insert(*id);
        }
      }
    }
    SpaceMigrationPlan plan;
    plan.user_data_directory = input.user_data_directory;
    plan.root_profile_basename = root;
    auto existing_plan = ReadExistingPlan(input.user_data_directory, root);
    if (!existing_plan.has_value()) {
      return base::unexpected(existing_plan.error());
    }
    if (*existing_plan) {
      plan = (**existing_plan).plan;
    }
    std::set<std::string> unopened;
    for (const auto& id : identities) {
      if (bound[root].contains(id)) {
        continue;
      }
      if (std::ranges::any_of(plan.targets, [&](const auto& target) {
            return target.id == id;
          })) {
        continue;
      }
      const auto relative = PartitionPath(id);
      const auto partition = source.Append(relative);
      if (base::IsLink(partition)) {
        return Fail(Code::kConflict, partition);
      }
      if (!base::DirectoryExists(partition)) {
        if (base::PathExists(partition) || stored_tabs.contains(id)) {
          return Fail(Code::kReadFailed, partition);
        }
        unopened.insert(id);
        continue;
      }
      plan.targets.push_back({id, DestinationBasename(root, id), relative});
    }
    auto recorded =
        RecordUnopenedSpaces(input.user_data_directory, root, unopened);
    if (!recorded) {
      return base::unexpected(recorded.error());
    }
    if (!plan.targets.empty()) {
      base::expected<SpaceMigrationPrepared, Error> migrated =
          [&]() -> base::expected<SpaceMigrationPrepared, Error> {
        if (*existing_plan && (**existing_plan).committed_generation) {
          if (!std::ranges::equal(plan.targets, (**existing_plan).plan.targets,
                                  [](const auto& a, const auto& b) {
                                    return a.id == b.id &&
                                           a.profile_basename ==
                                               b.profile_basename &&
                                           a.partition_relative_path ==
                                               b.partition_relative_path;
                                  })) {
            return Fail(Code::kConflict, source);
          }
          SpaceMigrationPrepared result;
          result.recovery_directory =
              input.user_data_directory.AppendASCII("NoventraSpaceRecovery")
                  .Append(root);
          result.snapshot_profile_directory =
              result.recovery_directory.AppendASCII("profile");
          result.targets = plan.targets;
          result.committed_catalog_generation =
              (**existing_plan).committed_generation;
          return result;
        }
        return PrepareSpaceProfileMigration(plan);
      }();
      if (!migrated) {
        return base::unexpected(migrated.error());
      }
      for (const auto& target : migrated->targets) {
        const auto binding =
            std::ranges::find_if(input.bindings, [&](const auto& binding) {
              return binding.root_basename == root && binding.id == target.id;
            });
        if (binding == input.bindings.end()) {
          input.bindings.push_back({root, target.id, target.profile_basename,
                                    SpaceProfileStorage::kLegacyPartition,
                                    false});
        } else if (binding->profile_basename != target.profile_basename ||
                   binding->storage != SpaceProfileStorage::kLegacyPartition) {
          return Fail(Code::kConflict, migrated->recovery_directory);
        }
      }
      prepared.migrations.push_back(std::move(*migrated));
    }
  }
  for (const auto& binding : input.bindings) {
    const auto destination =
        input.user_data_directory.Append(binding.profile_basename);
    if (base::IsLink(destination)) {
      return Fail(Code::kConflict, destination);
    }
    if (!base::DirectoryExists(destination)) {
      if (binding.storage == SpaceProfileStorage::kDefault &&
          !binding.ui_seeded && !binding.initialization_started &&
          !base::PathExists(destination)) {
        prepared.sessions.push_back({destination, SpaceSessionSeed()});
        continue;
      }
      return Fail(Code::kReadFailed, destination);
    }
    if (binding.storage == SpaceProfileStorage::kDefault &&
        !IsSpaceProfileDirectoryOwned(input.user_data_directory, binding)) {
      return Fail(Code::kConflict, destination);
    }
    if (binding.storage == SpaceProfileStorage::kLegacyPartition) {
      auto valid =
          ValidateLegacyDestination(input.user_data_directory, binding);
      if (!valid) {
        return base::unexpected(valid.error());
      }
    }
    auto native =
        ReadSpaceMigrationSessionSeed(destination, binding.id, input.encryptor);
    if (!native) {
      return base::unexpected(native.error());
    }
    if (!native->filter.invalid_space_ids.empty() ||
        native->filter.removed_tabs) {
      return Fail(Code::kInvalidSession, destination);
    }
    if (!native->has_complete_native_session &&
        binding.storage == SpaceProfileStorage::kLegacyPartition) {
      const auto snapshot =
          input.user_data_directory.AppendASCII("NoventraSpaceRecovery")
              .Append(binding.root_basename)
              .AppendASCII("profile");
      auto recovery =
          ReadSpaceMigrationSessionSeed(snapshot, binding.id, input.encryptor);
      if (!recovery || !recovery->filter.invalid_space_ids.empty()) {
        return recovery ? Fail(Code::kInvalidSession, snapshot)
                        : base::unexpected(recovery.error());
      }
      if (recovery->windows.empty()) {
        auto pins = SeedSavedPins(snapshot, binding.id, &*recovery);
        if (!pins) {
          return base::unexpected(pins.error());
        }
      }
      // These saved-group IDs belong to the source Profile's sync model.
      // Rebuild local groups in the destination instead of connecting to
      // missing foreign IDs, which native sync treats as remote deletions.
      for (auto& window : recovery->windows) {
        for (auto& group : window->tab_groups) {
          group->saved_guid.reset();
        }
      }
      native = std::move(recovery);
    }
    prepared.sessions.push_back({destination, std::move(*native)});
  }
  return prepared;
}

template <class T>
base::expected<T, Error> RunOnWorker(
    scoped_refptr<base::SequencedTaskRunner> runner,
    base::OnceCallback<base::expected<T, Error>()> task,
    const base::FilePath& error_path) {
  std::optional<base::expected<T, Error>> result;
  base::RunLoop loop;
  if (!runner->PostTaskAndReplyWithResult(
          FROM_HERE, std::move(task),
          base::BindOnce(
              [](std::optional<base::expected<T, Error>>* result,
                 base::OnceClosure done, base::expected<T, Error> value) {
                result->emplace(std::move(value));
                std::move(done).Run();
              },
              &result, loop.QuitClosure()))) {
    return Fail(Code::kReadFailed, error_path);
  }
  loop.Run();
  if (!result) {
    return Fail(Code::kInterrupted, error_path);
  }
  return std::move(*result);
}

base::expected<void, Error> VerifyAndCommitOnWorker(
    const base::FilePath& user_data,
    base::DictValue expected_catalog,
    int generation,
    std::vector<SpaceMigrationPrepared> migrations) {
  if (!expected_catalog.empty()) {
    auto disk = ReadDictionary(user_data.AppendASCII("Local State"));
    const auto* actual =
        disk ? disk->FindDictByDottedPath(kSpaceProfilesLocalState) : nullptr;
    if (!actual || *actual != expected_catalog) {
      return Fail(Code::kWriteFailed, user_data.AppendASCII("Local State"));
    }
  }
  for (const auto& migration : migrations) {
    if (migration.committed_catalog_generation) {
      if (*migration.committed_catalog_generation > generation) {
        return Fail(Code::kConflict, migration.recovery_directory);
      }
      continue;
    }
    auto committed =
        CommitSpaceProfileMigration(migration.recovery_directory, generation);
    if (!committed) {
      return base::unexpected(committed.error());
    }
  }
  return base::ok();
}

}  // namespace

base::expected<void, SpaceMigrationError> PrepareProfileSpacesForStartup(
    const base::FilePath& user_data_directory,
    base::CommandLine& command_line) {
  if (!ProfileSpacesEnabled()) {
    return base::ok();
  }
  DCHECK_CURRENTLY_ON(content::BrowserThread::UI);
  auto* manager = g_browser_process->profile_manager();
  auto* local_state = g_browser_process->local_state();
  auto bindings = SpaceProfileBindingsForStartup();
  if (!manager || !local_state || !bindings ||
      !user_data_directory.IsAbsolute() ||
      user_data_directory != manager->user_data_dir() ||
      !manager->GetLoadedProfiles().empty()) {
    return Fail(Code::kInvalidPlan, user_data_directory);
  }
  StartupInput input;
  input.user_data_directory = user_data_directory;
  input.bindings = *bindings;
  std::map<base::FilePath, const SpaceProfileBindingForStartup*> child_bindings;
  for (const auto& binding : *bindings) {
    if (!IsRegularRoot(binding.root_basename) ||
        !IsRegularRoot(binding.profile_basename)) {
      return Fail(Code::kInvalidPlan, user_data_directory);
    }
    child_bindings.emplace(binding.profile_basename, &binding);
    input.roots.insert(binding.root_basename);
  }
  for (const auto [name, value] :
       local_state->GetDict(prefs::kProfileAttributes)) {
    const auto directory = base::FilePath::FromUTF8Unsafe(name);
    if (!IsRegularRoot(directory)) {
      continue;
    }
    if (!child_bindings.contains(directory)) {
      input.roots.insert(directory);
    }
  }
  auto requested = command_line.GetSwitchValuePath(switches::kProfileDirectory);
  if (requested.empty()) {
    requested = base::FilePath::FromUTF8Unsafe(
        local_state->GetString(prefs::kProfileLastUsed));
  }
  if (requested.empty()) {
    requested = base::FilePath::FromUTF8Unsafe(chrome::kInitialProfile);
  }
  if (!IsRegularRoot(requested)) {
    return Fail(Code::kInvalidPlan, requested);
  }
  if (const auto found = child_bindings.find(requested);
      found != child_bindings.end()) {
    const auto& binding = *found->second;
    requested = binding.root_basename;
    command_line.RemoveSwitch(switches::kProfileDirectory);
    command_line.AppendSwitchPath(switches::kProfileDirectory, requested);
    local_state->SetString(prefs::kProfileLastUsed, requested.AsUTF8Unsafe());
    SessionRestore::SetProfileSpaceStartupSelection(
        user_data_directory.Append(requested), binding.id);
  }
  input.roots.insert(requested);
  auto last_active = local_state->GetList(prefs::kProfilesLastActive).Clone();
  base::ListValue root_last_active;
  for (const auto& value : last_active) {
    const auto* name = value.GetIfString();
    if (!name) {
      return Fail(Code::kInvalidPlan,
                  user_data_directory.AppendASCII("Local State"));
    }
    auto directory = base::FilePath::FromUTF8Unsafe(*name);
    if (const auto found = child_bindings.find(directory);
        found != child_bindings.end()) {
      directory = found->second->root_basename;
    }
    if (!IsRegularRoot(directory)) {
      return Fail(Code::kInvalidPlan, directory);
    }
    if (!root_last_active.contains(directory.AsUTF8Unsafe())) {
      root_last_active.Append(directory.AsUTF8Unsafe());
    }
    input.roots.insert(directory);
  }
  if (root_last_active != last_active) {
    local_state->SetList(prefs::kProfilesLastActive,
                         std::move(root_last_active));
  }
  base::RunLoop crypt_ready;
  g_browser_process->os_crypt_async()->GetInstance(base::BindOnce(
      [](scoped_refptr<os_crypt_async::Encryptor>* encryptor,
         base::OnceClosure ready,
         scoped_refptr<os_crypt_async::Encryptor> value) {
        *encryptor = std::move(value);
        std::move(ready).Run();
      },
      &input.encryptor, crypt_ready.QuitClosure()));
  crypt_ready.Run();
  base::RunLoop initial_write;
  local_state->CommitPendingWrite(initial_write.QuitClosure());
  initial_write.Run();
  auto runner = CreateSpaceMigrationTaskRunner();
  auto prepared = RunOnWorker<StartupPrepared>(
      runner, base::BindOnce(&PrepareOnWorker, std::move(input)),
      user_data_directory);
  if (!prepared || !manager->GetLoadedProfiles().empty()) {
    return prepared ? Fail(Code::kInvalidPlan, user_data_directory)
                    : base::unexpected(prepared.error());
  }
  int generation = 0;
  for (const auto& migration : prepared->migrations) {
    std::vector<MigratedSpaceProfileBinding> incoming;
    for (const auto& target : migration.targets) {
      incoming.push_back({target.id, target.profile_basename});
    }
    const auto root = migration.recovery_directory.BaseName();
    auto written = RegisterMigratedSpaceProfiles(root, incoming);
    if (!written) {
      return Fail(Code::kConflict, migration.recovery_directory);
    }
    generation = std::max(generation, *written);
  }
  const auto catalog = local_state->GetDict(kSpaceProfilesLocalState).Clone();
  base::RunLoop catalog_write;
  local_state->CommitPendingWrite(catalog_write.QuitClosure());
  catalog_write.Run();
  auto committed = RunOnWorker<void>(
      runner,
      base::BindOnce(&VerifyAndCommitOnWorker, user_data_directory,
                     catalog.Clone(), generation,
                     std::move(prepared->migrations)),
      user_data_directory);
  if (!committed || !manager->GetLoadedProfiles().empty()) {
    return committed ? Fail(Code::kInvalidPlan, user_data_directory)
                     : base::unexpected(committed.error());
  }
  for (auto& cached : prepared->sessions) {
    SessionRestore::CacheProfileSpaceSession(cached.profile_path,
                                             std::move(cached.seed.windows),
                                             cached.seed.active_window_id);
  }
  return base::ok();
}

}  // namespace noventra
