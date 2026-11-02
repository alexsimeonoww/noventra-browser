/* Copyright 2020 The Brave Authors, 2025 imput. All rights reserved.
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef BRAVE_COMMON_IMPORTER_SCOPED_COPY_FILE_H_
#define BRAVE_COMMON_IMPORTER_SCOPED_COPY_FILE_H_

#include "base/files/file_path.h"
#include "base/files/scoped_temp_dir.h"
#include "base/functional/callback_forward.h"
#include "base/gtest_prod_util.h"

class ScopedCopyFile {
 public:
  enum class Mode {
    kFile,
    // Snapshot the main file plus WAL/journal into a private directory. Never
    // open the original in SQLite; reject sources that change during copying.
    kSqliteDatabase,
  };

  explicit ScopedCopyFile(const base::FilePath& original_file_path,
                          Mode mode = Mode::kFile);
  ~ScopedCopyFile();

  ScopedCopyFile(const ScopedCopyFile&) = delete;
  ScopedCopyFile& operator=(const ScopedCopyFile&) = delete;

  bool copy_success() const { return copy_success_; }
  base::FilePath copied_file_path() const { return copied_file_path_; }

 private:
  FRIEND_TEST_ALL_PREFIXES(ScopedCopyFileTest,
                           SourceChangingDuringCopyIsRejected);
  FRIEND_TEST_ALL_PREFIXES(ScopedCopyFileTest,
                           SidecarAppearingDuringCopyIsRejected);
  ScopedCopyFile(const base::FilePath& original_file_path,
                 Mode mode,
                 base::OnceClosure before_verification_for_testing);

  bool copy_success_ = false;
  base::FilePath copied_file_path_;
  base::ScopedTempDir copied_directory_;
};

#endif  // BRAVE_COMMON_IMPORTER_SCOPED_COPY_FILE_H_
