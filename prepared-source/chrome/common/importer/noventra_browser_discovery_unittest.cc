// Copyright 2026 Noventra contributors
// SPDX-License-Identifier: GPL-3.0-only

#include <memory>
#include <set>
#include <string>

#include "base/base_paths.h"
#include "base/files/file_util.h"
#include "base/files/scoped_temp_dir.h"
#include "base/scoped_environment_variable_override.h"
#include "base/strings/utf_string_conversions.h"
#include "base/test/scoped_path_override.h"
#include "build/build_config.h"
#include "chrome/common/importer/chrome_importer_utils.h"
#include "chrome/common/importer/firefox_importer_utils.h"
#include "testing/gtest/include/gtest/gtest.h"

#if BUILDFLAG(IS_LINUX)
namespace {
class NoventraBrowserDiscoveryTest : public testing::Test {
 protected:
  void SetUp() override {
    ASSERT_TRUE(home_.CreateUniqueTempDir());
    home_override_ = std::make_unique<base::ScopedPathOverride>(
        base::DIR_HOME, home_.GetPath());
    config_override_ =
        std::make_unique<base::ScopedEnvironmentVariableOverride>(
            "XDG_CONFIG_HOME", home_.GetPath().AppendASCII("config").value());
  }

  base::FilePath Seed(const char* ini_subpath, const char* profile_name) {
    auto ini = home_.GetPath().AppendASCII(ini_subpath);
    auto profile = ini.DirName().AppendASCII(profile_name);
    EXPECT_TRUE(base::CreateDirectory(profile));
    EXPECT_TRUE(base::WriteFile(
        ini,
        std::string("[Profile0]\nName=Owned profile\nIsRelative=1\nPath=") +
            profile_name + "\n"));
    return profile;
  }

  std::set<base::FilePath> Paths(const std::vector<FirefoxDetail>& profiles) {
    std::set<base::FilePath> paths;
    for (const auto& profile : profiles) {
      paths.insert(profile.path);
    }
    return paths;
  }

  base::ScopedTempDir home_;
  std::unique_ptr<base::ScopedPathOverride> home_override_;
  std::unique_ptr<base::ScopedEnvironmentVariableOverride> config_override_;
};

TEST_F(NoventraBrowserDiscoveryTest, AllFirefoxInstallationsRemainVisible) {
  std::set<base::FilePath> wanted;
  wanted.insert(Seed("config/mozilla/firefox/profiles.ini", "xdg"));
  wanted.insert(Seed(".mozilla/firefox/profiles.ini", "native"));
  wanted.insert(
      Seed("snap/firefox/common/.mozilla/firefox/profiles.ini", "snap"));
  wanted.insert(Seed(
      ".var/app/org.mozilla.firefox/.mozilla/firefox/profiles.ini", "flatpak"));
  EXPECT_EQ(Paths(GetFirefoxDetails("")), wanted);
}

TEST_F(NoventraBrowserDiscoveryTest, AllZenInstallationsRemainVisible) {
  std::set<base::FilePath> wanted;
  wanted.insert(Seed("config/zen/profiles.ini", "xdg"));
  wanted.insert(Seed(".zen/profiles.ini", "native"));
  wanted.insert(
      Seed(".var/app/app.zen_browser.zen/.zen/profiles.ini", "flatpak"));
  EXPECT_EQ(Paths(GetZenDetails("")), wanted);
}

TEST_F(NoventraBrowserDiscoveryTest, StalePrimaryDoesNotHideOtherInstall) {
  auto stale = Seed("config/mozilla/firefox/profiles.ini", "removed");
  ASSERT_TRUE(base::DeletePathRecursively(stale));
  auto available = Seed(".mozilla/firefox/profiles.ini", "available");
  EXPECT_EQ(Paths(GetFirefoxDetails("")),
            std::set<base::FilePath>({available}));
}

TEST_F(NoventraBrowserDiscoveryTest, DuplicateProfileRootsAreListedOnce) {
  auto profile = Seed("config/mozilla/firefox/profiles.ini", "owned");
  auto alias = home_.GetPath().AppendASCII(".mozilla/firefox/profiles.ini");
  ASSERT_TRUE(base::CreateDirectory(alias.DirName()));
  ASSERT_TRUE(base::WriteFile(
      alias, "[Profile0]\nName=Same profile\nIsRelative=0\nPath=" +
                 profile.value() + "\n"));
  EXPECT_EQ(GetFirefoxDetails("").size(), 1u);
}

TEST_F(NoventraBrowserDiscoveryTest,
       RemovedProfileNumberDoesNotHideLaterProfile) {
  auto first = Seed("config/mozilla/firefox/profiles.ini", "first");
  auto later = first.DirName().AppendASCII("later");
  ASSERT_TRUE(base::CreateDirectory(later));
  ASSERT_TRUE(
      base::AppendToFile(first.DirName().AppendASCII("profiles.ini"),
                         "[Profile2]\nName=Later\nIsRelative=1\nPath=later\n"));
  EXPECT_EQ(Paths(GetFirefoxDetails("")),
            std::set<base::FilePath>({first, later}));
}

TEST_F(NoventraBrowserDiscoveryTest, RelativeXdgFallsBackToHomeConfig) {
  base::ScopedEnvironmentVariableOverride relative("XDG_CONFIG_HOME",
                                                   "relative");
  auto profile = Seed(".config/mozilla/firefox/profiles.ini", "fallback");
  EXPECT_EQ(Paths(GetFirefoxDetails("")), std::set<base::FilePath>({profile}));
}

TEST_F(NoventraBrowserDiscoveryTest,
       FlatBrowserDataIsNotLostWithoutLocalState) {
  auto root = home_.GetPath().AppendASCII("opera-profile");
  ASSERT_TRUE(base::CreateDirectory(root));
  ASSERT_TRUE(base::WriteFile(root.AppendASCII("Bookmarks"), "{}"));
  auto profiles = GetChromeSourceProfiles(root.AppendASCII("Local State"));
  ASSERT_EQ(profiles.size(), 1u);
  EXPECT_EQ(*profiles[0].GetDict().FindString("id"), "");
}

TEST_F(NoventraBrowserDiscoveryTest, KnownFirefoxForksKeepTheirOwnNames) {
  for (const auto* name : {"LibreWolf", "Waterfox", "Floorp"}) {
    auto app = home_.GetPath().AppendASCII(name);
    ASSERT_TRUE(base::CreateDirectory(app));
    ASSERT_TRUE(base::WriteFile(app.AppendASCII("application.ini"),
                                std::string("[App]\nName=") + name + "\n"));
    EXPECT_EQ(GetFirefoxImporterName(app), base::UTF8ToUTF16(name));
  }
}
}  // namespace
#endif
