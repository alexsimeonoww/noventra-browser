// Copyright 2026 The Noventra Authors. All rights reserved.
#include "chrome/browser/profiles/noventra_space_migration.h"

#include <string>
#include <utility>

#include "base/files/file_util.h"
#include "base/files/scoped_temp_dir.h"
#include "base/functional/bind.h"
#include "base/functional/callback_helpers.h"
#include "base/memory/scoped_refptr.h"
#include "base/task/sequenced_task_runner.h"
#include "base/test/task_environment.h"
#include "base/test/test_future.h"
#include "build/build_config.h"
#include "components/sessions/core/command_storage_backend.h"
#include "components/sessions/core/session_command.h"
#include "components/sessions/core/session_service_commands.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace noventra {
namespace {

constexpr char kWork[] = "11111111-1111-4111-8111-111111111111";
constexpr char kStudy[] = "22222222-2222-4222-8222-222222222222";

class SpaceMigrationTest : public testing::Test {
 protected:
  void SetUp() override { ASSERT_TRUE(directory_.CreateUniqueTempDir()); }

  SpaceMigrationPlan Plan() {
    SpaceMigrationPlan plan;
    plan.user_data_directory = directory_.GetPath();
    plan.root_profile_basename = base::FilePath(FILE_PATH_LITERAL("Default"));
    plan.targets = {{kWork, base::FilePath(FILE_PATH_LITERAL("Profile 1")),
                     base::FilePath(FILE_PATH_LITERAL(
                         "Storage/ext/noventra-account-spaces/112233445566"))},
                    {kStudy, base::FilePath(FILE_PATH_LITERAL("Profile 2")),
                     base::FilePath(FILE_PATH_LITERAL(
                         "Storage/ext/noventra-account-spaces/AABBCCDDEEFF"))}};
    return plan;
  }

  void Seed(const SpaceMigrationPlan& plan) {
    const auto profile =
        plan.user_data_directory.Append(plan.root_profile_basename);
    ASSERT_TRUE(base::CreateDirectory(profile));
    ASSERT_TRUE(base::WriteFile(profile.AppendASCII("Preferences"), "catalog"));
    ASSERT_TRUE(
        base::WriteFile(profile.AppendASCII("History"), "personal history"));
    ASSERT_TRUE(base::WriteFile(profile.AppendASCII("Bookmarks"),
                                "personal bookmarks"));
    ASSERT_TRUE(base::WriteFile(profile.AppendASCII("Login Data"),
                                "personal passwords"));
    ASSERT_TRUE(
        base::WriteFile(plan.user_data_directory.AppendASCII("Local State"),
                        "encryption metadata"));
    for (const auto& target : plan.targets) {
      const auto partition = profile.Append(target.partition_relative_path);
      ASSERT_TRUE(base::CreateDirectory(partition.AppendASCII("IndexedDB")));
      ASSERT_TRUE(base::WriteFile(partition.AppendASCII("Cookies"), target.id));
      ASSERT_TRUE(base::WriteFile(partition.AppendASCII("IndexedDB/fixture"),
                                  target.id + " idb"));
    }
  }

  std::string Read(const base::FilePath& path) {
    std::string value;
    EXPECT_TRUE(base::ReadFileToString(path, &value));
    return value;
  }

  base::test::TaskEnvironment tasks_;
  base::ScopedTempDir directory_;
};

TEST_F(SpaceMigrationTest, PreservesSourceAndSeparatesDestinationLibraries) {
  const auto plan = Plan();
  Seed(plan);
  auto prepared = PrepareSpaceProfileMigration(plan);
  ASSERT_TRUE(prepared.has_value());
  for (const auto& target : plan.targets) {
    const auto destination =
        plan.user_data_directory.Append(target.profile_basename);
    EXPECT_EQ(Read(destination.Append(target.partition_relative_path)
                       .AppendASCII("Cookies")),
              target.id);
    EXPECT_FALSE(base::PathExists(destination.AppendASCII("History")));
    EXPECT_FALSE(base::PathExists(destination.AppendASCII("Bookmarks")));
    EXPECT_FALSE(base::PathExists(destination.AppendASCII("Login Data")));
    EXPECT_EQ(Read(prepared->snapshot_profile_directory
                       .Append(target.partition_relative_path)
                       .AppendASCII("Cookies")),
              target.id);
    EXPECT_EQ(Read(plan.user_data_directory.Append(plan.root_profile_basename)
                       .Append(target.partition_relative_path)
                       .AppendASCII("Cookies")),
              target.id);
  }
  EXPECT_EQ(Read(prepared->snapshot_profile_directory.AppendASCII("History")),
            "personal history");
  EXPECT_EQ(Read(prepared->recovery_directory.AppendASCII("Local State")),
            "encryption metadata");
  EXPECT_TRUE(
      CommitSpaceProfileMigration(prepared->recovery_directory, 7).has_value());
  const auto destination =
      plan.user_data_directory.Append(plan.targets[0].profile_basename)
          .Append(plan.targets[0].partition_relative_path)
          .AppendASCII("Cookies");
  ASSERT_TRUE(base::WriteFile(destination, "new independent browsing"));
  auto repeated = PrepareSpaceProfileMigration(plan);
  ASSERT_TRUE(repeated.has_value());
  EXPECT_EQ(repeated->committed_catalog_generation, 7);
  EXPECT_EQ(Read(destination), "new independent browsing");
  EXPECT_TRUE(
      CommitSpaceProfileMigration(prepared->recovery_directory, 7).has_value());
  EXPECT_FALSE(
      CommitSpaceProfileMigration(prepared->recovery_directory, 8).has_value());
}

TEST_F(SpaceMigrationTest, ResumesEveryRecordedBoundaryWithoutOverwriting) {
  for (const auto checkpoint : {SpaceMigrationCheckpoint::kSnapshotReady,
                                SpaceMigrationCheckpoint::kTargetsReady,
                                SpaceMigrationCheckpoint::kTargetPublished,
                                SpaceMigrationCheckpoint::kPublished}) {
    base::ScopedTempDir transaction;
    ASSERT_TRUE(transaction.CreateUniqueTempDir());
    auto plan = Plan();
    plan.user_data_directory = transaction.GetPath();
    Seed(plan);
    bool interrupted = false;
    auto first = PrepareSpaceProfileMigration(
        plan, base::BindRepeating(
                  [](SpaceMigrationCheckpoint stop, bool* interrupted,
                     SpaceMigrationCheckpoint current) {
                    if (!*interrupted && stop == current) {
                      *interrupted = true;
                      return false;
                    }
                    return true;
                  },
                  checkpoint, &interrupted));
    ASSERT_FALSE(first.has_value());
    EXPECT_EQ(first.error().code, SpaceMigrationErrorCode::kInterrupted);
    auto resumed = PrepareSpaceProfileMigration(plan);
    ASSERT_TRUE(resumed.has_value());
    EXPECT_TRUE(CommitSpaceProfileMigration(resumed->recovery_directory, 1)
                    .has_value());
    for (const auto& target : plan.targets) {
      EXPECT_EQ(Read(plan.user_data_directory.Append(target.profile_basename)
                         .Append(target.partition_relative_path)
                         .AppendASCII("Cookies")),
                target.id);
    }
  }
}

TEST_F(SpaceMigrationTest, RejectsUnownedDestinationAndMissingSource) {
  const auto plan = Plan();
  Seed(plan);
  const auto alien =
      plan.user_data_directory.Append(plan.targets[0].profile_basename);
  ASSERT_TRUE(base::CreateDirectory(alien));
  ASSERT_TRUE(
      base::WriteFile(alien.AppendASCII("History"), "unrelated profile"));
  auto result = PrepareSpaceProfileMigration(plan);
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error().code, SpaceMigrationErrorCode::kConflict);
  EXPECT_EQ(Read(alien.AppendASCII("History")), "unrelated profile");
  auto missing = Plan();
  missing.root_profile_basename = base::FilePath(FILE_PATH_LITERAL("Missing"));
  EXPECT_FALSE(PrepareSpaceProfileMigration(missing).has_value());
}

TEST_F(SpaceMigrationTest, StopsWhenSourceChangesBeforeSnapshotCommit) {
  const auto plan = Plan();
  Seed(plan);
  auto result = PrepareSpaceProfileMigration(
      plan, base::BindRepeating([](SpaceMigrationCheckpoint current) {
        return current != SpaceMigrationCheckpoint::kSnapshotReady;
      }));
  ASSERT_FALSE(result.has_value());
  const auto source =
      plan.user_data_directory.Append(plan.root_profile_basename);
  ASSERT_TRUE(base::WriteFile(source.AppendASCII("Preferences"),
                              "changed by old runtime"));
  result = PrepareSpaceProfileMigration(plan);
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error().code, SpaceMigrationErrorCode::kSourceChanged);
  EXPECT_EQ(Read(source.AppendASCII("Preferences")), "changed by old runtime");
}

TEST_F(SpaceMigrationTest, RejectsPathTraversalAndDuplicateBindings) {
  auto plan = Plan();
  Seed(plan);
  plan.targets[0].profile_basename =
      base::FilePath(FILE_PATH_LITERAL("../Other"));
  EXPECT_FALSE(PrepareSpaceProfileMigration(plan).has_value());
  plan = Plan();
  plan.targets[1].profile_basename = plan.targets[0].profile_basename;
  EXPECT_FALSE(PrepareSpaceProfileMigration(plan).has_value());
  plan = Plan();
  plan.targets[1].partition_relative_path =
      plan.targets[0].partition_relative_path;
  EXPECT_FALSE(PrepareSpaceProfileMigration(plan).has_value());
}

#if BUILDFLAG(IS_POSIX)
TEST_F(SpaceMigrationTest, RejectsSymlinkBeforeCopyingSource) {
  const auto plan = Plan();
  Seed(plan);
  const auto root = plan.user_data_directory.Append(plan.root_profile_basename);
  ASSERT_TRUE(base::CreateSymbolicLink(root.AppendASCII("History"),
                                       root.AppendASCII("linked-history")));
  auto result = PrepareSpaceProfileMigration(plan);
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error().code, SpaceMigrationErrorCode::kReadFailed);
  EXPECT_EQ(Read(root.AppendASCII("History")), "personal history");
}
#endif

TEST_F(SpaceMigrationTest, FiltersWholeTabsAndPreservesNativeMetadata) {
  std::vector<std::unique_ptr<sessions::SessionWindow>> windows;
  auto window = std::make_unique<sessions::SessionWindow>();
  window->selected_tab_index = 1;
  const auto group = tab_groups::TabGroupId::GenerateNew();
  const auto split = split_tabs::SplitTabId::GenerateNew();
  for (int index = 0; index < 3; ++index) {
    auto tab = std::make_unique<sessions::SessionTab>();
    tab->tab_id = SessionID::FromSerializedValue(index + 1);
    tab->extra_data["noventra.account_space"] = index == 0 ? "" : kWork;
    tab->session_storage_persistent_id = "retained session storage";
    tab->extra_data["noventra.pinned_tab"] = "opaque complete pin metadata";
    tab->pinned = true;
    tab->navigations.resize(2);
    tab->group = group;
    tab->split_id = split;
    window->tabs.push_back(std::move(tab));
  }
  auto* retained = window->tabs[1].get();
  window->tab_groups.push_back(
      std::make_unique<sessions::SessionTabGroup>(group));
  window->split_tabs.push_back(
      std::make_unique<sessions::SessionSplitTab>(split));
  windows.push_back(std::move(window));
  auto result = FilterSpaceSessionWindows(&windows, kWork);
  ASSERT_EQ(windows.size(), 1u);
  ASSERT_EQ(windows[0]->tabs.size(), 2u);
  EXPECT_EQ(windows[0]->tabs[0].get(), retained);
  EXPECT_EQ(windows[0]->selected_tab_index, 0);
  EXPECT_EQ(retained->navigations.size(), 2u);
  EXPECT_EQ(retained->session_storage_persistent_id,
            "retained session storage");
  EXPECT_EQ(retained->extra_data.at("noventra.pinned_tab"),
            "opaque complete pin metadata");
  EXPECT_TRUE(retained->pinned);
  EXPECT_EQ(windows[0]->tab_groups.size(), 1u);
  EXPECT_TRUE(windows[0]->split_tabs.empty());
  EXPECT_FALSE(retained->split_id);
  EXPECT_EQ(result.retained_tabs, 2u);
  EXPECT_EQ(result.removed_tabs, 1u);
}

TEST_F(SpaceMigrationTest, NativeParserLeavesOriginalSessionsUnchanged) {
  const auto profile = directory_.GetPath().AppendASCII("ColdProfile");
  ASSERT_TRUE(base::CreateDirectory(profile));
  {
    auto backend = base::MakeRefCounted<sessions::CommandStorageBackend>(
        base::SequencedTaskRunner::GetCurrentDefault(), profile,
        sessions::CommandStorageManager::SessionType::kSessionRestore, nullptr);
    std::vector<std::unique_ptr<sessions::SessionCommand>> commands;
    const auto window = SessionID::FromSerializedValue(1);
    commands.push_back(sessions::CreateSetWindowTypeCommand(
        window, sessions::SessionWindow::TYPE_NORMAL));
    commands.push_back(sessions::CreateWindowClosedCommand(window));
    backend->AppendCommands(std::move(commands), true, base::DoNothing());
  }
  const auto older = profile.AppendASCII("Sessions/Session_1");
  ASSERT_TRUE(base::WriteFile(older, "older recovery bytes"));
  auto result = ReadSpaceMigrationSessionSeed(profile, "");
  ASSERT_TRUE(result.has_value());
  EXPECT_TRUE(result->has_complete_native_session);
  EXPECT_TRUE(result->windows.empty());
  EXPECT_EQ(Read(older), "older recovery bytes");
  auto repeated = ReadSpaceMigrationSessionSeed(profile, "");
  ASSERT_TRUE(repeated.has_value());
  EXPECT_TRUE(repeated->has_complete_native_session);
}

TEST_F(SpaceMigrationTest, NativeParserRunsOnStartupWorker) {
  const auto profile = directory_.GetPath().AppendASCII("ColdProfile");
  ASSERT_TRUE(base::CreateDirectory(profile));
  auto runner = CreateSpaceMigrationTaskRunner();
  EXPECT_FALSE(runner->RunsTasksInCurrentSequence());
  base::test::TestFuture<base::expected<SpaceSessionSeed, SpaceMigrationError>>
      future;
  ASSERT_TRUE(runner->PostTaskAndReplyWithResult(
      FROM_HERE,
      base::BindOnce(&ReadSpaceMigrationSessionSeed, profile, std::string(),
                     scoped_refptr<os_crypt_async::Encryptor>()),
      future.GetCallback()));
  auto result = future.Take();
  ASSERT_TRUE(result.has_value());
  EXPECT_FALSE(result->has_complete_native_session);
}

TEST_F(SpaceMigrationTest, MissingSessionIsDifferentFromCorruption) {
  const auto profile = directory_.GetPath().AppendASCII("ColdProfile");
  ASSERT_TRUE(base::CreateDirectory(profile));
  auto result = ReadSpaceMigrationSessionSeed(profile, kWork);
  ASSERT_TRUE(result.has_value());
  EXPECT_FALSE(result->has_complete_native_session);
  ASSERT_TRUE(base::CreateDirectory(profile.AppendASCII("Sessions")));
  ASSERT_TRUE(base::WriteFile(profile.AppendASCII("Sessions/Session_1"),
                              "corrupt session"));
  result = ReadSpaceMigrationSessionSeed(profile, kWork);
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error().code, SpaceMigrationErrorCode::kInvalidSession);
  EXPECT_EQ(Read(profile.AppendASCII("Sessions/Session_1")), "corrupt session");
}

}  // namespace
}  // namespace noventra
