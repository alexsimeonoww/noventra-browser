/* Copyright (c) 2018 The Brave Authors, 2025 imput. All rights reserved.
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "chrome/common/importer/chrome_importer_utils.h"

#include "base/base_paths.h"
#include "base/environment.h"
#include "base/files/file_path.h"
#include "base/nix/xdg_util.h"
#include "base/path_service.h"

namespace {
base::FilePath ConfigFolder() {
  auto env = base::Environment::Create();
  auto config = base::nix::GetXDGDirectory(
      env.get(), base::nix::kXdgConfigHomeEnvVar, base::nix::kDotConfigDir);
  if (config.IsAbsolute()) {
    return config;
  }
  base::FilePath home;
  return base::PathService::Get(base::DIR_HOME, &home)
             ? home.Append(".config")
             : base::FilePath();
}

base::FilePath BrowserFolder(const char* name) {
  auto config = ConfigFolder();
  return config.empty() ? base::FilePath() : config.AppendASCII(name);
}
}  // namespace

base::FilePath GetChromeUserDataFolder() {
  return BrowserFolder("google-chrome");
}

base::FilePath GetChromeBetaUserDataFolder() {
  return BrowserFolder("google-chrome-beta");
}

base::FilePath GetChromeDevUserDataFolder() {
  return BrowserFolder("google-chrome-unstable");
}

base::FilePath GetVivaldiUserDataFolder() {
  return BrowserFolder("vivaldi");
}

base::FilePath GetChromiumUserDataFolder() {
  return BrowserFolder("chromium");
}

base::FilePath GetEdgeUserDataFolder() {
  return BrowserFolder("microsoft-edge");
}

base::FilePath GetOperaUserDataFolder() {
  return BrowserFolder("opera");
}

base::FilePath GetYandexUserDataFolder() {
  return BrowserFolder("yandex-browser");
}

base::FilePath GetWhaleUserDataFolder() {
  return BrowserFolder("naver-whale");
}

base::FilePath GetBraveUserDataFolder() {
  return BrowserFolder("BraveSoftware/Brave-Browser");
}

base::FilePath GetOperaSnapUserDataFolder() {
  base::FilePath home;
  if (!base::PathService::Get(base::DIR_HOME, &home)) {
    return base::FilePath();
  }
  return home.Append("snap/opera/current/.config/opera");
}
