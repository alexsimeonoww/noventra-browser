/* Copyright (c) 2019 The Brave Authors, 2025 imput. All rights reserved.
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at https://mozilla.org/MPL/2.0/. */

#include "chrome/browser/importer/importer_list.h"

#include <algorithm>
#include <initializer_list>

#include "base/check.h"
#include "base/files/file_path.h"
#include "base/files/file_util.h"
#include "base/base_paths.h"
#include "base/path_service.h"
#include "base/strings/strcat.h"
#include "base/strings/utf_string_conversions.h"
#include "base/threading/scoped_blocking_call.h"
#include "base/values.h"
#include "chrome/common/importer/chrome_importer_utils.h"
#include "chrome/common/importer/importer_constants.h"
#include "chrome/grit/generated_resources.h"
#include "components/user_data_importer/common/importer_type.h"
#include "ui/base/l10n/l10n_util.h"

namespace {
void AddChromeToProfiles(
    std::vector<user_data_importer::SourceProfile>* profiles,
    base::ListValue chrome_profiles,
    const base::FilePath& user_data_folder,
    const std::string& brand,
    user_data_importer::ImporterType type) {
  for (const auto& value : chrome_profiles) {
    const auto* dict = value.GetIfDict();
    if (!dict) {
      continue;
    }
    uint16_t items = user_data_importer::NONE;
    auto* profile = dict->FindString("id");
    auto* name = dict->FindString("name");
    if (!profile || !name) {
      continue;
    }
    base::FilePath path = user_data_folder.Append(
        base::FilePath::FromUTF8Unsafe(*profile));
    if (!ChromeImporterCanImport(path, type, &items) &&
        !base::PathExists(path.AppendASCII("Preferences"))) {
      continue;
    }
    path = base::MakeAbsoluteFilePath(path);
    if (path.empty() || std::ranges::any_of(*profiles, [&](const auto& existing) {
          return existing.source_path == path;
        })) {
      continue;
    }
    user_data_importer::SourceProfile chrome;
    chrome.importer_name = base::UTF8ToUTF16(brand);
    chrome.importer_type = type;
    chrome.services_supported = items;
    chrome.source_path = path;
    chrome.profile = base::UTF8ToUTF16(*name);
    profiles->push_back(chrome);
  }
}

void DetectChromeProfiles(
    std::vector<user_data_importer::SourceProfile>* profiles) {
  base::ScopedBlockingCall scoped_blocking_call(FROM_HERE,
                                                base::BlockingType::WILL_BLOCK);
  auto add_location = [&](const base::FilePath& root, const std::string& brand,
                          user_data_importer::ImporterType type) {
    if (!root.IsAbsolute()) {
      return;
    }
    AddChromeToProfiles(
        profiles, GetChromeSourceProfiles(root.AppendASCII("Local State")),
        root, brand, type);
  };
  AddChromeToProfiles(
      profiles,
      GetChromeSourceProfiles(GetChromeUserDataFolder().Append(
          base::FilePath::StringType(FILE_PATH_LITERAL("Local State")))),
      GetChromeUserDataFolder(), kGoogleChromeBrowser,
      user_data_importer::TYPE_CHROME);
  AddChromeToProfiles(
      profiles,
      GetChromeSourceProfiles(GetChromeBetaUserDataFolder().Append(
          base::FilePath::StringType(FILE_PATH_LITERAL("Local State")))),
      GetChromeBetaUserDataFolder(), kGoogleChromeBrowserBeta,
      user_data_importer::TYPE_CHROME);
  AddChromeToProfiles(
      profiles,
      GetChromeSourceProfiles(GetChromeDevUserDataFolder().Append(
          base::FilePath::StringType(FILE_PATH_LITERAL("Local State")))),
      GetChromeDevUserDataFolder(), kGoogleChromeBrowserDev,
      user_data_importer::TYPE_CHROME);
#if !BUILDFLAG(IS_LINUX)
  AddChromeToProfiles(
      profiles,
      GetChromeSourceProfiles(GetCanaryUserDataFolder().Append(
          base::FilePath::StringType(FILE_PATH_LITERAL("Local State")))),
      GetCanaryUserDataFolder(), kGoogleChromeBrowserCanary,
      user_data_importer::TYPE_CHROME);

  AddChromeToProfiles(
      profiles,
      GetChromeSourceProfiles(GetArcUserDataFolder().Append(
          base::FilePath::StringType(FILE_PATH_LITERAL("Local State")))),
      GetArcUserDataFolder(), kArcBrowser, user_data_importer::TYPE_ARC);
#endif
#if BUILDFLAG(IS_MAC)
  AddChromeToProfiles(
      profiles,
      GetChromeSourceProfiles(GetDiaUserDataFolder().Append(
          base::FilePath::StringType(FILE_PATH_LITERAL("Local State")))),
      GetDiaUserDataFolder(), kDiaBrowser, user_data_importer::TYPE_DIA);
#endif
#if BUILDFLAG(IS_WIN) || BUILDFLAG(IS_MAC)
  AddChromeToProfiles(
      profiles,
      GetChromeSourceProfiles(GetCometUserDataFolder().Append(
          base::FilePath::StringType(FILE_PATH_LITERAL("Local State")))),
      GetCometUserDataFolder(), kPerplexityCometBrowser, user_data_importer::TYPE_PERPLEXITY_COMET);
#endif
  AddChromeToProfiles(
      profiles,
      GetChromeSourceProfiles(GetChromiumUserDataFolder().Append(
          base::FilePath::StringType(FILE_PATH_LITERAL("Local State")))),
      GetChromiumUserDataFolder(), kChromiumBrowser,
      user_data_importer::TYPE_CHROME);

  AddChromeToProfiles(
      profiles,
      GetChromeSourceProfiles(GetEdgeUserDataFolder().Append(
          base::FilePath::StringType(FILE_PATH_LITERAL("Local State")))),
      GetEdgeUserDataFolder(), kMicrosoftEdgeBrowser,
      user_data_importer::TYPE_EDGE_CHROMIUM);

  AddChromeToProfiles(
      profiles,
      GetChromeSourceProfiles(GetVivaldiUserDataFolder().Append(
          base::FilePath::StringType(FILE_PATH_LITERAL("Local State")))),
      GetVivaldiUserDataFolder(), kVivaldiBrowser,
      user_data_importer::TYPE_VIVALDI);

  AddChromeToProfiles(
      profiles,
      GetChromeSourceProfiles(GetOperaUserDataFolder().Append(
          base::FilePath::StringType(FILE_PATH_LITERAL("Local State")))),
      GetOperaUserDataFolder(), kOperaBrowser, user_data_importer::TYPE_OPERA);

  AddChromeToProfiles(
      profiles,
      GetChromeSourceProfiles(GetYandexUserDataFolder().Append(
          base::FilePath::StringType(FILE_PATH_LITERAL("Local State")))),
      GetYandexUserDataFolder(), kYandexBrowser,
      user_data_importer::TYPE_YANDEX);

  AddChromeToProfiles(
      profiles,
      GetChromeSourceProfiles(GetWhaleUserDataFolder().Append(
          base::FilePath::StringType(FILE_PATH_LITERAL("Local State")))),
      GetWhaleUserDataFolder(), kWhaleBrowser, user_data_importer::TYPE_WHALE);

  struct BraveVariant {
    const char* directory;
    const char* name;
  };
  constexpr BraveVariant kBraveVariants[] = {
      {"Brave-Browser", kBraveBrowser},
      {"Brave-Browser-Beta", "Brave Beta"},
      {"Brave-Browser-Dev", "Brave Dev"},
      {"Brave-Browser-Nightly", "Brave Nightly"},
      {"Brave-Origin", "Brave Origin"},
      {"Brave-Origin-Beta", "Brave Origin Beta"},
      {"Brave-Origin-Dev", "Brave Origin Dev"},
      {"Brave-Origin-Nightly", "Brave Origin Nightly"},
  };
  // Origin and release channels keep independent user-data directories. On
  // Windows the profile root is an extra User Data level below each product.
  auto brave_parent = GetBraveUserDataFolder().DirName();
#if BUILDFLAG(IS_WIN)
  brave_parent = brave_parent.DirName();
#endif
  for (const auto& variant : kBraveVariants) {
    auto root = brave_parent.AppendASCII(variant.directory);
#if BUILDFLAG(IS_WIN)
    root = root.AppendASCII("User Data");
#endif
    add_location(root, variant.name, user_data_importer::TYPE_BRAVE);
  }

#if BUILDFLAG(IS_WIN)
  const auto local = GetChromiumUserDataFolder().DirName().DirName();
  add_location(local.AppendASCII("Helium").AppendASCII("User Data"), "Helium",
               user_data_importer::TYPE_CHROME);
  const auto edge_parent = GetEdgeUserDataFolder().DirName().DirName();
  add_location(edge_parent.AppendASCII("Edge Beta").AppendASCII("User Data"),
               "Microsoft Edge Beta", user_data_importer::TYPE_EDGE_CHROMIUM);
  add_location(edge_parent.AppendASCII("Edge Dev").AppendASCII("User Data"),
               "Microsoft Edge Dev", user_data_importer::TYPE_EDGE_CHROMIUM);
  add_location(edge_parent.AppendASCII("Edge SxS").AppendASCII("User Data"),
               "Microsoft Edge Canary", user_data_importer::TYPE_EDGE_CHROMIUM);
  add_location(
      GetOperaUserDataFolder().DirName().AppendASCII("Opera GX Stable"),
      "Opera GX", user_data_importer::TYPE_OPERA);
#elif BUILDFLAG(IS_MAC)
  const auto support = GetChromiumUserDataFolder().DirName();
  add_location(support.AppendASCII("net.imput.helium"), "Helium",
               user_data_importer::TYPE_CHROME);
  add_location(support.AppendASCII("Helium"), "Helium",
               user_data_importer::TYPE_CHROME);
  add_location(support.AppendASCII("Microsoft Edge Beta"),
               "Microsoft Edge Beta", user_data_importer::TYPE_EDGE_CHROMIUM);
  add_location(support.AppendASCII("Microsoft Edge Dev"), "Microsoft Edge Dev",
               user_data_importer::TYPE_EDGE_CHROMIUM);
  add_location(support.AppendASCII("Microsoft Edge Canary"),
               "Microsoft Edge Canary", user_data_importer::TYPE_EDGE_CHROMIUM);
  add_location(support.AppendASCII("com.operasoftware.OperaGX"), "Opera GX",
               user_data_importer::TYPE_OPERA);
#endif

#if BUILDFLAG(IS_LINUX)
  const auto config = GetChromiumUserDataFolder().DirName();
  add_location(config.AppendASCII("net.imput.helium"), "Helium",
               user_data_importer::TYPE_CHROME);
  add_location(config.AppendASCII("helium"), "Helium",
               user_data_importer::TYPE_CHROME);
  add_location(config.AppendASCII("microsoft-edge-beta"), "Microsoft Edge Beta",
               user_data_importer::TYPE_EDGE_CHROMIUM);
  add_location(config.AppendASCII("microsoft-edge-dev"), "Microsoft Edge Dev",
               user_data_importer::TYPE_EDGE_CHROMIUM);
  add_location(config.AppendASCII("vivaldi-snapshot"), "Vivaldi Snapshot",
               user_data_importer::TYPE_VIVALDI);
  base::FilePath home;
  if (base::PathService::Get(base::DIR_HOME, &home)) {
    add_location(home.AppendASCII(".var/app/net.imput.helium/config/net.imput.helium"),
                 "Helium", user_data_importer::TYPE_CHROME);
    add_location(home.AppendASCII(".var/app/org.chromium.Chromium/config/chromium"),
                 kChromiumBrowser, user_data_importer::TYPE_CHROME);
    add_location(home.AppendASCII(".var/app/com.google.Chrome/config/google-chrome"),
                 kGoogleChromeBrowser, user_data_importer::TYPE_CHROME);
    for (const char* location :
         {".var/app/com.brave.Browser/config/BraveSoftware",
          "snap/brave/current/.config/BraveSoftware",
          "snap/brave/common/.config/BraveSoftware"}) {
      for (const auto& variant : kBraveVariants) {
        add_location(home.AppendASCII(location).AppendASCII(variant.directory),
                     variant.name, user_data_importer::TYPE_BRAVE);
      }
    }
    add_location(
        home.AppendASCII(".var/app/com.vivaldi.Vivaldi/config/vivaldi"),
        kVivaldiBrowser, user_data_importer::TYPE_VIVALDI);
    add_location(home.AppendASCII(".var/app/com.opera.Opera/config/opera"),
                 kOperaBrowser, user_data_importer::TYPE_OPERA);
    add_location(home.AppendASCII(
                     ".var/app/io.github.ungoogled_software.ungoogled_chromium/"
                     "config/chromium"),
                 kChromiumBrowser, user_data_importer::TYPE_CHROME);
    add_location(home.AppendASCII(".var/app/com.microsoft.Edge/config/microsoft-edge"),
                 kMicrosoftEdgeBrowser, user_data_importer::TYPE_EDGE_CHROMIUM);
    add_location(home.AppendASCII("snap/chromium/common/chromium"),
                 kChromiumBrowser, user_data_importer::TYPE_CHROME);
    add_location(home.AppendASCII("snap/chromium/common/.config/chromium"),
                 kChromiumBrowser, user_data_importer::TYPE_CHROME);
    add_location(home.AppendASCII("snap/chromium/current/.config/chromium"),
                 kChromiumBrowser, user_data_importer::TYPE_CHROME);
    add_location(home.AppendASCII("snap/opera/common/.config/opera"),
                 kOperaBrowser, user_data_importer::TYPE_OPERA);
  }
  // Installed via snap Opera has different profile path.
  AddChromeToProfiles(
      profiles,
      GetChromeSourceProfiles(GetOperaSnapUserDataFolder().Append(
          base::FilePath::StringType(FILE_PATH_LITERAL("Local State")))),
      GetOperaSnapUserDataFolder(), kOperaBrowser,
      user_data_importer::TYPE_OPERA);
#endif
}

}  // namespace
