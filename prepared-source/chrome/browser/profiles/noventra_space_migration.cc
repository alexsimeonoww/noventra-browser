// Copyright 2026 The Noventra Authors. All rights reserved.
#include "chrome/browser/profiles/noventra_space_migration.h"

#include <algorithm>
#include <array>
#include <map>
#include <set>
#include <utility>

#include "base/files/file.h"
#include "base/files/file_enumerator.h"
#include "base/files/file_util.h"
#include "base/files/important_file_writer.h"
#include "base/files/scoped_temp_dir.h"
#include "base/functional/callback.h"
#include "base/json/json_reader.h"
#include "base/json/json_writer.h"
#include "base/numerics/checked_math.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_util.h"
#include "base/system/sys_info.h"
#include "base/task/sequenced_task_runner.h"
#include "base/task/thread_pool.h"
#include "base/uuid.h"
#include "base/values.h"
#include "build/build_config.h"
#include "components/sessions/core/command_storage_backend.h"
#include "components/sessions/core/command_storage_manager.h"
#include "components/sessions/core/command_storage_read_status.h"
#include "components/sessions/core/session_constants.h"
#include "components/sessions/core/session_service_commands.h"
#include "crypto/hash.h"

namespace noventra {
namespace {

constexpr char kSpaceExtraData[] = "noventra.account_space";
constexpr char kJournalName[] = "migration.json";
constexpr char kTargetMarker[] = "NoventraSpaceMigration.json";
constexpr int64_t kFreeSpaceReserve = 16 * 1024 * 1024;

using Error = SpaceMigrationError;
using Code = SpaceMigrationErrorCode;

base::unexpected<Error> Fail(Code code, const base::FilePath& path) {
  return base::unexpected(Error{code, path});
}

bool IsBasename(const base::FilePath& path) {
  return !path.empty() && path == path.BaseName() && !path.IsAbsolute() &&
         !path.ReferencesParent() &&
         path != base::FilePath(FILE_PATH_LITERAL("."));
}

bool IsSpaceId(const std::string& id) {
  return base::Uuid::ParseLowercase(id).is_valid();
}

bool IsPartitionPath(const base::FilePath& path) {
  if (path.empty() || path.IsAbsolute() || path.ReferencesParent()) {
    return false;
  }
  const auto parts = path.GetComponents();
  return parts.size() == 4 && parts[0] == FILE_PATH_LITERAL("Storage") &&
         parts[1] == FILE_PATH_LITERAL("ext") &&
         parts[2] == FILE_PATH_LITERAL("noventra-account-spaces") &&
         !parts[3].empty();
}

bool CopyTree(const base::FilePath& from, const base::FilePath& to) {
#if BUILDFLAG(IS_POSIX)
  return base::CopyDirectoryNoFollow(from, to, true);
#else
  return base::CopyDirectory(from, to, true);
#endif
}

base::expected<base::DictValue, Error> ReadDictionary(
    const base::FilePath& path) {
  std::string data;
  if (!base::ReadFileToString(path, &data)) {
    return Fail(Code::kReadFailed, path);
  }
  auto value = base::JSONReader::ReadDict(data, base::JSON_PARSE_RFC);
  if (!value) {
    return Fail(Code::kReadFailed, path);
  }
  return std::move(*value);
}

bool WriteDictionary(const base::FilePath& path, const base::DictValue& value) {
  auto data = base::WriteJson(value);
  return data && base::ImportantFileWriter::WriteFileAtomically(path, *data);
}

base::expected<std::string, Error> HashFile(const base::FilePath& path) {
  uint32_t flags = base::File::FLAG_OPEN | base::File::FLAG_READ;
#if BUILDFLAG(IS_POSIX)
  flags |= base::File::FLAG_NO_FOLLOW;
#endif
  base::File file(path, flags);
  std::array<uint8_t, crypto::hash::kSha256Size> hash;
  if (!file.IsValid() ||
      !crypto::hash::HashFile(crypto::hash::HashKind::kSha256, &file, hash)) {
    return Fail(Code::kReadFailed, path);
  }
  return base::HexEncode(hash);
}

struct Inventory {
  base::DictValue files;
  base::ListValue directories;
  int64_t bytes = 0;
};

base::expected<Inventory, Error> InventoryTree(const base::FilePath& root) {
  if (!base::DirectoryExists(root) || base::IsLink(root)) {
    return Fail(Code::kReadFailed, root);
  }
  int types = base::FileEnumerator::FILES | base::FileEnumerator::DIRECTORIES;
#if BUILDFLAG(IS_POSIX) || BUILDFLAG(IS_FUCHSIA)
  types |= base::FileEnumerator::SHOW_SYM_LINKS;
#endif
  base::FileEnumerator files(
      root, true, types, base::FilePath::StringType(),
      base::FileEnumerator::FolderSearchPolicy::ALL,
      base::FileEnumerator::ErrorPolicy::STOP_ENUMERATION);
  Inventory inventory;
  std::set<std::string> directories;
  base::CheckedNumeric<int64_t> bytes = 0;
  for (base::FilePath path = files.Next(); !path.empty(); path = files.Next()) {
    base::FilePath relative;
    if (!root.AppendRelativePath(path, &relative) || base::IsLink(path)) {
      return Fail(Code::kReadFailed, path);
    }
    const auto info = files.GetInfo();
    if (info.IsDirectory()) {
      directories.insert(relative.AsUTF8Unsafe());
      continue;
    }
#if BUILDFLAG(IS_POSIX) || BUILDFLAG(IS_FUCHSIA)
    if (!S_ISREG(info.stat().st_mode)) {
      return Fail(Code::kReadFailed, path);
    }
#endif
    auto digest = HashFile(path);
    if (!digest.has_value()) {
      return base::unexpected(digest.error());
    }
    bytes += info.GetSize();
    inventory.files.Set(relative.AsUTF8Unsafe(),
                        base::DictValue()
                            .Set("size", base::NumberToString(info.GetSize()))
                            .Set("sha256", std::move(*digest)));
  }
  if (files.GetError() != base::File::FILE_OK) {
    return Fail(Code::kReadFailed, root);
  }
  if (!bytes.AssignIfValid(&inventory.bytes)) {
    return Fail(Code::kInsufficientSpace, root);
  }
  for (const auto& directory : directories) {
    inventory.directories.Append(directory);
  }
  return inventory;
}

base::DictValue InventoryValue(const Inventory& inventory) {
  return base::DictValue()
      .Set("files", inventory.files.Clone())
      .Set("directories", inventory.directories.Clone());
}

base::DictValue PlanValue(const SpaceMigrationPlan& plan) {
  base::DictValue targets;
  for (const auto& target : plan.targets) {
    targets.Set(
        target.id,
        base::DictValue()
            .Set("directory", target.profile_basename.AsUTF8Unsafe())
            .Set("partition", target.partition_relative_path.AsUTF8Unsafe()));
  }
  return base::DictValue()
      .Set("root", plan.root_profile_basename.AsUTF8Unsafe())
      .Set("targets", std::move(targets));
}

base::DictValue MarkerValue(const SpaceMigrationPlan& plan,
                            const SpaceMigrationTarget& target) {
  return base::DictValue()
      .Set("version", 1)
      .Set("root", plan.root_profile_basename.AsUTF8Unsafe())
      .Set("space", target.id)
      .Set("directory", target.profile_basename.AsUTF8Unsafe())
      .Set("partition", target.partition_relative_path.AsUTF8Unsafe());
}

bool ValidatePlan(const SpaceMigrationPlan& plan) {
  if (!plan.user_data_directory.IsAbsolute() ||
      plan.user_data_directory.ReferencesParent() ||
      !IsBasename(plan.root_profile_basename) || plan.targets.empty() ||
      plan.root_profile_basename ==
          base::FilePath(FILE_PATH_LITERAL("NoventraSpaceRecovery"))) {
    return false;
  }
  std::set<std::string> identities;
  std::set<base::FilePath> directories;
  std::set<base::FilePath> partitions;
  for (const auto& target : plan.targets) {
    if (!IsSpaceId(target.id) || !IsBasename(target.profile_basename) ||
        target.profile_basename == plan.root_profile_basename ||
        target.profile_basename ==
            base::FilePath(FILE_PATH_LITERAL("NoventraSpaceRecovery")) ||
        !IsPartitionPath(target.partition_relative_path) ||
        !identities.insert(target.id).second ||
        !directories.insert(target.profile_basename).second ||
        !partitions.insert(target.partition_relative_path).second) {
      return false;
    }
  }
  return true;
}

bool AtCheckpoint(
    const base::RepeatingCallback<bool(SpaceMigrationCheckpoint)>& callback,
    SpaceMigrationCheckpoint checkpoint) {
  return !callback || callback.Run(checkpoint);
}

bool HasSessionFile(const base::FilePath& directory) {
  base::FileEnumerator files(directory, false, base::FileEnumerator::FILES,
                             FILE_PATH_LITERAL("Session_*"));
  return !files.Next().empty();
}

}  // namespace

base::expected<SpaceMigrationPrepared, SpaceMigrationError>
PrepareSpaceProfileMigration(
    const SpaceMigrationPlan& plan,
    base::RepeatingCallback<bool(SpaceMigrationCheckpoint)> checkpoint) {
  if (!ValidatePlan(plan)) {
    return Fail(Code::kInvalidPlan, plan.user_data_directory);
  }
  const auto source =
      plan.user_data_directory.Append(plan.root_profile_basename);
  const auto recovery_parent =
      plan.user_data_directory.AppendASCII("NoventraSpaceRecovery");
  const auto recovery = recovery_parent.Append(plan.root_profile_basename);
  const auto journal_path = recovery.AppendASCII(kJournalName);
  const auto snapshot = recovery.AppendASCII("profile");
  const auto snapshot_staging = recovery.AppendASCII("profile.incomplete");
  const auto local_state = plan.user_data_directory.AppendASCII("Local State");
  const auto targets_directory = recovery.AppendASCII("targets");
  for (const auto& path : {source, recovery_parent, recovery, journal_path,
                           snapshot, snapshot_staging, targets_directory}) {
    if (base::IsLink(path)) {
      return Fail(Code::kConflict, path);
    }
  }
  base::DictValue journal;
  if (base::PathExists(recovery)) {
    auto loaded = ReadDictionary(journal_path);
    if (!loaded.has_value()) {
      return base::unexpected(loaded.error());
    }
    journal = std::move(*loaded);
    if (journal.FindInt("version") != 1 || !journal.FindDict("plan") ||
        *journal.FindDict("plan") != PlanValue(plan)) {
      return Fail(Code::kConflict, journal_path);
    }
  } else {
    auto inventory = InventoryTree(source);
    if (!inventory.has_value()) {
      return base::unexpected(inventory.error());
    }
    base::CheckedNumeric<int64_t> needed = inventory->bytes;
    for (const auto& target : plan.targets) {
      auto partition =
          InventoryTree(source.Append(target.partition_relative_path));
      if (!partition.has_value()) {
        return base::unexpected(partition.error());
      }
      needed += partition->bytes;
    }
    needed += kFreeSpaceReserve;
    const auto free =
        base::SysInfo::AmountOfFreeDiskSpace(plan.user_data_directory);
    int64_t required;
    if (!needed.AssignIfValid(&required) || !free || *free < required) {
      return Fail(Code::kInsufficientSpace, recovery);
    }
    journal.Set("version", 1);
    journal.Set("plan", PlanValue(plan));
    journal.Set("source", InventoryValue(*inventory));
    journal.Set("state", "inventory");
    if (base::PathExists(local_state)) {
      auto digest = HashFile(local_state);
      if (!digest.has_value()) {
        return base::unexpected(digest.error());
      }
      journal.Set("local_state_sha256", std::move(*digest));
    }
    base::ScopedTempDir initialized;
    if (!base::CreateDirectory(recovery_parent) ||
        !initialized.CreateUniqueTempDirUnderPath(recovery_parent) ||
        !WriteDictionary(initialized.GetPath().AppendASCII(kJournalName),
                         journal) ||
        !base::Move(initialized.GetPath(), recovery)) {
      return Fail(Code::kWriteFailed, journal_path);
    }
    initialized.Take();
  }
  const auto* state = journal.FindString("state");
  const auto* original_inventory = journal.FindDict("source");
  if (!state || !original_inventory ||
      (*state != "inventory" && *state != "snapshot" && *state != "prepared" &&
       *state != "published" && *state != "committed")) {
    return Fail(Code::kConflict, journal_path);
  }
  std::string phase = *state;
  if (phase == "inventory") {
    auto current = InventoryTree(source);
    if (!current.has_value() ||
        InventoryValue(*current) != *original_inventory) {
      return Fail(Code::kSourceChanged, source);
    }
    if (!base::PathExists(snapshot)) {
      if (!base::DeletePathRecursively(snapshot_staging) ||
          !CopyTree(source, snapshot_staging)) {
        return Fail(Code::kWriteFailed, snapshot_staging);
      }
      auto copied = InventoryTree(snapshot_staging);
      auto unchanged = InventoryTree(source);
      if (!copied.has_value() || !unchanged.has_value() ||
          InventoryValue(*copied) != *original_inventory ||
          InventoryValue(*unchanged) != *original_inventory) {
        return Fail(Code::kSourceChanged, source);
      }
      if (!base::Move(snapshot_staging, snapshot)) {
        return Fail(Code::kWriteFailed, snapshot);
      }
    }
    if (const auto* expected = journal.FindString("local_state_sha256")) {
      auto current_hash = HashFile(local_state);
      if (!current_hash.has_value() || *current_hash != *expected ||
          !base::CopyFile(local_state, recovery.AppendASCII("Local State"))) {
        return Fail(Code::kSourceChanged, local_state);
      }
    }
    if (!AtCheckpoint(checkpoint, SpaceMigrationCheckpoint::kSnapshotReady)) {
      return Fail(Code::kInterrupted, snapshot);
    }
    journal.Set("state", "snapshot");
    if (!WriteDictionary(journal_path, journal)) {
      return Fail(Code::kWriteFailed, journal_path);
    }
    phase = "snapshot";
  }
  auto saved = InventoryTree(snapshot);
  if (!saved.has_value() || InventoryValue(*saved) != *original_inventory) {
    return Fail(Code::kSourceChanged, snapshot);
  }
  if (phase == "snapshot") {
    if (!base::CreateDirectory(targets_directory)) {
      return Fail(Code::kWriteFailed, targets_directory);
    }
    for (const auto& target : plan.targets) {
      const auto staging = targets_directory.Append(target.profile_basename);
      const auto partition_source =
          snapshot.Append(target.partition_relative_path);
      const auto partition_destination =
          staging.Append(target.partition_relative_path);
      if (base::PathExists(
              plan.user_data_directory.Append(target.profile_basename))) {
        return Fail(Code::kConflict,
                    plan.user_data_directory.Append(target.profile_basename));
      }
      if (!base::DeletePathRecursively(staging) ||
          !base::CreateDirectory(partition_destination.DirName()) ||
          !CopyTree(partition_source, partition_destination)) {
        return Fail(Code::kWriteFailed, staging);
      }
      auto before = InventoryTree(partition_source);
      auto after = InventoryTree(partition_destination);
      if (!before.has_value() || !after.has_value() ||
          InventoryValue(*before) != InventoryValue(*after)) {
        return Fail(Code::kSourceChanged, partition_destination);
      }
      if (!WriteDictionary(staging.AppendASCII(kTargetMarker),
                           MarkerValue(plan, target))) {
        return Fail(Code::kWriteFailed, staging);
      }
    }
    if (!AtCheckpoint(checkpoint, SpaceMigrationCheckpoint::kTargetsReady)) {
      return Fail(Code::kInterrupted, targets_directory);
    }
    journal.Set("state", "prepared");
    if (!WriteDictionary(journal_path, journal)) {
      return Fail(Code::kWriteFailed, journal_path);
    }
    phase = "prepared";
  }
  if (phase == "prepared") {
    for (const auto& target : plan.targets) {
      const auto destination =
          plan.user_data_directory.Append(target.profile_basename);
      if (!base::PathExists(destination)) {
        if (!base::Move(targets_directory.Append(target.profile_basename),
                        destination)) {
          return Fail(Code::kWriteFailed, destination);
        }
      }
      auto marker = ReadDictionary(destination.AppendASCII(kTargetMarker));
      if (!marker.has_value() || *marker != MarkerValue(plan, target)) {
        return Fail(Code::kConflict, destination);
      }
      if (!AtCheckpoint(checkpoint,
                        SpaceMigrationCheckpoint::kTargetPublished)) {
        return Fail(Code::kInterrupted, destination);
      }
    }
    journal.Set("state", "published");
    if (!WriteDictionary(journal_path, journal)) {
      return Fail(Code::kWriteFailed, journal_path);
    }
    phase = "published";
  }
  if (!AtCheckpoint(checkpoint, SpaceMigrationCheckpoint::kPublished)) {
    return Fail(Code::kInterrupted, recovery);
  }
  SpaceMigrationPrepared result;
  result.recovery_directory = recovery;
  result.snapshot_profile_directory = snapshot;
  result.targets = plan.targets;
  if (phase == "committed") {
    result.committed_catalog_generation = journal.FindInt("catalog_generation");
    if (!result.committed_catalog_generation ||
        *result.committed_catalog_generation < 1) {
      return Fail(Code::kConflict, journal_path);
    }
  }
  return result;
}

base::expected<void, SpaceMigrationError> CommitSpaceProfileMigration(
    const base::FilePath& recovery_directory,
    int catalog_generation) {
  const auto path = recovery_directory.AppendASCII(kJournalName);
  auto journal = ReadDictionary(path);
  if (!journal.has_value()) {
    return base::unexpected(journal.error());
  }
  const auto* state = journal->FindString("state");
  if (!state || (*state != "published" && *state != "committed") ||
      catalog_generation < 1 ||
      (*state == "committed" &&
       journal->FindInt("catalog_generation") != catalog_generation)) {
    return Fail(Code::kConflict, path);
  }
  journal->Set("state", "committed");
  journal->Set("catalog_generation", catalog_generation);
  if (!WriteDictionary(path, *journal)) {
    return Fail(Code::kWriteFailed, path);
  }
  return base::ok();
}

SpaceSessionFilterResult FilterSpaceSessionWindows(
    std::vector<std::unique_ptr<sessions::SessionWindow>>* windows,
    const std::string& owning_space_id) {
  SpaceSessionFilterResult result;
  for (auto& window : *windows) {
    std::map<split_tabs::SplitTabId, size_t> original_split_counts;
    std::optional<SessionID> selected_id;
    if (window->selected_tab_index >= 0 &&
        static_cast<size_t>(window->selected_tab_index) < window->tabs.size()) {
      selected_id = window->tabs[window->selected_tab_index]->tab_id;
    }
    for (const auto& tab : window->tabs) {
      if (tab->split_id) {
        ++original_split_counts[*tab->split_id];
      }
    }
    std::erase_if(window->tabs, [&](const auto& tab) {
      const auto found = tab->extra_data.find(kSpaceExtraData);
      const std::string id =
          found == tab->extra_data.end() ? "" : found->second;
      if (!id.empty() && !IsSpaceId(id)) {
        result.invalid_space_ids.push_back(id);
      }
      const bool retain =
          id == owning_space_id && (id.empty() || IsSpaceId(id));
      retain ? ++result.retained_tabs : ++result.removed_tabs;
      return !retain;
    });
    std::set<tab_groups::TabGroupId> groups;
    std::map<split_tabs::SplitTabId, size_t> splits;
    window->selected_tab_index = 0;
    for (size_t i = 0; i < window->tabs.size(); ++i) {
      auto& tab = window->tabs[i];
      tab->tab_visual_index = static_cast<int>(i);
      if (selected_id && tab->tab_id == *selected_id) {
        window->selected_tab_index = static_cast<int>(i);
      }
      if (tab->group) {
        groups.insert(*tab->group);
      }
      if (tab->split_id) {
        ++splits[*tab->split_id];
      }
    }
    for (auto& tab : window->tabs) {
      if (tab->split_id &&
          (splits[*tab->split_id] < 2 ||
           splits[*tab->split_id] != original_split_counts[*tab->split_id])) {
        tab->split_id.reset();
      }
    }
    std::erase_if(window->tab_groups, [&](const auto& group) {
      return !groups.contains(group->id);
    });
    std::erase_if(window->split_tabs, [&](const auto& split) {
      return splits[split->id_] < 2 ||
             splits[split->id_] != original_split_counts[split->id_];
    });
  }
  std::erase_if(*windows,
                [](const auto& window) { return window->tabs.empty(); });
  return result;
}

scoped_refptr<base::SingleThreadTaskRunner> CreateSpaceMigrationTaskRunner() {
  return base::ThreadPool::CreateSingleThreadTaskRunner(
      {base::MayBlock(), base::TaskPriority::USER_BLOCKING,
       base::TaskShutdownBehavior::BLOCK_SHUTDOWN});
}

base::expected<SpaceSessionSeed, SpaceMigrationError>
ReadSpaceMigrationSessionSeed(
    const base::FilePath& cold_profile_directory,
    const std::string& owning_space_id,
    scoped_refptr<os_crypt_async::Encryptor> encryptor) {
  if (!owning_space_id.empty() && !IsSpaceId(owning_space_id)) {
    return Fail(Code::kInvalidPlan, cold_profile_directory);
  }
  if (!base::DirectoryExists(cold_profile_directory) ||
      base::IsLink(cold_profile_directory)) {
    return Fail(Code::kReadFailed, cold_profile_directory);
  }
  base::ScopedTempDir working;
  if (!working.CreateUniqueTempDir()) {
    return Fail(Code::kWriteFailed, cold_profile_directory);
  }
  for (auto directory :
       {sessions::kSessionsDirectory, sessions::kEncryptedSessionsDirectory}) {
    const auto source = cold_profile_directory.Append(directory);
    if (base::PathExists(source) &&
        !CopyTree(source, working.GetPath().Append(directory))) {
      return Fail(Code::kWriteFailed, source);
    }
  }
  const bool encrypted = HasSessionFile(
      working.GetPath().Append(sessions::kEncryptedSessionsDirectory));
  if (encrypted && !encryptor) {
    return Fail(Code::kInvalidSession, cold_profile_directory);
  }
  auto backend = base::MakeRefCounted<sessions::CommandStorageBackend>(
      base::SequencedTaskRunner::GetCurrentDefault(), working.GetPath(),
      sessions::CommandStorageManager::SessionType::kSessionRestore,
      encrypted ? std::move(encryptor) : nullptr);
  auto commands = backend->ReadLastSessionCommands();
  if (sessions::IsCommandStorageReadError(commands.status)) {
    return Fail(Code::kInvalidSession, cold_profile_directory);
  }
  SpaceSessionSeed seed;
  seed.has_complete_native_session =
      commands.status == sessions::CommandStorageReadStatus::kSuccess;
  std::set<SessionID> discarded;
  std::string platform_session_id;
  const auto replay = sessions::RestoreSessionFromCommands(
      commands.commands, &seed.windows, &seed.active_window_id,
      &platform_session_id, &discarded);
  if (replay == sessions::SessionReplayResult::kCorruptedCommand ||
      replay == sessions::SessionReplayResult::kAllTabsPrunedNoNavigations ||
      replay == sessions::SessionReplayResult::kAllWindowsHadNoValidTabs) {
    return Fail(Code::kInvalidSession, cold_profile_directory);
  }
  std::set<std::string> observed;
  for (const auto& window : seed.windows) {
    for (const auto& tab : window->tabs) {
      const auto found = tab->extra_data.find(kSpaceExtraData);
      if (found != tab->extra_data.end() && IsSpaceId(found->second)) {
        observed.insert(found->second);
      }
    }
  }
  seed.observed_space_ids.assign(observed.begin(), observed.end());
  seed.filter = FilterSpaceSessionWindows(&seed.windows, owning_space_id);
  if (!std::ranges::any_of(seed.windows, [&](const auto& window) {
        return window->window_id == seed.active_window_id;
      })) {
    seed.active_window_id = seed.windows.empty()
                                ? SessionID::InvalidValue()
                                : seed.windows.front()->window_id;
  }
  return seed;
}

}  // namespace noventra
