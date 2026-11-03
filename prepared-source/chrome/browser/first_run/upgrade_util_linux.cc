// Copyright 2011 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/first_run/upgrade_util.h"

#include "base/base_paths.h"
#include "base/command_line.h"
#include "base/files/file_path.h"
#include "base/files/file_util.h"
#include "base/logging.h"
#include "base/path_service.h"
#include "base/process/launch.h"
#include "base/process/process_handle.h"
#include "base/strings/string_number_conversions.h"
#include "chrome/browser/first_run/upgrade_util_linux.h"

namespace {

double saved_last_modified_time_of_exe = 0;

}  // namespace

namespace upgrade_util {

bool RelaunchChromeBrowserImpl(const base::CommandLine& command_line) {
  base::LaunchOptions options;
  // Don't set NO_NEW_PRIVS on the relaunched browser process.
  options.allow_new_privs = true;
  base::FilePath executable;
  if (!base::PathService::Get(base::FILE_EXE, &executable)) {
    return base::LaunchProcess(command_line, options).IsValid();
  }
  const auto updater =
      executable.DirName().DirName().DirName().AppendASCII("updater");
  const auto helper = updater.AppendASCII("desktop_updater.py");
  if (base::PathExists(updater.AppendASCII("config.json")) &&
      base::PathExists(helper)) {
    // The bundled helper waits for this process before replacing resources.
    // Chromium's filtered relaunch arguments retain session restoration.
    base::CommandLine relaunch(base::FilePath("/usr/bin/python3"));
    relaunch.AppendArg("-I");
    relaunch.AppendArg("-B");
    relaunch.AppendArgPath(helper);
    relaunch.AppendArg("--owner-pid");
    relaunch.AppendArg(base::NumberToString(base::GetCurrentProcId()));
    relaunch.AppendArg("relaunch");
    relaunch.AppendArg("--");
    const auto& arguments = command_line.argv();
    for (size_t i = 1; i < arguments.size(); ++i) {
      relaunch.AppendArg(arguments[i]);
    }
    if (base::LaunchProcess(relaunch, options).IsValid()) {
      return true;
    }
  }
  return base::LaunchProcess(command_line, options).IsValid();
}

bool IsUpdatePendingRestart() {
  return saved_last_modified_time_of_exe != GetLastModifiedTimeOfExe();
}

void SaveLastModifiedTimeOfExe() {
  saved_last_modified_time_of_exe = GetLastModifiedTimeOfExe();
}

double GetLastModifiedTimeOfExe() {
  base::FilePath exe_file_path;
  if (!base::PathService::Get(base::FILE_EXE, &exe_file_path)) {
    LOG(WARNING) << "Failed to get base::FilePath object for FILE_EXE.";
    return saved_last_modified_time_of_exe;
  }
  base::File::Info exe_file_info;
  if (!base::GetFileInfo(exe_file_path, &exe_file_info)) {
    LOG(WARNING) << "Failed to get FileInfo object for FILE_EXE - "
                 << exe_file_path.value();
    return saved_last_modified_time_of_exe;
  }
  return exe_file_info.last_modified.InSecondsFSinceUnixEpoch();
}

}  // namespace upgrade_util
