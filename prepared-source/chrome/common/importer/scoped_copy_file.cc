/* Copyright 2020 The Brave Authors, 2025 imput. All rights reserved.
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "chrome/common/importer/scoped_copy_file.h"

#include <algorithm>
#include <array>
#include <optional>
#include <utility>
#include <vector>

#include "base/containers/span.h"
#include "base/files/file.h"
#include "base/files/file_util.h"
#include "base/functional/callback.h"
#include "build/build_config.h"

namespace {

// Large histories are copied in fixed-size chunks, never retained in memory.
// Cap the combined on-disk snapshot instead of exhausting temporary storage.
constexpr int64_t kMaximumDatabaseBytes = int64_t{2} * 1024 * 1024 * 1024;

bool SameFileState(const base::File::Info& first,
                   const base::File::Info& second) {
  return !second.is_directory && !second.is_symbolic_link &&
         first.size == second.size &&
         first.last_modified == second.last_modified;
}

bool CopyBoundedFile(const base::FilePath& source,
                     const base::FilePath& destination,
                     const base::File::Info& before) {
  uint32_t read_flags = base::File::FLAG_OPEN | base::File::FLAG_READ |
                        base::File::FLAG_WIN_SHARE_DELETE;
#if BUILDFLAG(IS_POSIX)
  read_flags |= base::File::FLAG_NO_FOLLOW;
#endif
  base::File input(source, read_flags);
  base::File output(destination,
                    base::File::FLAG_CREATE | base::File::FLAG_WRITE);
  base::File::Info opened;
  if (!input.IsValid() || !output.IsValid() || !input.GetInfo(&opened) ||
      !SameFileState(before, opened)) {
    return false;
  }
  std::array<uint8_t, 64 * 1024> bytes;
  for (int64_t remaining = before.size; remaining > 0;) {
    const size_t length = static_cast<size_t>(
        std::min(remaining, static_cast<int64_t>(bytes.size())));
    const auto block = base::span(bytes).first(length);
    if (!input.ReadAtCurrentPosAndCheck(block) ||
        !output.WriteAtCurrentPosAndCheck(block)) {
      return false;
    }
    remaining -= static_cast<int64_t>(length);
  }
  std::array<uint8_t, 1> extra;
  const auto count = input.ReadAtCurrentPos(extra);
  if (!count || *count != 0) {
    return false;
  }
#if BUILDFLAG(IS_POSIX)
  if (!base::SetPosixFilePermissions(destination, 0600)) {
    return false;
  }
#endif
  return true;
}

bool SnapshotDatabase(const base::FilePath& source,
                      const base::FilePath& destination,
                      base::OnceClosure before_verification_for_testing) {
  struct Part {
    base::FilePath source;
    base::FilePath destination;
    std::optional<base::File::Info> before;
  };
  std::vector<Part> parts;
  int64_t total_bytes = 0;
  for (const auto* suffix : {FILE_PATH_LITERAL(""), FILE_PATH_LITERAL("-wal"),
                             FILE_PATH_LITERAL("-journal")}) {
    const base::FilePath path(source.value() + suffix);
    Part part{path, base::FilePath(destination.value() + suffix), std::nullopt};
    if (base::PathExists(path)) {
      base::File::Info info;
      if (base::IsLink(path) || !base::GetFileInfo(path, &info) ||
          info.is_directory || info.is_symbolic_link || info.size < 0 ||
          info.size > kMaximumDatabaseBytes - total_bytes) {
        return false;
      }
      total_bytes += info.size;
      part.before = info;
    } else if (suffix[0] == 0) {
      return false;
    }
    parts.push_back(std::move(part));
  }
  for (const auto& part : parts) {
    if (part.before &&
        !CopyBoundedFile(part.source, part.destination, *part.before)) {
      return false;
    }
  }
  if (before_verification_for_testing) {
    std::move(before_verification_for_testing).Run();
  }
  for (const auto& part : parts) {
    if (base::PathExists(part.source) != part.before.has_value()) {
      return false;
    }
    if (part.before) {
      base::File::Info after;
      if (base::IsLink(part.source) ||
          !base::GetFileInfo(part.source, &after) ||
          !SameFileState(*part.before, after) ||
          !base::ContentsEqual(part.source, part.destination) ||
          !base::GetFileInfo(part.source, &after) ||
          !SameFileState(*part.before, after)) {
        return false;
      }
    }
  }
  return true;
}

}  // namespace

ScopedCopyFile::ScopedCopyFile(const base::FilePath& original_file_path,
                               Mode mode)
    : ScopedCopyFile(original_file_path, mode, {}) {}

ScopedCopyFile::ScopedCopyFile(
    const base::FilePath& original_file_path,
    Mode mode,
    base::OnceClosure before_verification_for_testing) {
  if (mode == Mode::kSqliteDatabase) {
    if (!copied_directory_.CreateUniqueTempDir()) {
      return;
    }
#if BUILDFLAG(IS_POSIX)
    if (!base::SetPosixFilePermissions(copied_directory_.GetPath(), 0700)) {
      return;
    }
#endif
    copied_file_path_ =
        copied_directory_.GetPath().Append(original_file_path.BaseName());
    copy_success_ =
        SnapshotDatabase(original_file_path, copied_file_path_,
                         std::move(before_verification_for_testing));
    return;
  }
  DCHECK(base::PathExists(original_file_path));
  if (base::CreateTemporaryFile(&copied_file_path_)) {
    copy_success_ = base::CopyFile(original_file_path, copied_file_path_);
  }
}

ScopedCopyFile::~ScopedCopyFile() {
  // ScopedTempDir also removes any SQLite recovery/shared-memory sidecars.
  if (!copied_directory_.IsValid() && base::PathExists(copied_file_path_)) {
    base::DeleteFile(copied_file_path_);
  }
}
