// Copyright 2026 The Noventra Authors. All rights reserved.
#ifndef CHROME_BROWSER_PROFILES_NOVENTRA_SPACE_MIGRATION_H_
#define CHROME_BROWSER_PROFILES_NOVENTRA_SPACE_MIGRATION_H_

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "base/files/file_path.h"
#include "base/functional/callback.h"
#include "base/memory/scoped_refptr.h"
#include "base/task/single_thread_task_runner.h"
#include "base/types/expected.h"
#include "components/os_crypt/async/common/encryptor.h"
#include "components/sessions/core/session_id.h"
#include "components/sessions/core/session_types.h"

namespace noventra {

// The native session backend captures a single-thread callback runner when
// constructed, including during cold reads that never post a callback.
scoped_refptr<base::SingleThreadTaskRunner> CreateSpaceMigrationTaskRunner();

enum class SpaceMigrationErrorCode {
  kInvalidPlan,
  kSourceChanged,
  kConflict,
  kReadFailed,
  kWriteFailed,
  kInsufficientSpace,
  kInterrupted,
  kInvalidSession,
};

struct SpaceMigrationError {
  SpaceMigrationErrorCode code;
  base::FilePath path;
};

struct SpaceMigrationTarget {
  std::string id;
  base::FilePath profile_basename;
  // Obtained from the engine's storage-partition path helper. Never derive
  // this path from an account label or duplicate the engine's hash algorithm.
  base::FilePath partition_relative_path;
};

struct SpaceMigrationPlan {
  base::FilePath user_data_directory;
  base::FilePath root_profile_basename;
  std::vector<SpaceMigrationTarget> targets;
};

enum class SpaceMigrationCheckpoint {
  kSnapshotReady,
  kTargetsReady,
  kTargetPublished,
  kPublished,
};

struct SpaceMigrationPrepared {
  base::FilePath recovery_directory;
  base::FilePath snapshot_profile_directory;
  std::vector<SpaceMigrationTarget> targets;
  std::optional<int> committed_catalog_generation;
};

// Run only while holding the user-data singleton and before initializing ANY
// Profile. This blocking operation copies source bytes; it never moves or
// deletes source data. A failure must block cutover, not open an empty profile.
// The optional checkpoint is fault injection for disposable regression tests.
base::expected<SpaceMigrationPrepared, SpaceMigrationError>
PrepareSpaceProfileMigration(
    const SpaceMigrationPlan& plan,
    base::RepeatingCallback<bool(SpaceMigrationCheckpoint)> checkpoint = {});

// Call only after the exact prepared bindings have been committed through
// Local State's CommitPendingWrite callback. SetDict/Save scheduling alone is
// not a durable acknowledgement. Recovery files remain after this transition.
base::expected<void, SpaceMigrationError> CommitSpaceProfileMigration(
    const base::FilePath& recovery_directory,
    int catalog_generation);

struct SpaceSessionFilterResult {
  size_t retained_tabs = 0;
  size_t removed_tabs = 0;
  std::vector<std::string> invalid_space_ids;
};

// Move complete native SessionTab objects, preserving navigation stacks, pins,
// and sessionStorage IDs. Personal owns the empty identity. Foreign/invalid
// tabs are excluded before any WebContents can be created for this Profile.
SpaceSessionFilterResult FilterSpaceSessionWindows(
    std::vector<std::unique_ptr<sessions::SessionWindow>>* windows,
    const std::string& owning_space_id);

struct SpaceSessionSeed {
  std::vector<std::unique_ptr<sessions::SessionWindow>> windows;
  SessionID active_window_id = SessionID::InvalidValue();
  SpaceSessionFilterResult filter;
  std::vector<std::string> observed_space_ids;
  bool has_complete_native_session = false;
};

// Blocking, on a sequenced task runner. The argument is a cold profile or
// immutable recovery profile. Native session parsing can delete older files,
// so it runs only on a disposable COPY of its session directories. A complete
// native session, including a valid empty session, supersedes a recovery seed.
base::expected<SpaceSessionSeed, SpaceMigrationError>
ReadSpaceMigrationSessionSeed(
    const base::FilePath& cold_profile_directory,
    const std::string& owning_space_id,
    scoped_refptr<os_crypt_async::Encryptor> encryptor = nullptr);

}  // namespace noventra

#endif  // CHROME_BROWSER_PROFILES_NOVENTRA_SPACE_MIGRATION_H_
