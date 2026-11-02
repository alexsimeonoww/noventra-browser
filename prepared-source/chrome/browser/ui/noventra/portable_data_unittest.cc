// Copyright 2026 The Noventra Authors. All rights reserved.
#include "chrome/browser/ui/noventra/portable_data.h"

#include <string>
#include <string_view>
#include <utility>

#include "testing/gtest/include/gtest/gtest.h"

namespace noventra {
namespace {

base::DictValue Snapshot() {
  base::DictValue categories;
  for (const char* name :
       {"bookmarks", "spaces", "tabs", "settings", "extensions", "passwords"}) {
    categories.Set(name, std::string_view(name) == "bookmarks");
  }
  base::ListValue bookmarks;
  bookmarks.Append(base::DictValue()
                       .Set("title", "An owned bookmark")
                       .Set("url", "https://portable.invalid/bookmark"));
  base::ListValue spaces;
  spaces.Append(base::DictValue()
                    .Set("id", "")
                    .Set("name", "Personal")
                    .Set("archived", false)
                    .Set("color", "default")
                    .Set("icon", "default"));
  return base::DictValue()
      .Set("format", "noventra-portable-data")
      .Set("version", 1)
      .Set("transfer_id", "8f25b234-85c7-4712-b8cd-70f0bedc5c21")
      .Set("created_at", "2026-10-30T00:00:00.000Z")
      .Set("categories", std::move(categories))
      .Set("bookmarks", std::move(bookmarks))
      .Set("spaces", std::move(spaces))
      .Set("routes", base::ListValue())
      .Set("layout", base::DictValue())
      .Set("preferences", base::DictValue())
      .Set("tabs", base::ListValue())
      .Set("extensions", base::ListValue())
      .Set("excluded", base::DictValue()
                           .Set("bookmarks", 0)
                           .Set("tabs", 0)
                           .Set("extensions", 0));
}

base::DictValue Tab(std::string url = "https://portable.invalid/tab") {
  return base::DictValue()
      .Set("space_id", "")
      .Set("url", std::move(url))
      .Set("pinned", false)
      .Set("name", "")
      .Set("home_url", "");
}

TEST(PortableDataTest, ReviewContainsCountsWithoutPayloads) {
  auto snapshot = Snapshot();
  snapshot.FindDict("categories")->Set("extensions", true);
  snapshot.FindList("extensions")
      ->Append(base::DictValue()
                   .Set("id", "abcdefghijklmnopabcdefghijklmnop")
                   .Set("name", "Owned extension"));
  auto result = ValidatePortableData(snapshot);
  ASSERT_TRUE(result.has_value()) << result.error();
  EXPECT_EQ(result->FindInt("bookmarks"), 1);
  EXPECT_EQ(result->FindInt("bookmark_folders"), 0);
  EXPECT_EQ(result->FindInt("spaces"), 0);
  EXPECT_EQ(result->FindInt("extensions"), 1);
  EXPECT_FALSE(result->FindBool("password_present").value());
  EXPECT_FALSE(result->contains("preferences"));
  EXPECT_FALSE(result->contains("passwords_csv"));
  EXPECT_TRUE(result->FindList("extension_items"));
  EXPECT_TRUE(result->Find("bookmarks")->is_int());
}

TEST(PortableDataTest, RejectsForeignOrSecretSnapshotFields) {
  for (const char* field : {"cookies", "local_state", "login_data", "history",
                            "extension_data", "recovery_key"}) {
    auto snapshot = Snapshot();
    snapshot.Set(field, "not portable");
    EXPECT_FALSE(ValidatePortableData(snapshot).has_value()) << field;
  }
}

TEST(PortableDataTest, RejectsMalformedVersionIdentityAndTimestamp) {
  auto snapshot = Snapshot();
  snapshot.Set("version", 2);
  EXPECT_FALSE(ValidatePortableData(snapshot).has_value());
  snapshot = Snapshot();
  snapshot.Set("transfer_id", "not-a-uuid");
  EXPECT_FALSE(ValidatePortableData(snapshot).has_value());
  snapshot = Snapshot();
  snapshot.Set("created_at", "not a timestamp");
  EXPECT_FALSE(ValidatePortableData(snapshot).has_value());
}

TEST(PortableDataTest, RejectsExecutableLocalAndCredentialedBookmarks) {
  for (const char* url : {"javascript:alert(1)", "file:///private/export",
                          "data:text/html,script", "chrome://settings/",
                          "https://user:secret@portable.invalid/"}) {
    auto snapshot = Snapshot();
    snapshot.FindList("bookmarks")->front().GetDict().Set("url", url);
    EXPECT_FALSE(ValidatePortableData(snapshot).has_value()) << url;
  }
}

TEST(PortableDataTest, RejectsAmbiguousOrExcessivelyNestedBookmarks) {
  auto snapshot = Snapshot();
  snapshot.FindList("bookmarks")
      ->front()
      .GetDict()
      .Set("children", base::ListValue());
  EXPECT_FALSE(ValidatePortableData(snapshot).has_value());
  snapshot = Snapshot();
  base::ListValue nested;
  for (int depth = 0; depth < 22; ++depth) {
    base::ListValue outer;
    outer.Append(base::DictValue()
                     .Set("title", "Folder")
                     .Set("children", std::move(nested)));
    nested = std::move(outer);
  }
  snapshot.Set("bookmarks", std::move(nested));
  EXPECT_FALSE(ValidatePortableData(snapshot).has_value());
}

TEST(PortableDataTest, RejectsUnselectedContentAndUnsupportedCategories) {
  auto snapshot = Snapshot();
  snapshot.FindDict("categories")->Set("cookies", true);
  EXPECT_FALSE(ValidatePortableData(snapshot).has_value());
  snapshot = Snapshot();
  snapshot.FindDict("categories")->Set("bookmarks", false);
  snapshot.FindDict("categories")->Set("passwords", true);
  EXPECT_FALSE(ValidatePortableData(snapshot).has_value());
  snapshot = Snapshot();
  snapshot.FindDict("categories")->Set("tabs", true);
  snapshot.FindList("tabs")->Append(Tab());
  EXPECT_FALSE(ValidatePortableData(snapshot).has_value());
}

TEST(PortableDataTest, RequiresUniqueSpacesAndPersonal) {
  auto snapshot = Snapshot();
  snapshot.FindDict("categories")->Set("spaces", true);
  snapshot.FindList("spaces")->Append(
      snapshot.FindList("spaces")->front().Clone());
  EXPECT_FALSE(ValidatePortableData(snapshot).has_value());
  snapshot = Snapshot();
  snapshot.FindDict("categories")->Set("spaces", true);
  snapshot.FindList("spaces")->front().GetDict().Set("archived", true);
  EXPECT_FALSE(ValidatePortableData(snapshot).has_value());
  snapshot.FindList("spaces")->front().GetDict().Set(
      "id", "238e0b5e-1959-49ba-9f71-22967faab7f0");
  EXPECT_FALSE(ValidatePortableData(snapshot).has_value());
}

TEST(PortableDataTest, AcceptsLegacySpacesAndOptionalThemePresets) {
  auto legacy = Snapshot();
  legacy.FindDict("categories")->Set("spaces", true);
  ASSERT_TRUE(ValidatePortableData(legacy).has_value());
  for (const char* theme :
       {"browser", "white-glass", "black-glass", "solid-white", "solid-black",
        "forest", "ocean", "sand"}) {
    auto snapshot = legacy.Clone();
    auto named = snapshot.FindList("spaces")->front().Clone();
    named.GetDict().Set("id", "238e0b5e-1959-49ba-9f71-22967faab7f0");
    named.GetDict().Set("name", "Work");
    named.GetDict().Set("theme", theme);
    snapshot.FindList("spaces")->Append(std::move(named));
    auto result = ValidatePortableData(snapshot);
    ASSERT_TRUE(result.has_value()) << theme << ": " << result.error();
    EXPECT_EQ(result->FindInt("spaces"), 1);
    EXPECT_FALSE(result->contains("theme"));
    EXPECT_FALSE(result->contains("space_appearance"));
  }
}

TEST(PortableDataTest, RejectsInvalidOptionalSpaceThemeOrUnknownFields) {
  for (const char* theme :
       {"", "default", "custom-theme", "https://theme.invalid"}) {
    auto snapshot = Snapshot();
    snapshot.FindDict("categories")->Set("spaces", true);
    snapshot.FindList("spaces")->front().GetDict().Set("theme", theme);
    EXPECT_FALSE(ValidatePortableData(snapshot).has_value()) << theme;
  }
  auto snapshot = Snapshot();
  snapshot.FindList("spaces")->front().GetDict().Set("theme", 1);
  EXPECT_FALSE(ValidatePortableData(snapshot).has_value());
  snapshot = Snapshot();
  snapshot.FindList("spaces")->front().GetDict().Set("unknown", "forest");
  EXPECT_FALSE(ValidatePortableData(snapshot).has_value());
  snapshot = Snapshot();
  auto& space = snapshot.FindList("spaces")->front().GetDict();
  space.Set("theme", "forest");
  space.Set("unknown", "forest");
  EXPECT_FALSE(ValidatePortableData(snapshot).has_value());
}

TEST(PortableDataTest, RejectsCrossSpaceReferencesAndNoncanonicalRoutes) {
  auto snapshot = Snapshot();
  snapshot.FindDict("categories")->Set("spaces", true);
  snapshot.FindDict("categories")->Set("tabs", true);
  auto tab = Tab();
  tab.Set("space_id", "238e0b5e-1959-49ba-9f71-22967faab7f0");
  snapshot.FindList("tabs")->Append(std::move(tab));
  EXPECT_FALSE(ValidatePortableData(snapshot).has_value());
  snapshot.FindList("tabs")->clear();
  snapshot.FindList("routes")->Append(
      base::DictValue()
          .Set("host", "https://portable.invalid/")
          .Set("space_id", ""));
  EXPECT_FALSE(ValidatePortableData(snapshot).has_value());
}

TEST(PortableDataTest, RejectsUnsafeTabsAndInvalidPinMetadata) {
  for (const char* url : {"file:///private/file", "chrome://flags/",
                          "chrome-extension://abcdefghijklmnop/page.html",
                          "https://user:secret@portable.invalid/"}) {
    auto snapshot = Snapshot();
    snapshot.FindDict("categories")->Set("spaces", true);
    snapshot.FindDict("categories")->Set("tabs", true);
    snapshot.FindList("tabs")->Append(Tab(url));
    EXPECT_FALSE(ValidatePortableData(snapshot).has_value()) << url;
  }
  auto snapshot = Snapshot();
  snapshot.FindDict("categories")->Set("spaces", true);
  snapshot.FindDict("categories")->Set("tabs", true);
  auto tab = Tab();
  tab.Set("pinned", true);
  tab.Set("name", "a\nmisleading title");
  snapshot.FindList("tabs")->Append(std::move(tab));
  EXPECT_FALSE(ValidatePortableData(snapshot).has_value());
}

TEST(PortableDataTest, ValidHomeAndPinnedTabsHaveOnlyCountReview) {
  auto snapshot = Snapshot();
  snapshot.FindDict("categories")->Set("spaces", true);
  snapshot.FindDict("categories")->Set("tabs", true);
  snapshot.FindList("tabs")->Append(Tab("chrome://newtab/"));
  auto pin = Tab();
  pin.Set("pinned", true);
  pin.Set("name", "Owned pin");
  pin.Set("home_url", "https://portable.invalid/home");
  snapshot.FindList("tabs")->Append(std::move(pin));
  auto result = ValidatePortableData(snapshot);
  ASSERT_TRUE(result.has_value()) << result.error();
  EXPECT_EQ(result->FindInt("tabs"), 2);
  EXPECT_EQ(result->FindInt("pins"), 1);
  EXPECT_TRUE(result->Find("tabs")->is_int());
}

TEST(PortableDataTest, RejectsNonstoreExtensionIdsAndExtraFields) {
  for (const char* id :
       {"not-an-extension", "abcdefghijklmnopabcdefghijklmnoq"}) {
    auto snapshot = Snapshot();
    snapshot.FindDict("categories")->Set("extensions", true);
    snapshot.FindList("extensions")
        ->Append(base::DictValue().Set("id", id).Set("name", "Owned"));
    EXPECT_FALSE(ValidatePortableData(snapshot).has_value());
  }
  auto snapshot = Snapshot();
  snapshot.FindDict("categories")->Set("extensions", true);
  snapshot.FindList("extensions")
      ->Append(base::DictValue()
                   .Set("id", "abcdefghijklmnopabcdefghijklmnop")
                   .Set("name", "Owned")
                   .Set("update_url", "https://unreviewed.invalid/"));
  EXPECT_FALSE(ValidatePortableData(snapshot).has_value());
}

TEST(PortableDataTest, PasswordPayloadIsBoundedAndNeverInReview) {
  auto snapshot = Snapshot();
  snapshot.Set("passwords_csv", "owned-only-fixture");
  EXPECT_FALSE(ValidatePortableData(snapshot).has_value());
  snapshot.FindDict("categories")->Set("passwords", true);
  auto result = ValidatePortableData(snapshot);
  ASSERT_TRUE(result.has_value()) << result.error();
  EXPECT_TRUE(result->FindBool("password_present").value());
  EXPECT_FALSE(result->contains("passwords_csv"));
  snapshot.Set("passwords_csv", std::string(1000 * 1024 + 1, 'x'));
  EXPECT_FALSE(ValidatePortableData(snapshot).has_value());
}

TEST(PortableDataTest, RejectsArbitraryPreferenceRestore) {
  auto snapshot = Snapshot();
  snapshot.FindDict("preferences")->Set("proxy", "https://unreviewed.invalid");
  EXPECT_FALSE(ValidatePortableData(snapshot).has_value());
}

}  // namespace
}  // namespace noventra
