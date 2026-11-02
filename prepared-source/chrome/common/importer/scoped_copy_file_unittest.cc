// Copyright 2026 Noventra contributors
// SPDX-License-Identifier: GPL-3.0-only

#include "chrome/common/importer/scoped_copy_file.h"

#include <map>
#include <string>
#include <utility>

#include "base/files/file.h"
#include "base/files/file_util.h"
#include "base/files/scoped_temp_dir.h"
#include "base/test/bind.h"
#include "base/test/task_environment.h"
#include "build/build_config.h"
#include "sql/database.h"
#include "sql/statement.h"
#include "sql/test/test_helpers.h"
#include "testing/gtest/include/gtest/gtest.h"

class ScopedCopyFileTest : public testing::Test {
 protected:
  void SetUp() override { ASSERT_TRUE(source_.CreateUniqueTempDir()); }

  base::FilePath Source() const {
    return source_.GetPath().AppendASCII("History");
  }

  auto SourceBytes() {
    std::map<base::FilePath::StringType, std::string> result;
    for (const auto* suffix :
         {FILE_PATH_LITERAL(""), FILE_PATH_LITERAL("-wal"),
          FILE_PATH_LITERAL("-journal"), FILE_PATH_LITERAL("-shm")}) {
      const base::FilePath path(Source().value() + suffix);
      if (base::PathExists(path)) {
        std::string data;
        EXPECT_TRUE(base::ReadFileToString(path, &data));
        result[path.BaseName().value()] = std::move(data);
      }
    }
    return result;
  }

  base::test::TaskEnvironment task_environment_;
  base::ScopedTempDir source_;
};

TEST_F(ScopedCopyFileTest, DefaultModeRemainsSingleFile) {
  ASSERT_TRUE(base::WriteFile(Source(), "owned main"));
  ASSERT_TRUE(base::WriteFile(
      base::FilePath(Source().value() + FILE_PATH_LITERAL("-wal")),
      "owned sidecar"));
  base::FilePath copied;
  {
    ScopedCopyFile copy(Source());
    ASSERT_TRUE(copy.copy_success());
    copied = copy.copied_file_path();
    EXPECT_TRUE(base::ContentsEqual(Source(), copied));
    EXPECT_FALSE(base::PathExists(
        base::FilePath(copied.value() + FILE_PATH_LITERAL("-wal"))));
  }
  EXPECT_FALSE(base::PathExists(copied));
}

TEST_F(ScopedCopyFileTest, MissingDatabaseFailsWithoutCreatingSource) {
  ScopedCopyFile copy(Source(), ScopedCopyFile::Mode::kSqliteDatabase);
  EXPECT_FALSE(copy.copy_success());
  EXPECT_FALSE(base::PathExists(Source()));
}

TEST_F(ScopedCopyFileTest, WalOnlyCommittedRowsAreCopiedAndSourceIsUnchanged) {
  sql::Database source(
      sql::DatabaseOptions().set_wal_mode(true).set_exclusive_locking(false),
      sql::test::kTestTag);
  ASSERT_TRUE(source.Open(Source()));
  ASSERT_TRUE(source.Execute("PRAGMA wal_autocheckpoint=0"));
  ASSERT_TRUE(source.Execute("CREATE TABLE owned_history (value INTEGER)"));
  ASSERT_TRUE(source.Execute("INSERT INTO owned_history VALUES (42)"));
  ASSERT_TRUE(base::PathExists(
      base::FilePath(Source().value() + FILE_PATH_LITERAL("-wal"))));
  const auto before = SourceBytes();
  base::FilePath copied_directory;
  {
    ScopedCopyFile copy(Source(), ScopedCopyFile::Mode::kSqliteDatabase);
    ASSERT_TRUE(copy.copy_success());
    copied_directory = copy.copied_file_path().DirName();
    EXPECT_NE(copied_directory, source_.GetPath());
    sql::Database copied(sql::test::kTestTag);
    ASSERT_TRUE(copied.Open(copy.copied_file_path()));
    sql::Statement rows(
        copied.GetUniqueStatement("SELECT value FROM owned_history"));
    ASSERT_TRUE(rows.Step());
    EXPECT_EQ(rows.ColumnInt(0), 42);
    EXPECT_FALSE(rows.Step());
    EXPECT_TRUE(rows.Succeeded());
    EXPECT_EQ(before, SourceBytes());
  }
  EXPECT_FALSE(base::PathExists(copied_directory));
  EXPECT_EQ(before, SourceBytes());
}

TEST_F(ScopedCopyFileTest, JournalIsCopiedWithoutTouchingSource) {
  ASSERT_TRUE(base::WriteFile(Source(), "owned main"));
  const base::FilePath journal(Source().value() +
                               FILE_PATH_LITERAL("-journal"));
  ASSERT_TRUE(base::WriteFile(journal, "owned rollback journal"));
  const auto before = SourceBytes();
  ScopedCopyFile copy(Source(), ScopedCopyFile::Mode::kSqliteDatabase);
  ASSERT_TRUE(copy.copy_success());
  EXPECT_TRUE(base::ContentsEqual(
      journal, base::FilePath(copy.copied_file_path().value() +
                              FILE_PATH_LITERAL("-journal"))));
  EXPECT_EQ(before, SourceBytes());
}

TEST_F(ScopedCopyFileTest, SourceChangingDuringCopyIsRejected) {
  ASSERT_TRUE(base::WriteFile(Source(), "owned before"));
  base::File::Info before;
  ASSERT_TRUE(base::GetFileInfo(Source(), &before));
  ScopedCopyFile copy(
      Source(), ScopedCopyFile::Mode::kSqliteDatabase,
      base::BindLambdaForTesting([&] {
        EXPECT_TRUE(base::WriteFile(Source(), "owned change"));
        // Same size/time ensures the streaming content comparison catches it.
        EXPECT_TRUE(base::TouchFile(Source(), before.last_accessed,
                                    before.last_modified));
      }));
  EXPECT_FALSE(copy.copy_success());
}

TEST_F(ScopedCopyFileTest, SidecarAppearingDuringCopyIsRejected) {
  ASSERT_TRUE(base::WriteFile(Source(), "owned main"));
  ScopedCopyFile copy(
      Source(), ScopedCopyFile::Mode::kSqliteDatabase,
      base::BindLambdaForTesting([&] {
        EXPECT_TRUE(base::WriteFile(
            base::FilePath(Source().value() + FILE_PATH_LITERAL("-wal")),
            "owned new WAL"));
      }));
  EXPECT_FALSE(copy.copy_success());
}

TEST_F(ScopedCopyFileTest, OversizedSparseDatabaseIsRejectedBeforeCopying) {
  base::File source(Source(), base::File::FLAG_CREATE | base::File::FLAG_WRITE);
  ASSERT_TRUE(source.IsValid());
  ASSERT_TRUE(source.SetLength(int64_t{2} * 1024 * 1024 * 1024 + 1));
  source.Close();
  ScopedCopyFile copy(Source(), ScopedCopyFile::Mode::kSqliteDatabase);
  EXPECT_FALSE(copy.copy_success());
  EXPECT_FALSE(base::PathExists(copy.copied_file_path()));
}

#if BUILDFLAG(IS_POSIX)
TEST_F(ScopedCopyFileTest, SnapshotPermissionsArePrivate) {
  ASSERT_TRUE(base::WriteFile(Source(), "owned main"));
  ScopedCopyFile copy(Source(), ScopedCopyFile::Mode::kSqliteDatabase);
  ASSERT_TRUE(copy.copy_success());
  int mode = 0;
  EXPECT_TRUE(base::GetPosixFilePermissions(copy.copied_file_path(), &mode));
  EXPECT_EQ(mode, 0600);
  EXPECT_TRUE(
      base::GetPosixFilePermissions(copy.copied_file_path().DirName(), &mode));
  EXPECT_EQ(mode, 0700);
}

TEST_F(ScopedCopyFileTest, LinkedDatabaseIsRejected) {
  const base::FilePath actual = source_.GetPath().AppendASCII("actual");
  ASSERT_TRUE(base::WriteFile(actual, "owned main"));
  ASSERT_TRUE(base::CreateSymbolicLink(actual, Source()));
  ScopedCopyFile copy(Source(), ScopedCopyFile::Mode::kSqliteDatabase);
  EXPECT_FALSE(copy.copy_success());
}
#endif
