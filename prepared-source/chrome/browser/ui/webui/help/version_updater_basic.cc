// Copyright 2012 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <memory>
#include <string>

#include "build/build_config.h"
#include "chrome/browser/ui/webui/help/version_updater.h"
#include "chrome/browser/upgrade_detector/upgrade_detector.h"

#if BUILDFLAG(IS_LINUX)
#include "base/base_paths.h"
#include "base/command_line.h"
#include "base/files/file_util.h"
#include "base/functional/bind.h"
#include "base/json/json_reader.h"
#include "base/memory/weak_ptr.h"
#include "base/path_service.h"
#include "base/process/launch.h"
#include "base/process/process_handle.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/utf_string_conversions.h"
#include "base/task/thread_pool.h"
#endif

namespace {

#if BUILDFLAG(IS_LINUX)
struct DesktopUpdateResult {
  VersionUpdater::Status status = VersionUpdater::FAILED;
  std::string version;
  std::u16string message =
      u"Could not check for updates. Updates require Python 3.11 or newer "
      u"and OpenSSL.";
};

DesktopUpdateResult CheckDesktopUpdate(const base::FilePath& helper,
                                       VersionUpdater::Status fallback) {
  DesktopUpdateResult result;
  if (!base::PathExists(helper)) {
    result.status = fallback;
    result.message.clear();
    return result;
  }
  // No shell, URL or executable supplied by the feed. The helper and pinned
  // public key are inventoried files beside this installed executable.
  base::CommandLine command(base::FilePath("/usr/bin/python3"));
  command.AppendArg("-I");
  command.AppendArg("-B");
  command.AppendArgPath(helper);
  // A late-starting child must not bind to a supervisor after this process
  // exits.
  command.AppendArg("--owner-pid");
  command.AppendArg(base::NumberToString(base::GetCurrentProcId()));
  command.AppendArg("check");
  std::string output;
  int exit_code = -1;
  if (!base::GetAppOutputWithExitCode(command, &output, &exit_code) ||
      output.size() > 64 * 1024) {
    return result;
  }
  const auto value = base::JSONReader::ReadDict(output, base::JSON_PARSE_RFC);
  if (!value) {
    return result;
  }
  if (const auto* message = value->FindString("message")) {
    result.message = base::UTF8ToUTF16(*message);
  }
  const auto* status = value->FindString("status");
  if (exit_code != 0 || !status) {
    return result;
  }
  if (*status == "up_to_date" || *status == "applied") {
    result.status = VersionUpdater::UPDATED;
  } else if (*status == "ready" || *status == "busy") {
    // Chromium's Relaunch invokes the executable directly. Applying is done
    // by the coordinated package launcher after every window has closed.
    // DEFERRED displays the helper's honest close/reopen instruction instead
    // of offering a Relaunch button that would bypass that launcher.
    result.status = VersionUpdater::DEFERRED;
    if (const auto* version = value->FindString("version")) {
      result.version = *version;
    }
  }
  return result;
}
#endif

// Bare bones implementation just checks if a new version is ready.
class VersionUpdaterBasic : public VersionUpdater {
 public:
  VersionUpdaterBasic(const VersionUpdaterBasic&) = delete;
  VersionUpdaterBasic& operator=(const VersionUpdaterBasic&) = delete;
  VersionUpdaterBasic() = default;
  ~VersionUpdaterBasic() override = default;

  // VersionUpdater implementation.
  void CheckForUpdate(StatusCallback callback, PromoteCallback) override {
#if BUILDFLAG(IS_LINUX)
    base::FilePath executable;
    if (base::PathService::Get(base::FILE_EXE, &executable)) {
      const auto helper = executable.DirName()
                              .DirName()
                              .DirName()
                              .AppendASCII("updater")
                              .AppendASCII("desktop_updater.py");
      callback.Run(CHECKING, 0, false, false, std::string(), 0,
                   std::u16string());
      if (!checking_) {
        checking_ = true;
        const Status fallback =
            UpgradeDetector::GetInstance()->is_upgrade_available()
                ? NEARLY_UPDATED
                : DISABLED;
        base::ThreadPool::PostTaskAndReplyWithResult(
            FROM_HERE,
            {base::MayBlock(), base::TaskPriority::USER_VISIBLE,
             // Only local helper I/O runs here; the weak reply stays on the UI
             // sequence. The parent-death-bound helper must not block quitting.
             base::TaskShutdownBehavior::CONTINUE_ON_SHUTDOWN},
            base::BindOnce(&CheckDesktopUpdate, helper, fallback),
            base::BindOnce(&VersionUpdaterBasic::OnDesktopChecked,
                           weak_factory_.GetWeakPtr(), callback));
      }
      return;
    }
#endif
    const Status status = UpgradeDetector::GetInstance()->is_upgrade_available()
                              ? NEARLY_UPDATED
                              : DISABLED;
    callback.Run(status, 0, false, false, std::string(), 0, std::u16string());
  }

#if BUILDFLAG(IS_LINUX)
 private:
  void OnDesktopChecked(StatusCallback callback, DesktopUpdateResult result) {
    checking_ = false;
    callback.Run(result.status, 0, false, false, result.version, 0,
                 result.message);
  }

  bool checking_ = false;
  base::WeakPtrFactory<VersionUpdaterBasic> weak_factory_{this};
#endif
};

}  // namespace

std::unique_ptr<VersionUpdater> VersionUpdater::Create(
    content::WebContents* web_contents) {
  return std::make_unique<VersionUpdaterBasic>();
}
