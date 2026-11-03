/* Copyright (c) 2019 The Brave Authors, 2025 imput. All rights reserved.
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at https://mozilla.org/MPL/2.0/. */

#include "chrome/common/importer/chrome_importer_utils.h"

#include <algorithm>
#include <memory>
#include <optional>
#include <utility>

#include "base/check.h"
#include "base/files/file_enumerator.h"
#include "base/files/file_path.h"
#include "base/files/file_util.h"
#include "base/json/json_reader.h"
#include "base/values.h"
#include "build/config/linux/dbus/buildflags.h"
#include "chrome/common/importer/importer_constants.h"
#include "chrome/common/importer/scoped_copy_file.h"
#include "components/user_data_importer/common/importer_data_types.h"
#include "sql/database.h"
#include "sql/statement.h"

#if BUILDFLAG(ENABLE_EXTENSIONS)
#include "extensions/common/extension.h"
#include "extensions/common/manifest.h"
#endif

#if BUILDFLAG(ENABLE_EXTENSIONS)
using extensions::Extension;
using extensions::Manifest;
#endif

namespace {
#if BUILDFLAG(ENABLE_EXTENSIONS)

std::optional<base::DictValue> GetChromeExtensionsListFromFile(
    const base::FilePath& preference_path) {
  if (!base::PathExists(preference_path)) {
    return std::nullopt;
  }

  std::string preference_content;
  base::ReadFileToString(preference_path, &preference_content);

  std::optional<base::DictValue> preference = base::JSONReader::ReadDict(
      preference_content, base::JSON_PARSE_CHROMIUM_EXTENSIONS);
  if (!preference) {
    return std::nullopt;
  }
  if (auto* extensions =
          preference->FindDictByDottedPath(kChromeExtensionsListPath)) {
    return std::move(*extensions);
  }
  return std::nullopt;
}

bool HasImportableExtensions(const base::FilePath& profile_path) {
  return GetImportableChromeExtensionsList(profile_path).has_value();
}

std::vector<std::string> GetImportableListFromChromeExtensionsList(
    const base::DictValue& extensions_list) {
  std::vector<std::string> extensions;
  for (const auto [key, value] : extensions_list) {
    if (!value.is_dict()) {
      continue;
    }
    const base::DictValue& dict = value.GetDict();
    // Only import if type is extension, it's came from webstore and it's not
    // installed by default.
    if (dict.FindBool("was_installed_by_default").value_or(true)) {
      continue;
    }

    const auto state = dict.FindInt("state");
    if (state.has_value()) {
      // If `state` exists, probably it is an old browser version.
      if (state == 0) {
        // explicit `"state": 0` means disabled state
        continue;
      }
    } else if (const auto* disable_reasons = dict.FindList("disable_reasons");
               disable_reasons && !disable_reasons->empty()) {
      // For new browsers an extension is enabled if there is no disable reason.
      continue;
    }

    if (!dict.FindBool("from_webstore").value_or(false)) {
      continue;
    }

    if (auto* manifest_dict = dict.FindDict("manifest")) {
      if (Manifest::GetTypeFromManifestValue(*manifest_dict) ==
          Manifest::Type::kExtension) {
        extensions.push_back(key);
      }
    }
  }

  return extensions;
}

std::optional<base::DictValue> GetChromeExtensionsList(
    const base::FilePath& profile_path) {
  auto list_from_secure_preference = GetChromeExtensionsListFromFile(
      profile_path.AppendASCII(kChromeSecurePreferencesFile));

  auto list_from_preferences = GetChromeExtensionsListFromFile(
      profile_path.AppendASCII(kChromePreferencesFile));
  if (!list_from_secure_preference.has_value()) {
    return list_from_preferences;
  }

  if (list_from_secure_preference.has_value() &&
      list_from_preferences.has_value()) {
    list_from_secure_preference->Merge(
        std::move(list_from_preferences.value()));
    return list_from_secure_preference;
  }

  return list_from_secure_preference;
}
#endif

bool IsLastActiveProfile(const std::string& profile,
                         const base::ListValue& last_active_profiles) {
  for (const auto& it : last_active_profiles) {
    if (it.GetString() == profile) {
      return true;
    }
  }
  return false;
}

}  // namespace

base::ListValue GetChromeSourceProfiles(
    const base::FilePath& local_state_path) {
  base::ListValue profiles;
  const base::FilePath root = local_state_path.DirName();
  if (!base::DirectoryExists(root)) {
    return profiles;
  }
  if (base::PathExists(local_state_path)) {
    std::string local_state_content;
    base::ReadFileToString(local_state_path, &local_state_content);
    std::optional<base::DictValue> local_state_dict =
        base::JSONReader::ReadDict(local_state_content,
                                   base::JSON_PARSE_CHROMIUM_EXTENSIONS);
    const auto* profile_dict =
        local_state_dict ? local_state_dict->FindDict("profile") : nullptr;
    if (profile_dict) {
      const auto* last_active_profiles =
          profile_dict->FindList("last_active_profiles");

      const auto* info_cache = profile_dict->FindDict("info_cache");
      if (info_cache) {
        for (const auto value : *info_cache) {
          const base::FilePath id = base::FilePath::FromUTF8Unsafe(value.first);
          if (id.empty() || id.IsAbsolute() || id.ReferencesParent() ||
              id != id.BaseName()) {
            continue;
          }
          const auto* profile = value.second.GetIfDict();
          if (!profile) {
            continue;
          }

          auto* name = profile->FindString("name");
          if (!name) {
            continue;
          }
          base::DictValue entry;
          entry.Set("id", value.first);
          entry.Set("name", *name);
          if (last_active_profiles) {
            entry.Set("last_active",
                      IsLastActiveProfile(value.first, *last_active_profiles));
          }

          auto* avatar_icon = profile->FindString("avatar_icon");
          if (avatar_icon) {
            entry.Set("avatar_icon", *avatar_icon);
          }
          auto active_time = profile->FindDouble("active_time");
          if (active_time) {
            entry.Set("active_time", *active_time);
          }
          profiles.Append(std::move(entry));
        }
      }
    }
  }
  {
    // Some browsers omit info_cache, or do not finish writing Local State
    // until they exit. Merge real directories even when a cache is stale or
    // incomplete, retaining cached display names and one entry per ID.
    auto has_data = [](const base::FilePath& path) {
      return base::PathExists(path.AppendASCII("Preferences")) ||
             base::PathExists(path.AppendASCII("Bookmarks")) ||
             base::PathExists(path.AppendASCII("History")) ||
             base::PathExists(path.AppendASCII("Login Data")) ||
             base::PathExists(path.AppendASCII("Login Data For Account"));
    };
    auto already_listed = [&](const std::string& id) {
      return std::ranges::any_of(profiles, [&](const auto& value) {
        const auto* dict = value.GetIfDict();
        const auto* existing = dict ? dict->FindString("id") : nullptr;
        return existing && *existing == id;
      });
    };
    if (has_data(root) && !already_listed("")) {
      base::DictValue entry;
      entry.Set("id", "");
      entry.Set("name", "Default");
      profiles.Append(std::move(entry));
    }
    base::FileEnumerator children(root, false,
                                  base::FileEnumerator::DIRECTORIES);
    for (auto path = children.Next(); !path.empty(); path = children.Next()) {
      const std::string id = path.BaseName().AsUTF8Unsafe();
      if ((id != "Default" && !id.starts_with("Profile ")) || !has_data(path) ||
          already_listed(id)) {
        continue;
      }
      base::DictValue entry;
      entry.Set("id", id);
      entry.Set("name", id);
      profiles.Append(std::move(entry));
    }
  }
  return profiles;
}

bool ChromeImporterCanImport(const base::FilePath& profile,
                             user_data_importer::ImporterType type,
                             uint16_t* services_supported) {
  DCHECK(services_supported);
  *services_supported = user_data_importer::NONE;

  base::FilePath bookmarks = profile.Append(
      base::FilePath::StringType(FILE_PATH_LITERAL("Bookmarks")));
  if (base::PathExists(bookmarks)) {
    *services_supported |= user_data_importer::FAVORITES;
  }

  // Arc stores sidebar items (pinned tabs, top apps, folders) in
  // StorableSidebar.json at the Arc root, two levels above the profile dir.
  if (type == user_data_importer::TYPE_ARC) {
    base::FilePath sidebar = profile.DirName().DirName().Append(
        base::FilePath::StringType(FILE_PATH_LITERAL("StorableSidebar.json")));
    if (base::PathExists(sidebar)) {
      *services_supported |= user_data_importer::FAVORITES;
    }
  }

  base::FilePath history =
      profile.Append(base::FilePath::StringType(FILE_PATH_LITERAL("History")));
  if (base::PathExists(history)) {
    *services_supported |= user_data_importer::HISTORY;
  }

#if BUILDFLAG(IS_LINUX) && BUILDFLAG(USE_DBUS)
  if ((type == user_data_importer::TYPE_CHROME ||
       type == user_data_importer::TYPE_BRAVE) &&
      (base::PathExists(
           profile.AppendASCII("Network").AppendASCII("Cookies")) ||
       base::PathExists(profile.AppendASCII("Cookies")))) {
    *services_supported |= user_data_importer::COOKIES;
  }
  // The native reader supports these Chrome/Chromium and Brave keys.
  // Other platforms and brands retain their existing import capabilities.
  if ((type == user_data_importer::TYPE_CHROME ||
       type == user_data_importer::TYPE_BRAVE) &&
      (base::PathExists(profile.AppendASCII("Login Data")) ||
       base::PathExists(profile.AppendASCII("Login Data For Account")))) {
    *services_supported |= user_data_importer::PASSWORDS;
  }
#endif

#if BUILDFLAG(ENABLE_EXTENSIONS)
  if (HasImportableExtensions(profile)) {
    *services_supported |= user_data_importer::EXTENSIONS;
  }
#endif

  return *services_supported != user_data_importer::NONE;
}

#if BUILDFLAG(ENABLE_EXTENSIONS)
std::optional<std::vector<std::string>> GetImportableChromeExtensionsList(
    const base::FilePath& profile_path) {
  if (auto extensions = GetChromeExtensionsList(profile_path)) {
    return GetImportableListFromChromeExtensionsList(extensions.value());
  }
  return std::nullopt;
}
#endif
