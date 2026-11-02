// Copyright 2012 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/common/importer/firefox_importer_utils.h"

#include <stddef.h>

#include <algorithm>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <utility>

#include "base/base_paths.h"
#include "base/environment.h"
#include "base/files/file_enumerator.h"
#include "base/files/file_util.h"
#include "base/logging.h"
#include "base/path_service.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_split.h"
#include "base/strings/string_util.h"
#include "base/strings/stringprintf.h"
#include "base/strings/utf_string_conversions.h"
#include "base/values.h"
#include "build/build_config.h"
#include "chrome/common/ini_parser.h"
#include "chrome/grit/generated_resources.h"
#include "ui/base/l10n/l10n_util.h"
#include "url/gurl.h"

#if BUILDFLAG(IS_WIN)
#include "base/base_paths_win.h"
#endif

namespace {

// Retrieves the file system path of the profile name.
base::FilePath GetProfilePath(const base::DictValue& root,
                              const std::string& profile_name,
                              const base::FilePath& base_dir) {
  std::string path_str;
  const std::string* is_relative =
      root.FindStringByDottedPath(profile_name + ".IsRelative");
  if (!is_relative)
    return base::FilePath();
  if (const std::string* ptr =
          root.FindStringByDottedPath(profile_name + ".Path"))
    path_str = *ptr;
  else
    return base::FilePath();

#if BUILDFLAG(IS_WIN)
  base::ReplaceSubstringsAfterOffset(&path_str, 0, "/", "\\");
#endif
  base::FilePath path = base::FilePath::FromUTF8Unsafe(path_str);

  // IsRelative=1 means the folder path would be relative to the
  // path of profiles.ini. IsRelative=0 refers to a custom profile
  // location.
  if (*is_relative == "1")
    path = base_dir.Append(path);

  return path;
}

} // namespace

std::vector<FirefoxDetail> GetFirefoxDetailsImpl(
    const std::string& firefox_install_id,
    base::FilePath ini_file) {
  std::string content;
  base::ReadFileToString(ini_file, &content);
  DictionaryValueINIParser ini_parser;
  ini_parser.Parse(content);
  return GetFirefoxDetailsFromDictionary(ini_parser.root(), firefox_install_id,
                                         ini_file.DirName());
}

namespace {

std::vector<base::FilePath> MozillaProfileINIs(bool zen) {
  std::vector<base::FilePath> paths;
  paths.push_back(zen ? GetZenProfilesINI() : GetProfilesINI());
  base::FilePath home;
  base::PathService::Get(base::DIR_HOME, &home);
  auto add = [&](const base::FilePath& root, const char* subpath) {
    if (root.IsAbsolute()) {
      paths.push_back(root.Append(base::FilePath::FromUTF8Unsafe(subpath)));
    }
  };
#if BUILDFLAG(IS_LINUX)
  base::FilePath config;
  auto env = base::Environment::Create();
  if (const auto value = env->GetVar("XDG_CONFIG_HOME"); value) {
    config = base::FilePath(*value);
  }
  if (!config.IsAbsolute() && !home.empty()) {
    config = home.AppendASCII(".config");
  }
  if (zen) {
    add(config, "zen/profiles.ini");
    add(home, ".zen/profiles.ini");
    add(home, ".var/app/app.zen_browser.zen/.zen/profiles.ini");
    add(home, ".var/app/app.zen_browser.zen/config/zen/profiles.ini");
  } else {
    add(config, "mozilla/firefox/profiles.ini");
    add(home, ".mozilla/firefox/profiles.ini");
    add(home, "snap/firefox/common/.mozilla/firefox/profiles.ini");
    add(home, ".var/app/org.mozilla.firefox/.mozilla/firefox/profiles.ini");
    add(home,
        ".var/app/org.mozilla.firefox/config/mozilla/firefox/profiles.ini");
    add(config, "librewolf/librewolf/profiles.ini");
    add(home, ".librewolf/profiles.ini");
    add(home, ".var/app/io.gitlab.librewolf-community/.librewolf/profiles.ini");
    add(home,
        ".var/app/io.gitlab.librewolf-community/config/librewolf/librewolf/"
        "profiles.ini");
    add(config, "waterfox/waterfox/profiles.ini");
    add(config, "waterfox/profiles.ini");
    add(home, ".waterfox/profiles.ini");
    add(config, "floorp/floorp/profiles.ini");
    add(config, "floorp/profiles.ini");
    add(home, ".floorp/profiles.ini");
    add(home, ".var/app/one.ablaze.floorp/.floorp/profiles.ini");
  }
#elif BUILDFLAG(IS_MAC)
  base::FilePath app_data;
  if (base::PathService::Get(base::DIR_APP_DATA, &app_data)) {
    add(app_data, zen ? "zen/profiles.ini" : "Firefox/profiles.ini");
    if (!zen) {
      add(app_data, "LibreWolf/profiles.ini");
      add(app_data, "librewolf/profiles.ini");
      add(app_data, "Waterfox/profiles.ini");
      add(app_data, "Floorp/profiles.ini");
      add(app_data, "floorp/profiles.ini");
    }
  }
  if (!zen) {
    add(home, ".librewolf/profiles.ini");
  }
#elif BUILDFLAG(IS_WIN)
  base::FilePath roaming;
  if (base::PathService::Get(base::DIR_ROAMING_APP_DATA, &roaming)) {
    add(roaming, zen ? "zen/profiles.ini" : "Mozilla/Firefox/profiles.ini");
    if (!zen) {
      add(roaming, "LibreWolf/profiles.ini");
      add(roaming, "librewolf/profiles.ini");
      add(roaming, "Waterfox/profiles.ini");
      add(roaming, "Floorp/profiles.ini");
    }
  }
  if (!zen) {
    add(home, ".librewolf/profiles.ini");
    base::FilePath local;
    if (base::PathService::Get(base::DIR_LOCAL_APP_DATA, &local)) {
      // Microsoft Store Firefox keeps a separate current-user profile root.
      base::FileEnumerator packages(local.AppendASCII("Packages"), false,
                                    base::FileEnumerator::DIRECTORIES,
                                    FILE_PATH_LITERAL("Mozilla.Firefox*"));
      for (base::FilePath package = packages.Next(); !package.empty();
           package = packages.Next()) {
        add(package, "LocalCache/Roaming/Mozilla/Firefox/profiles.ini");
      }
    }
  }
#endif
  return paths;
}

std::vector<FirefoxDetail> AllMozillaDetails(const std::string& install_id,
                                             bool zen) {
  std::vector<FirefoxDetail> profiles;
  std::set<base::FilePath> seen;
  for (const auto& ini : MozillaProfileINIs(zen)) {
    if (!ini.IsAbsolute() || !base::PathExists(ini)) {
      continue;
    }
    for (auto& profile : GetFirefoxDetailsImpl(install_id, ini)) {
      if (!base::DirectoryExists(profile.path)) {
        continue;
      }
      const auto canonical = base::MakeAbsoluteFilePath(profile.path);
      if (!canonical.empty() && seen.insert(canonical).second) {
        profile.path = canonical;
        profiles.push_back(std::move(profile));
      }
    }
  }
  if (profiles.size() > 1) {
    for (auto& profile : profiles) {
      if (profile.name.empty()) {
        profile.name = profile.path.BaseName().LossyDisplayName();
      }
    }
  }
  return profiles;
}

}  // namespace

std::vector<FirefoxDetail> GetFirefoxDetails(
    const std::string& firefox_install_id) {
  return AllMozillaDetails(firefox_install_id, false);
}

std::vector<FirefoxDetail> GetZenDetails(const std::string& zen_install_id) {
  return AllMozillaDetails(zen_install_id, true);
}

std::vector<FirefoxDetail> GetFirefoxDetailsFromDictionary(
    const base::DictValue& root,
    const std::string& firefox_install_id,
    const base::FilePath& base_dir) {
  std::vector<FirefoxDetail> profile_details;

  std::vector<std::pair<int, std::string>> sections;
  for (const auto [key, value] : root) {
    if (!value.is_dict() || !base::StartsWith(key, "Profile")) {
      continue;
    }
    const auto suffix = std::string_view(key).substr(7);
    int index;
    if (!suffix.empty() &&
        std::ranges::all_of(suffix,
                            [](char c) { return c >= '0' && c <= '9'; }) &&
        base::StringToInt(suffix, &index)) {
      sections.emplace_back(index, key);
    }
  }
  std::ranges::sort(sections);
  for (const auto& section : sections) {
    const std::string& current_profile = section.second;

    if (!root.FindByDottedPath(current_profile + ".Path")) {
      continue;
    }

    FirefoxDetail details;
    details.path = GetProfilePath(root, current_profile, base_dir);
    std::u16string name;
    if (const std::string* name_utf8 =
            root.FindStringByDottedPath(current_profile + ".Name")) {
      name = base::UTF8ToUTF16(*name_utf8);
    }
    // Make the profile name more presentable by replacing dashes with spaces.
    base::ReplaceChars(name, u"-", u" ", &name);
    details.name = name;
    profile_details.push_back(details);
  }

  // If there is only one profile, set the name as a blank string.
  // The name is only used to disambiguate profiles in the profile selection UI,
  // which is only useful when there are multiple profiles.
  if (profile_details.size() == 1) {
    profile_details[0].name = std::u16string();
  }

  return profile_details;
}

#if BUILDFLAG(IS_MAC)
// Find the "*.app" component of the path and build up from there.
// The resulting path will be .../Firefox.app/Contents/MacOS.
// We do this because we don't trust LastAppDir to always be
// this particular path, without any subdirs, and we want to make
// our assumption about Firefox's root being in that path explicit.
bool ComposeMacAppPath(const std::string& path_from_file,
                       base::FilePath* output) {
  std::vector<base::FilePath::StringType> path_components =
      base::FilePath(path_from_file).GetComponents();
  if (path_components.empty())
    return false;
  // The first path component is special because it may be absolute. Calling
  // Append with an absolute path component will trigger an assert, so we
  // must handle it differently and initialize output with it.
  *output = base::FilePath(path_components[0]);
  // Append next path components until we find the *.app component. When we do,
  // append Contents/MacOS.
  for (size_t i = 1; i < path_components.size(); ++i) {
    *output = output->Append(path_components[i]);
    if (base::EndsWith(path_components[i], ".app",
                       base::CompareCase::SENSITIVE)) {
      *output = output->Append("Contents");
      *output = output->Append("MacOS");
      return true;
    }
  }
  LOG(ERROR) << path_from_file << " doesn't look like a valid Firefox "
             << "installation path: missing /*.app/ directory.";
  return false;
}
#endif  // BUILDFLAG(IS_MAC)

bool GetFirefoxVersionAndPathFromProfile(const base::FilePath& profile_path,
                                         int* version,
                                         base::FilePath* app_path) {
  bool ret = false;
  base::FilePath compatibility_file =
      profile_path.AppendASCII("compatibility.ini");
  std::string content;
  base::ReadFileToString(compatibility_file, &content);
  base::ReplaceSubstringsAfterOffset(&content, 0, "\r\n", "\n");

  for (const std::string& line : base::SplitString(
           content, "\n", base::TRIM_WHITESPACE, base::SPLIT_WANT_ALL)) {
    if (line.empty() || line[0] == '#' || line[0] == ';')
      continue;
    size_t equal = line.find('=');
    if (equal != std::string::npos) {
      std::string key = line.substr(0, equal);
      if (key == "LastVersion") {
        base::StringToInt(line.substr(equal + 1), version);
        ret = true;
      } else if (key == "LastAppDir") {
        // TODO(evanm): If the path in question isn't convertible to
        // UTF-8, what does Firefox do?  If it puts raw bytes in the
        // file, we could go straight from bytes -> filepath;
        // otherwise, we're out of luck here.
#if BUILDFLAG(IS_MAC)
        // Extract path from "LastAppDir=/actual/path"
        size_t separator_pos = line.find_first_of('=');
        const std::string& path_from_ini = line.substr(separator_pos + 1);
        if (!ComposeMacAppPath(path_from_ini, app_path))
          return false;
#else   // BUILDFLAG(IS_MAC)
        *app_path = base::FilePath::FromUTF8Unsafe(line.substr(equal + 1));
#endif  // BUILDFLAG(IS_MAC)
      }
    }
  }
  return ret;
}

bool ReadPrefFile(const base::FilePath& path, std::string* content) {
  if (content == nullptr)
    return false;

  base::ReadFileToString(path, content);

  if (content->empty()) {
    LOG(WARNING) << "Firefox preference file " << path.value() << " is empty.";
    return false;
  }

  return true;
}

std::string ReadBrowserConfigProp(const base::FilePath& app_path,
                                  const std::string& pref_key) {
  std::string content;
  if (!ReadPrefFile(app_path.AppendASCII("browserconfig.properties"), &content))
    return std::string();

  // This file has the syntax: key=value.
  size_t prop_index = content.find(pref_key + "=");
  if (prop_index == std::string::npos)
    return std::string();

  size_t start = prop_index + pref_key.length();
  size_t stop = std::string::npos;
  if (start != std::string::npos)
    stop = content.find("\n", start + 1);

  if (start == std::string::npos ||
      stop == std::string::npos || (start == stop)) {
    LOG(WARNING) << "Firefox property " << pref_key << " could not be parsed.";
    return std::string();
  }

  return content.substr(start + 1, stop - start - 1);
}

std::string ReadPrefsJsValue(const base::FilePath& profile_path,
                             const std::string& pref_key) {
  std::string content;
  if (!ReadPrefFile(profile_path.AppendASCII("prefs.js"), &content))
    return std::string();

  return GetPrefsJsValue(content, pref_key);
}

GURL GetHomepage(const base::FilePath& profile_path) {
  std::string home_page_list =
      ReadPrefsJsValue(profile_path, "browser.startup.homepage");

  size_t seperator = home_page_list.find_first_of('|');
  if (seperator == std::string::npos)
    return GURL(home_page_list);

  return GURL(home_page_list.substr(0, seperator));
}

bool IsDefaultHomepage(const GURL& homepage, const base::FilePath& app_path) {
  if (!homepage.is_valid())
    return false;

  std::string default_homepages =
      ReadBrowserConfigProp(app_path, "browser.startup.homepage");

  size_t seperator = default_homepages.find_first_of('|');
  if (seperator == std::string::npos)
    return homepage.spec() == GURL(default_homepages).spec();

  // Crack the string into separate homepage urls.
  for (const std::string& url : base::SplitString(
           default_homepages, "|",
           base::TRIM_WHITESPACE, base::SPLIT_WANT_ALL)) {
    if (homepage.spec() == GURL(url).spec())
      return true;
  }

  return false;
}

std::string GetPrefsJsValue(const std::string& content,
                            const std::string& pref_key) {
  // This file has the syntax: user_pref("key", value);
  std::string search_for = std::string("user_pref(\"") + pref_key +
                           std::string("\", ");
  size_t prop_index = content.find(search_for);
  if (prop_index == std::string::npos)
    return std::string();

  size_t start = prop_index + search_for.length();
  size_t stop = std::string::npos;
  if (start != std::string::npos) {
    // Stop at the last ')' on this line.
    stop = content.find("\n", start + 1);
    stop = content.rfind(")", stop);
  }

  if (start == std::string::npos || stop == std::string::npos ||
      stop < start) {
    LOG(WARNING) << "Firefox property " << pref_key << " could not be parsed.";
    return std::string();
  }

  // String values have double quotes we don't need to return to the caller.
  if (content[start] == '\"' && content[stop - 1] == '\"') {
    ++start;
    --stop;
  }

  return content.substr(start, stop - start);
}

// The branding name is obtained from the application.ini file from the Firefox
// application directory. A sample application.ini file is the following:
//   [App]
//   Vendor=Mozilla
//   Name=Iceweasel
//   Profile=mozilla/firefox
//   Version=3.5.16
//   BuildID=20120421070307
//   Copyright=Copyright (c) 1998 - 2010 m0z111a.qjz9zk
//   ID={ec8030f7-c20a-464f-9b0e-13a3a9e97384}
//   .........................................
// In this example the function returns "Iceweasel" (or a localized equivalent).
std::u16string GetFirefoxImporterName(const base::FilePath& app_path) {
  const base::FilePath app_ini_file = app_path.AppendASCII("application.ini");
  std::string branding_name;
  if (base::PathExists(app_ini_file)) {
    std::string content;
    base::ReadFileToString(app_ini_file, &content);

    const std::string name_attr("Name=");
    bool in_app_section = false;
    for (std::string_view line : base::SplitStringPiece(
             content, "\n", base::TRIM_WHITESPACE, base::SPLIT_WANT_ALL)) {
      if (line == "[App]") {
        in_app_section = true;
      } else if (in_app_section) {
        if (base::StartsWith(line, name_attr, base::CompareCase::SENSITIVE)) {
          branding_name = std::string(line.substr(name_attr.size()));
          break;
        }
        if (line.length() > 0 && line[0] == '[') {
          // No longer in the [App] section.
          break;
        }
      }
    }
  }

  branding_name = base::ToLowerASCII(branding_name);
  // Firefox-compatible forks retain their own name in the source picker.
  if (branding_name == "librewolf")
    return u"LibreWolf";
  if (branding_name == "waterfox" || branding_name == "waterfox classic")
    return u"Waterfox";
  if (branding_name == "floorp")
    return u"Floorp";
  if (branding_name.find("iceweasel") != std::string::npos)
    return l10n_util::GetStringUTF16(IDS_IMPORT_FROM_ICEWEASEL);
  return l10n_util::GetStringUTF16(IDS_IMPORT_FROM_FIREFOX);
}
