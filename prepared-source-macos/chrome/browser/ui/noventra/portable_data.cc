// Copyright 2026 The Noventra Authors. All rights reserved.
#include "chrome/browser/ui/noventra/portable_data.h"

#include <algorithm>
#include <array>
#include <map>
#include <optional>
#include <set>
#include <string_view>
#include <utility>

#include "base/containers/span.h"
#include "base/functional/bind.h"
#include "base/functional/callback.h"
#include "base/i18n/time_formatting.h"
#include "base/json/json_writer.h"
#include "base/memory/ref_counted.h"
#include "base/no_destructor.h"
#include "base/strings/string_util.h"
#include "base/strings/utf_string_conversions.h"
#include "base/task/single_thread_task_runner.h"
#include "base/time/time.h"
#include "base/uuid.h"
#include "chrome/browser/bookmarks/bookmark_model_factory.h"
#include "chrome/browser/profiles/noventra_account_spaces.h"
#include "chrome/browser/profiles/noventra_space_profile_registry.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/sessions/session_service_base.h"
#include "chrome/browser/sessions/session_service_lookup.h"
#include "chrome/browser/themes/theme_service.h"
#include "chrome/browser/themes/theme_service_factory.h"
#include "chrome/browser/ui/browser_window/public/browser_window_interface.h"
#include "chrome/browser/ui/browser_window/public/global_browser_collection.h"
#include "chrome/browser/ui/navigator/browser_navigator.h"
#include "chrome/browser/ui/navigator/browser_navigator_params.h"
#include "chrome/browser/ui/noventra/incognito_space_host.h"
#include "chrome/browser/ui/noventra/layout_model.h"
#include "chrome/browser/ui/noventra/pinned_tab_state.h"
#include "chrome/browser/ui/noventra/workspace_routes.h"
#include "chrome/browser/ui/noventra_commands.h"
#include "chrome/browser/ui/tabs/tab_strip_model.h"
#include "chrome/common/pref_names.h"
#include "components/bookmarks/browser/bookmark_model.h"
#include "components/bookmarks/browser/bookmark_node.h"
#include "components/bookmarks/browser/scoped_group_bookmark_actions.h"
#include "components/bookmarks/common/bookmark_pref_names.h"
#include "components/prefs/pref_service.h"
#include "content/public/browser/navigation_entry.h"
#include "content/public/browser/restore_type.h"
#include "content/public/browser/site_instance.h"
#include "content/public/browser/web_contents.h"
#include "crypto/secure_util.h"
#include "extensions/browser/extension_registry.h"
#include "extensions/common/extension.h"
#include "extensions/common/manifest.h"
#include "url/gurl.h"

namespace noventra {
namespace {

constexpr size_t kMaximumBookmarks = 20000;
constexpr size_t kMaximumBookmarkDepth = 20;
constexpr size_t kMaximumSpaces = 1000;
constexpr size_t kMaximumTabs = 1000;
constexpr size_t kMaximumExtensions = 1000;
constexpr size_t kMaximumImports = 512;
constexpr size_t kMaximumTitleBytes = 4096;
constexpr size_t kMaximumURLBytes = 32 * 1024;
constexpr size_t kMaximumPasswordCSVBytes = 1000 * 1024;
constexpr std::string_view kCategoryNames[] = {
    "bookmarks", "spaces", "tabs", "settings", "extensions", "passwords"};
constexpr std::string_view kSpaceColors[] = {"default", "red",    "orange",
                                             "yellow",  "green",  "teal",
                                             "blue",    "purple", "pink"};
constexpr std::string_view kSpaceIcons[] = {
    "default", "work", "home", "code", "book", "globe", "music", "star"};
constexpr std::string_view kSpaceThemes[] = {
    "browser",     "white-glass", "black-glass", "solid-white",
    "solid-black", "forest",      "ocean",       "sand"};
constexpr std::string_view kBooleanPreferences[] = {
    "noventra.browser.glass_enabled",
    "noventra.browser.window_mirroring",
    "noventra.browser.link_preview",
    "noventra.browser.link_preview_pinned",
    "noventra.browser.sidebar_controls",
    "noventra.browser.tab_letters",
    "noventra.browser.address_on_demand",
    "noventra.browser.show_navigation_buttons",
    "browser.show_home_button",
    "browser.show_forward_button",
    "bookmark_bar.show_on_all_tabs"};
constexpr std::string_view kIntegerPreferences[] = {
    "noventra.browser.tab_density", "noventra.browser.bookmark_opening",
    "noventra.browser.link_preview_trigger"};
constexpr std::string_view kLayoutControls[] = {
    "back",       "forward", "reload", "home",
    "address",    "tabs",    "spaces", "saved-sites",
    "bookmarks",  "new-tab", "menu",   "customize",
    "extensions", "profile", "media",  "additional-actions"};

bool IsUUID(std::string_view value) {
  const auto parsed = base::Uuid::ParseLowercase(value);
  return parsed.is_valid() && parsed.AsLowercaseString() == value;
}

bool IsText(const std::string* text, size_t maximum, bool allow_empty = true) {
  return text && text->size() <= maximum && (allow_empty || !text->empty()) &&
         base::IsStringUTF8(*text) && text->find('\0') == std::string::npos;
}

bool IsName(const std::string* text, size_t maximum) {
  if (!IsText(text, maximum, false)) {
    return false;
  }
  std::string trimmed;
  base::TrimWhitespaceASCII(*text, base::TRIM_ALL, &trimmed);
  return trimmed == *text && !std::ranges::any_of(*text, [](unsigned char c) {
           return c < 0x20 || c == 0x7f;
         });
}

bool IsPortableURL(std::string_view text, bool allow_home) {
  const GURL url(text);
  if (text.size() > kMaximumURLBytes || !url.is_valid() || url.has_username() ||
      url.has_password()) {
    return false;
  }
  return url.SchemeIsHTTPOrHTTPS() ||
         (allow_home && (url == GURL("chrome://newtab/") ||
                         url == GURL("chrome://new-tab-page/") ||
                         url == GURL("about:blank")));
}

bool IsPinName(const std::string* text) {
  if (!IsText(text, 1024)) {
    return false;
  }
  const auto decoded = base::UTF8ToUTF16(*text);
  return decoded.size() <= 256 &&
         base::TrimWhitespace(decoded, base::TRIM_ALL) == decoded &&
         !std::ranges::any_of(decoded, [](char16_t character) {
           return character < 0x20 ||
                  (character >= 0x7f && character <= 0x9f) ||
                  character == 0x2028 || character == 0x2029 ||
                  (character >= 0x202a && character <= 0x202e) ||
                  (character >= 0x2066 && character <= 0x2069);
         });
}

bool IsRegularBrowser(BrowserWindowInterface* browser) {
  return browser && browser->GetProfile()->IsRegularProfile() &&
         !browser->GetProfile()->IsOffTheRecord() &&
         !browser->GetProfile()->IsGuestSession() &&
         browser->GetType() == BrowserWindowInterface::TYPE_NORMAL &&
         !browser->IsDeleteScheduled();
}

bool Selected(const base::DictValue& categories, std::string_view category) {
  return categories.FindBool(category).value_or(false);
}

base::expected<base::DictValue, std::string> NormalizeCategories(
    const base::DictValue& categories) {
  base::DictValue result;
  for (const auto [name, value] : categories) {
    if (std::ranges::find(kCategoryNames, name) == std::end(kCategoryNames) ||
        !value.is_bool()) {
      return base::unexpected("Choose supported transfer categories.");
    }
  }
  bool any = false;
  for (const auto name : kCategoryNames) {
    const bool selected = Selected(categories, name);
    result.Set(name, selected);
    any |= selected;
  }
  if (!any || (Selected(result, "tabs") && !Selected(result, "spaces"))) {
    return base::unexpected(
        "Choose at least one category. Tabs also require Spaces.");
  }
  return result;
}

bool ValidTheme(const base::DictValue& value) {
  const auto mode = value.FindInt("mode");
  const auto variant = value.FindInt("variant");
  const auto* palette = value.FindString("palette");
  return value.size() == 5 && mode && *mode >= 0 && *mode <= 2 && variant &&
         *variant >= 0 && *variant <= 4 && palette &&
         (*palette == "default" || *palette == "user" || *palette == "keep") &&
         value.FindInt("color").has_value() &&
         value.FindBool("grayscale").has_value() &&
         (*palette != "user" || !*value.FindBool("grayscale"));
}

bool ValidPreferences(const base::DictValue& preferences) {
  for (const auto [name, value] : preferences) {
    if (name == "theme") {
      if (!value.is_dict() || !ValidTheme(value.GetDict())) {
        return false;
      }
    } else if (std::ranges::find(kBooleanPreferences, name) !=
               std::end(kBooleanPreferences)) {
      if (!value.is_bool()) {
        return false;
      }
    } else if (std::ranges::find(kIntegerPreferences, name) !=
               std::end(kIntegerPreferences)) {
      const int maximum =
          name == "noventra.browser.link_preview_trigger" ? 2 : 1;
      if (!value.is_int() || value.GetInt() < 0 || value.GetInt() > maximum) {
        return false;
      }
    } else {
      return false;
    }
  }
  return true;
}

struct BookmarkCounts {
  int urls = 0;
  int folders = 0;
};

bool ValidateBookmarks(const base::ListValue& nodes,
                       size_t depth,
                       BookmarkCounts& counts) {
  if (depth > kMaximumBookmarkDepth || nodes.size() > kMaximumBookmarks) {
    return false;
  }
  for (const auto& value : nodes) {
    const auto* node = value.GetIfDict();
    if (!node || !IsText(node->FindString("title"), kMaximumTitleBytes)) {
      return false;
    }
    const auto* children = node->FindList("children");
    const auto* url = node->FindString("url");
    if (children) {
      if (node->size() != 2 || url ||
          !ValidateBookmarks(*children, depth + 1, counts)) {
        return false;
      }
      ++counts.folders;
    } else {
      if (node->size() != 2 || !url || !IsPortableURL(*url, false)) {
        return false;
      }
      ++counts.urls;
    }
    if (static_cast<size_t>(counts.urls + counts.folders) > kMaximumBookmarks) {
      return false;
    }
  }
  return true;
}

bool CollectBookmark(const bookmarks::BookmarkNode& node,
                     size_t depth,
                     size_t& total,
                     int& skipped,
                     base::ListValue& output) {
  if (depth > kMaximumBookmarkDepth || ++total > kMaximumBookmarks) {
    return false;
  }
  const auto title = base::UTF16ToUTF8(node.GetTitle());
  if (!IsText(&title, kMaximumTitleBytes) ||
      (node.is_url() && !IsPortableURL(node.url().spec(), false))) {
    ++skipped;
    return true;
  }
  base::DictValue result;
  result.Set("title", title);
  if (node.is_url()) {
    result.Set("url", node.url().spec());
  } else {
    base::ListValue children;
    for (const auto& child : node.children()) {
      if (!CollectBookmark(*child, depth + 1, total, skipped, children)) {
        return false;
      }
    }
    result.Set("children", std::move(children));
  }
  output.Append(std::move(result));
  return true;
}

void ApplyBookmarks(bookmarks::BookmarkModel& model,
                    const bookmarks::BookmarkNode* parent,
                    const base::ListValue& nodes) {
  for (const auto& value : nodes) {
    const auto& node = value.GetDict();
    const auto title = base::UTF8ToUTF16(*node.FindString("title"));
    if (const auto* children = node.FindList("children")) {
      const auto* folder =
          model.AddFolder(parent, parent->children().size(), title);
      ApplyBookmarks(model, folder, *children);
    } else {
      model.AddURL(parent, parent->children().size(), title,
                   GURL(*node.FindString("url")));
    }
  }
}

bool UserModifiable(const PrefService& prefs, std::string_view name) {
  const auto* preference = prefs.FindPreference(name);
  return preference && preference->IsUserModifiable();
}

std::optional<std::string> CheckPolicy(Profile* profile,
                                       const base::DictValue& categories,
                                       const base::DictValue& preferences) {
  const auto& prefs = *profile->GetPrefs();
  if (!UserModifiable(prefs, kPortableImportsPref)) {
    return "Transfers are unavailable for this managed profile.";
  }
  if (Selected(categories, "bookmarks") &&
      (!prefs.FindPreference(bookmarks::prefs::kEditBookmarksEnabled) ||
       !prefs.GetBoolean(bookmarks::prefs::kEditBookmarksEnabled))) {
    return "Your organization does not allow bookmark changes.";
  }
  if (Selected(categories, "spaces")) {
    auto* catalog = SpaceCatalogProfile(profile);
    if (!catalog) {
      return "Space settings are unavailable.";
    }
    for (const char* name : {kSpacesPref, kSpaceOrderPref, kArchivedSpacesPref,
                             kSpaceAppearancePref, kWorkspaceRoutesPref}) {
      if (!UserModifiable(*catalog->GetPrefs(), name)) {
        return "Your organization manages Space settings.";
      }
    }
  }
  if (Selected(categories, "settings")) {
    for (const auto [name, value] : preferences) {
      if (name == "theme") {
        for (const char* theme_pref :
             {prefs::kBrowserColorScheme, prefs::kUserColor,
              prefs::kBrowserColorVariant, prefs::kCurrentThemeID,
              prefs::kGrayscaleThemeEnabled}) {
          if (!UserModifiable(prefs, theme_pref)) {
            return "Your organization manages browser appearance.";
          }
        }
        auto* theme = ThemeServiceFactory::GetForProfile(profile);
        if (!theme || theme->UsingPolicyTheme()) {
          return "Your organization manages browser appearance.";
        }
      } else if (!UserModifiable(prefs, name)) {
        return "A selected browser setting is managed.";
      }
    }
    for (const auto& capability : GetLayoutCapabilities(*profile->GetPrefs())) {
      if (capability.managed) {
        return "Your organization manages the browser layout.";
      }
    }
  }
  return std::nullopt;
}

void MarkCompleted(PrefService& prefs,
                   std::string_view id,
                   const base::DictValue& entry) {
  auto ledger = prefs.GetDict(kPortableImportsPref).Clone();
  ledger.Set(id, entry.Clone());
  prefs.SetDict(kPortableImportsPref, std::move(ledger));
}

}  // namespace

PortableResult CollectPortableData(BrowserWindowInterface* browser,
                                   const base::DictValue& categories) {
  if (!IsRegularBrowser(browser)) {
    return base::unexpected("Transfer data from a regular browser window.");
  }
  auto selected = NormalizeCategories(categories);
  if (!selected.has_value()) {
    return base::unexpected(selected.error());
  }
  Profile* profile = browser->GetProfile();
  auto& prefs = *profile->GetPrefs();
  auto* catalog = SpaceCatalogProfile(profile);
  if (!catalog) {
    return base::unexpected("Space settings are unavailable.");
  }
  const auto& catalog_prefs = *catalog->GetPrefs();
  base::DictValue preferences;
  if (Selected(*selected, "settings")) {
    for (const auto name : kBooleanPreferences) {
      if (prefs.FindPreference(name)) {
        preferences.Set(name, prefs.GetBoolean(name));
      }
    }
    for (const auto name : kIntegerPreferences) {
      if (prefs.FindPreference(name)) {
        preferences.Set(name, prefs.GetInteger(name));
      }
    }
    if (auto* theme = ThemeServiceFactory::GetForProfile(profile)) {
      const auto color = theme->GetUserColor();
      const bool user = theme->GetThemeID() == ThemeService::kUserColorThemeID;
      preferences.Set(
          "theme",
          base::DictValue()
              .Set("mode", static_cast<int>(theme->GetBrowserColorScheme()))
              .Set("palette", user                         ? "user"
                              : theme->UsingDefaultTheme() ? "default"
                                                           : "keep")
              .Set("color", static_cast<int>(color.value_or(0)))
              .Set("variant", static_cast<int>(theme->GetBrowserColorVariant()))
              .Set("grayscale", theme->GetIsGrayscale()));
    }
  }
  if (auto error = CheckPolicy(profile, *selected, preferences)) {
    return base::unexpected(*error);
  }
  base::ListValue bookmarks;
  int skipped_bookmarks = 0;
  if (Selected(*selected, "bookmarks")) {
    auto* model = BookmarkModelFactory::GetForBrowserContext(profile);
    if (!model || !model->loaded()) {
      return base::unexpected(
          "Bookmarks are still loading. Try again shortly.");
    }
    size_t total = 0;
    // The managed root is intentionally absent. All six roots are user data;
    // account roots may be absent in this account-free build.
    const std::array<const bookmarks::BookmarkNode*, 6> roots = {
        model->bookmark_bar_node(),  model->other_node(),
        model->mobile_node(),        model->account_bookmark_bar_node(),
        model->account_other_node(), model->account_mobile_node()};
    for (const auto* root : roots) {
      if (root && !root->children().empty() &&
          !CollectBookmark(*root, 0, total, skipped_bookmarks, bookmarks)) {
        return base::unexpected(
            "This profile exceeds the transfer limit of 20,000 bookmarks or "
            "20 nested folders. Export a smaller bookmark selection first.");
      }
    }
  }
  base::ListValue spaces;
  base::ListValue routes;
  const auto ids = Selected(*selected, "spaces") ? SpaceIds(browser, true)
                                                 : std::vector<std::string>{""};
  if (ids.size() > kMaximumSpaces) {
    return base::unexpected("A transfer supports at most 1,000 Spaces.");
  }
  for (const auto& id : ids) {
    const auto* name = catalog_prefs.GetDict(kSpacesPref).FindString(id);
    const auto appearance = GetSpaceAppearance(profile, id);
    base::DictValue space;
    space.Set("id", id);
    space.Set("name", name ? *name : id.empty() ? "Personal" : "Account space");
    space.Set("archived", IsSpaceArchived(profile, id));
    space.Set("color", appearance.color);
    space.Set("icon", appearance.icon);
    // Inheriting Spaces keep the original five-field shape. Older transfers
    // remain readable; only an explicit Space theme needs the optional field.
    if (Selected(*selected, "spaces") && appearance.theme != "browser") {
      space.Set("theme", appearance.theme);
    }
    spaces.Append(std::move(space));
  }
  if (Selected(*selected, "spaces")) {
    for (const auto& route : GetWorkspaceRoutes(profile)) {
      if (std::ranges::find(ids, route.space_id) != ids.end()) {
        routes.Append(base::DictValue()
                          .Set("host", route.host)
                          .Set("space_id", route.space_id));
      }
    }
  }
  base::ListValue tabs;
  int skipped_tabs = 0;
  bool excessive_tabs = false;
  if (Selected(*selected, "tabs")) {
    GlobalBrowserCollection::GetInstance()->ForEach(
        [&](BrowserWindowInterface* window) {
          if (!IsRegularBrowser(window) || window->GetProfile() != profile) {
            return true;
          }
          auto* model = window->GetTabStripModel();
          for (int i = 0; i < model->count(); ++i) {
            auto* contents = model->GetWebContentsAt(i);
            // Lazy-restored entries may not have committed a renderer yet.
            // Their visible URL is the portable URL the user sees in the tab.
            const auto& url = contents->GetVisibleURL();
            const std::string space = SpaceId(contents);
            if (!IsPortableURL(url.spec(), true) ||
                std::ranges::find(ids, space) == ids.end()) {
              ++skipped_tabs;
              continue;
            }
            if (tabs.size() == kMaximumTabs) {
              excessive_tabs = true;
              return false;
            }
            const bool pinned = model->IsTabPinned(i);
            const auto* pin = pinned ? GetPinnedTabData(contents) : nullptr;
            const bool safe_home =
                pin && IsPortableURL(pin->home_url.spec(), true);
            tabs.Append(
                base::DictValue()
                    .Set("space_id", space)
                    .Set("url", url.spec())
                    .Set("pinned", pinned)
                    .Set("name", pin ? base::UTF16ToUTF8(pin->custom_name) : "")
                    .Set("home_url", safe_home ? pin->home_url.spec() : ""));
          }
          return true;
        });
  }
  if (excessive_tabs) {
    return base::unexpected("A transfer supports at most 1,000 open tabs.");
  }
  base::ListValue extensions;
  int skipped_extensions = 0;
  if (Selected(*selected, "extensions")) {
    auto* registry = extensions::ExtensionRegistry::Get(profile);
    if (registry) {
      for (const auto& extension : registry->GenerateInstalledExtensionsSet()) {
        if (!extension->from_webstore() || !extension->is_extension() ||
            extensions::Manifest::IsComponentLocation(extension->location()) ||
            extensions::Manifest::IsPolicyLocation(extension->location())) {
          ++skipped_extensions;
          continue;
        }
        if (extensions.size() == kMaximumExtensions) {
          return base::unexpected(
              "A transfer supports at most 1,000 extensions.");
        }
        extensions.Append(base::DictValue()
                              .Set("id", extension->id())
                              .Set("name", extension->name()));
      }
    }
  }
  base::DictValue layout;
  if (Selected(*selected, "settings")) {
    layout = LayoutToValue(ReadOrMigrateLayout(prefs));
    layout.Set("revision", 0);
  }
  base::DictValue snapshot;
  snapshot.Set("format", "noventra-portable-data");
  snapshot.Set("version", 1);
  snapshot.Set("transfer_id",
               base::Uuid::GenerateRandomV4().AsLowercaseString());
  snapshot.Set("created_at", base::TimeFormatAsIso8601(base::Time::Now()));
  snapshot.Set("categories", std::move(*selected));
  snapshot.Set("bookmarks", std::move(bookmarks));
  snapshot.Set("spaces", std::move(spaces));
  snapshot.Set("routes", std::move(routes));
  snapshot.Set("layout", std::move(layout));
  snapshot.Set("preferences", std::move(preferences));
  snapshot.Set("tabs", std::move(tabs));
  snapshot.Set("extensions", std::move(extensions));
  snapshot.Set("excluded", base::DictValue()
                               .Set("bookmarks", skipped_bookmarks)
                               .Set("tabs", skipped_tabs)
                               .Set("extensions", skipped_extensions));
  auto checked = ValidatePortableData(snapshot);
  if (!checked.has_value()) {
    return base::unexpected(checked.error());
  }
  return snapshot;
}

PortableResult ValidatePortableData(const base::DictValue& snapshot) {
  const auto* format = snapshot.FindString("format");
  const auto* id = snapshot.FindString("transfer_id");
  const auto* created = snapshot.FindString("created_at");
  const auto* categories = snapshot.FindDict("categories");
  const auto* bookmarks = snapshot.FindList("bookmarks");
  const auto* spaces = snapshot.FindList("spaces");
  const auto* routes = snapshot.FindList("routes");
  const auto* layout = snapshot.FindDict("layout");
  const auto* preferences = snapshot.FindDict("preferences");
  const auto* tabs = snapshot.FindList("tabs");
  const auto* extensions = snapshot.FindList("extensions");
  const auto* excluded = snapshot.FindDict("excluded");
  const auto* passwords = snapshot.FindString("passwords_csv");
  if (snapshot.size() != 13u + (snapshot.contains("passwords_csv") ? 1u : 0u) ||
      !format || *format != "noventra-portable-data" ||
      snapshot.FindInt("version") != 1 || !id || !IsUUID(*id) ||
      !IsText(created, 40, false) || !categories || categories->size() != 6 ||
      !bookmarks || !spaces || spaces->empty() ||
      spaces->size() > kMaximumSpaces || !routes ||
      routes->size() > kMaxWorkspaceRoutes || !layout || !preferences ||
      !tabs || tabs->size() > kMaximumTabs || !extensions ||
      extensions->size() > kMaximumExtensions || !excluded ||
      excluded->size() != 3) {
    return base::unexpected("This is not a supported Noventra transfer.");
  }
  const auto normalized = NormalizeCategories(*categories);
  if (!normalized.has_value() || *normalized != *categories) {
    return base::unexpected("The transfer has invalid category choices.");
  }
  for (const char* key : {"bookmarks", "tabs", "extensions"}) {
    const auto count = excluded->FindInt(key);
    if (!count || *count < 0 || *count > 10000000) {
      return base::unexpected("The transfer has invalid skipped-item counts.");
    }
  }
  base::Time created_time;
  if (!base::Time::FromUTCString(created->c_str(), &created_time) ||
      created_time.is_null()) {
    return base::unexpected("The transfer has an invalid creation date.");
  }
  BookmarkCounts bookmark_counts;
  if (!ValidateBookmarks(*bookmarks, 0, bookmark_counts)) {
    return base::unexpected(
        "The transfer contains invalid or excessive bookmarks.");
  }
  std::set<std::string> space_ids;
  for (const auto& value : *spaces) {
    const auto* space = value.GetIfDict();
    const auto* space_id = space ? space->FindString("id") : nullptr;
    const auto* color = space ? space->FindString("color") : nullptr;
    const auto* icon = space ? space->FindString("icon") : nullptr;
    const auto* theme = space ? space->FindString("theme") : nullptr;
    const bool has_theme = space && space->contains("theme");
    const auto archived = space ? space->FindBool("archived") : std::nullopt;
    if (!space || space->size() != 5u + (has_theme ? 1u : 0u) || !space_id ||
        (!space_id->empty() && !IsUUID(*space_id)) ||
        !space_ids.insert(*space_id).second ||
        !IsName(space->FindString("name"), 160) || !archived ||
        (space_id->empty() && *archived) || !color || !icon ||
        std::ranges::find(kSpaceColors, *color) == std::end(kSpaceColors) ||
        std::ranges::find(kSpaceIcons, *icon) == std::end(kSpaceIcons) ||
        (has_theme && (!theme || std::ranges::find(kSpaceThemes, *theme) ==
                                     std::end(kSpaceThemes)))) {
      return base::unexpected(
          "The transfer contains an invalid or duplicate Space.");
    }
  }
  if (!space_ids.contains("")) {
    return base::unexpected("The transfer must include Personal.");
  }
  std::set<std::string> hosts;
  for (const auto& value : *routes) {
    const auto* route = value.GetIfDict();
    const auto* host = route ? route->FindString("host") : nullptr;
    const auto* space = route ? route->FindString("space_id") : nullptr;
    if (!route || route->size() != 2 || !host || !space ||
        !space_ids.contains(*space)) {
      return base::unexpected("The transfer contains an invalid domain rule.");
    }
    const auto canonical = CanonicalWorkspaceRouteHost(*host);
    if (!canonical.has_value() || *canonical != *host ||
        !hosts.insert(*host).second) {
      return base::unexpected(
          "The transfer contains an invalid or duplicate domain.");
    }
  }
  if (!ValidPreferences(*preferences)) {
    return base::unexpected(
        "The transfer contains an unsupported browser setting.");
  }
  if (Selected(*categories, "settings")) {
    // Validate the portable shape here. The destination's currently available
    // controls, policy and required visibility are checked before any write.
    if (layout->size() != 3 || layout->FindInt("version") != 1 ||
        layout->FindInt("revision") != 0) {
      return base::unexpected("The transfer contains an unsupported layout.");
    }
    const auto* items = layout->FindList("items");
    if (!items || items->empty() || items->size() > 128) {
      return base::unexpected("The transfer contains an invalid layout.");
    }
    std::set<std::string> controls;
    std::set<std::pair<std::string, int>> slots;
    for (const auto& item_value : *items) {
      const auto* item = item_value.GetIfDict();
      const auto* control = item ? item->FindString("id") : nullptr;
      const auto* edge = item ? item->FindString("edge") : nullptr;
      const auto order = item ? item->FindInt("order") : std::nullopt;
      if (!item || item->size() != 4 || !IsName(control, 80) || !edge ||
          (*edge != "top" && *edge != "left" && *edge != "right") || !order ||
          *order < 0 || *order >= 256 || !item->FindBool("visible") ||
          (std::ranges::find(kLayoutControls, *control) ==
               std::end(kLayoutControls) &&
           !NativeDockActionId(*control).has_value()) ||
          (*control == "spaces" && *edge == "top") ||
          (*control == "tabs" && !*item->FindBool("visible")) ||
          !controls.insert(*control).second ||
          !slots.insert({*edge, *order}).second) {
        return base::unexpected(
            "The transfer contains an invalid layout control.");
      }
    }
  } else if (!layout->empty() || !preferences->empty()) {
    return base::unexpected(
        "The transfer contains unselected browser settings.");
  }
  int pins = 0;
  for (const auto& value : *tabs) {
    const auto* tab = value.GetIfDict();
    const auto* space = tab ? tab->FindString("space_id") : nullptr;
    const auto* url = tab ? tab->FindString("url") : nullptr;
    const auto* home = tab ? tab->FindString("home_url") : nullptr;
    const auto* name = tab ? tab->FindString("name") : nullptr;
    const auto pinned = tab ? tab->FindBool("pinned") : std::nullopt;
    if (!tab || tab->size() != 5 || !space || !space_ids.contains(*space) ||
        !url || !IsPortableURL(*url, true) || !pinned || !IsPinName(name) ||
        !home || (!home->empty() && !IsPortableURL(*home, true)) ||
        (!*pinned && (!name->empty() || !home->empty()))) {
      return base::unexpected("The transfer contains an invalid tab or pin.");
    }
    pins += *pinned ? 1 : 0;
  }
  std::set<std::string> extension_ids;
  for (const auto& value : *extensions) {
    const auto* extension = value.GetIfDict();
    const auto* extension_id =
        extension ? extension->FindString("id") : nullptr;
    if (!extension || extension->size() != 2 || !extension_id ||
        extension_id->size() != 32 ||
        !std::ranges::all_of(*extension_id,
                             [](char c) { return c >= 'a' && c <= 'p'; }) ||
        !extension_ids.insert(*extension_id).second ||
        !IsText(extension->FindString("name"), 1024, false)) {
      return base::unexpected("The transfer contains an invalid extension.");
    }
  }
  if ((!Selected(*categories, "bookmarks") && !bookmarks->empty()) ||
      (!Selected(*categories, "spaces") &&
       (spaces->size() != 1 || !routes->empty())) ||
      (!Selected(*categories, "tabs") && !tabs->empty()) ||
      (!Selected(*categories, "extensions") && !extensions->empty()) ||
      (snapshot.contains("passwords_csv") &&
       (!passwords || passwords->size() > kMaximumPasswordCSVBytes ||
        !Selected(*categories, "passwords") ||
        !base::IsStringUTF8(*passwords)))) {
    return base::unexpected("The transfer contains unsupported category data.");
  }
  auto serialized = base::WriteJson(snapshot);
  const bool within_limit =
      serialized && serialized->size() <= kPortableMaximumBytes;
  if (serialized) {
    // The bounded-size check creates an owned serialization; clear its native
    // plaintext copy before returning even though no bytes reach the WebUI.
    crypto::SecureZeroBuffer(base::as_writable_byte_span(*serialized));
  }
  if (!within_limit) {
    return base::unexpected("The transfer exceeds the 32 MB data limit.");
  }
  return base::DictValue()
      .Set("transfer_id", *id)
      .Set("created_at", *created)
      .Set("categories", categories->Clone())
      .Set("bookmarks", bookmark_counts.urls)
      .Set("bookmark_folders", bookmark_counts.folders)
      .Set("spaces", static_cast<int>(spaces->size() - 1))
      .Set("tabs", static_cast<int>(tabs->size()))
      .Set("pins", pins)
      .Set("settings", static_cast<int>(preferences->size()))
      .Set("extensions", static_cast<int>(extensions->size()))
      .Set("extension_items", extensions->Clone())
      .Set("excluded", excluded->Clone())
      .Set("password_present", passwords && !passwords->empty());
}

namespace {
struct PendingPortableTabs {
  base::ListValue tabs;
  std::map<std::string, std::string> space_map;
  base::DictValue entry;
  std::string transfer_id;
  size_t next = 0;
};

// A transfer may be requested from two Settings windows. Its saved progress
// has one writer while asynchronous profile creation lets other UI work run.
class PortableImportLease {
 public:
  using Key = std::pair<base::FilePath, std::string>;
  static std::unique_ptr<PortableImportLease> Acquire(Key key) {
    if (!Active().insert(key).second) {
      return nullptr;
    }
    return std::unique_ptr<PortableImportLease>(
        new PortableImportLease(std::move(key)));
  }
  ~PortableImportLease() { Active().erase(key_); }

 private:
  explicit PortableImportLease(Key key) : key_(std::move(key)) {}
  static std::set<Key>& Active() {
    static base::NoDestructor<std::set<Key>> active;
    return *active;
  }
  Key key_;
};

PortableResult PreparePortableData(
    BrowserWindowInterface* browser,
    const base::DictValue& snapshot,
    const base::DictValue& categories,
    int expected_layout_revision,
    std::unique_ptr<PendingPortableTabs>* pending) {
  if (!IsRegularBrowser(browser)) {
    return base::unexpected("Import data in a regular browser window.");
  }
  auto review = ValidatePortableData(snapshot);
  if (!review.has_value()) {
    return base::unexpected(review.error());
  }
  auto selected = NormalizeCategories(categories);
  if (!selected.has_value()) {
    return base::unexpected(selected.error());
  }
  const auto& available = *snapshot.FindDict("categories");
  for (const auto category : kCategoryNames) {
    if (Selected(*selected, category) && !Selected(available, category)) {
      return base::unexpected(
          "A selected category is not present in this transfer.");
    }
  }
  Profile* profile = browser->GetProfile();
  auto& prefs = *profile->GetPrefs();
  auto* catalog = SpaceCatalogProfile(profile);
  if (!catalog) {
    return base::unexpected("Space settings are unavailable.");
  }
  auto& catalog_prefs = *catalog->GetPrefs();
  if (auto error =
          CheckPolicy(profile, *selected, *snapshot.FindDict("preferences"))) {
    return base::unexpected(*error);
  }
  const auto& id = *snapshot.FindString("transfer_id");
  const auto& ledger = prefs.GetDict(kPortableImportsPref);
  const auto* previous = ledger.FindDict(id);
  if ((!previous && ledger.size() >= kMaximumImports) ||
      (ledger.contains(id) && !previous)) {
    return base::unexpected(
        "This profile cannot record another transfer safely.");
  }
  base::DictValue entry = previous ? previous->Clone() : base::DictValue();
  const auto done = [&](std::string_view category) {
    return entry.FindBool(category).value_or(false);
  };
  const auto wants = [&](std::string_view category) {
    return Selected(*selected, category) && !done(category);
  };
  // Named Space themes are explicit appearance requests. Do not silently save
  // a choice that the destination's appearance policy forbids. This check runs
  // before any category is applied and leaves the review available for retry.
  if (wants("spaces") && !CanCustomizeSpaceTheme(browser) &&
      std::ranges::any_of(*snapshot.FindList("spaces"), [](const auto& value) {
        const auto& space = value.GetDict();
        const auto* theme = space.FindString("theme");
        return !space.FindString("id")->empty() && theme && *theme != "browser";
      })) {
    return base::unexpected(
        "Space themes cannot be changed in this browser. Leave Spaces "
        "unchecked "
        "or use a window where appearance can be changed.");
  }
  const int restored_tabs = entry.FindInt("tabs_completed").value_or(0);
  if (restored_tabs < 0 ||
      static_cast<size_t>(restored_tabs) > snapshot.FindList("tabs")->size()) {
    return base::unexpected("The previous transfer record is invalid.");
  }
  const auto current_id =
      ProfileSpacesEnabled() ? ProfileSpaceId(profile) : std::string();
  std::map<std::string, std::string> space_map{{"", current_id}};
  if (const auto* saved_map = entry.FindDict("space_map")) {
    if (saved_map->size() > kMaximumSpaces) {
      return base::unexpected("The previous transfer record is invalid.");
    }
    std::set<std::string> local_ids;
    std::set<std::string> source_ids;
    for (const auto& space : *snapshot.FindList("spaces")) {
      source_ids.insert(*space.GetDict().FindString("id"));
    }
    for (const auto [source, local] : *saved_map) {
      if (!local.is_string() || (!source.empty() && !IsUUID(source)) ||
          (!local.GetString().empty() && !IsUUID(local.GetString())) ||
          (source.empty() ? local.GetString() != current_id
                          : local.GetString().empty()) ||
          !source_ids.contains(source) ||
          !local_ids.insert(local.GetString()).second) {
        return base::unexpected("The previous transfer record is invalid.");
      }
      space_map[source] = local.GetString();
    }
    if (done("spaces") && saved_map->size() != source_ids.size()) {
      return base::unexpected("The previous transfer record is invalid.");
    }
  } else if (done("spaces")) {
    return base::unexpected("The previous transfer record is invalid.");
  }
  const auto known_spaces = SpaceIds(browser, true);
  base::DictValue names = catalog_prefs.GetDict(kSpacesPref).Clone();
  base::ListValue order = catalog_prefs.GetList(kSpaceOrderPref).Clone();
  base::ListValue archived = catalog_prefs.GetList(kArchivedSpacesPref).Clone();
  base::DictValue appearance =
      catalog_prefs.GetDict(kSpaceAppearancePref).Clone();
  int added_spaces = 0;
  if (wants("spaces")) {
    for (const auto& value : *snapshot.FindList("spaces")) {
      const auto& space = value.GetDict();
      const auto& source = *space.FindString("id");
      if (source.empty()) {
        continue;
      }
      const std::string local =
          base::Uuid::GenerateRandomV4().AsLowercaseString();
      space_map[source] = local;
      names.Set(local, *space.FindString("name"));
      order.Append(local);
      if (*space.FindBool("archived")) {
        archived.Append(local);
      }
      base::DictValue imported_appearance;
      imported_appearance.Set("color", *space.FindString("color"));
      imported_appearance.Set("icon", *space.FindString("icon"));
      if (const auto* theme = space.FindString("theme");
          theme && *theme != "browser") {
        imported_appearance.Set("theme", *theme);
      }
      appearance.Set(local, std::move(imported_appearance));
      ++added_spaces;
    }
    if (names.size() > kMaximumSpaces * 2) {
      return base::unexpected("Import would exceed 2,000 local Spaces.");
    }
  }
  if (wants("tabs")) {
    for (const auto& value : *snapshot.FindList("tabs")) {
      const auto& source = *value.GetDict().FindString("space_id");
      const auto found = space_map.find(source);
      if (found == space_map.end() ||
          (!found->second.empty() && !wants("spaces") &&
           std::ranges::find(known_spaces, found->second) ==
               known_spaces.end())) {
        return base::unexpected(
            "A Space from this transfer is no longer available. Existing data "
            "was kept.");
      }
    }
    if (browser->GetTabStripModel()->closing_all() ||
        browser->GetTabStripModel()->count() +
                snapshot.FindList("tabs")->size() - restored_tabs >
            2000) {
      return base::unexpected(
          "Close some tabs before importing more than 2,000.");
    }
  }
  auto merged_routes = catalog_prefs.GetList(kWorkspaceRoutesPref).Clone();
  int added_routes = 0;
  if (wants("spaces")) {
    std::set<std::string> existing_hosts;
    for (const auto& route : GetWorkspaceRoutes(profile)) {
      existing_hosts.insert(route.host);
    }
    // Existing route validation is shared with normal Space settings. Preserve
    // only current valid routes rather than opaque user-pref records.
    merged_routes.clear();
    for (const auto& route : GetWorkspaceRoutes(profile)) {
      merged_routes.Append(base::DictValue()
                               .Set("host", route.host)
                               .Set("space_id", route.space_id));
    }
    for (const auto& value : *snapshot.FindList("routes")) {
      const auto& route = value.GetDict();
      const auto& host = *route.FindString("host");
      if (existing_hosts.insert(host).second) {
        merged_routes.Append(
            base::DictValue()
                .Set("host", host)
                .Set("space_id", space_map.at(*route.FindString("space_id"))));
        ++added_routes;
      }
    }
    if (merged_routes.size() > kMaxWorkspaceRoutes) {
      return base::unexpected("Import would exceed 100 domain rules.");
    }
  }
  bookmarks::BookmarkModel* bookmarks = nullptr;
  if (wants("bookmarks")) {
    bookmarks = BookmarkModelFactory::GetForBrowserContext(profile);
    if (!bookmarks || !bookmarks->loaded() || !bookmarks->other_node()) {
      return base::unexpected(
          "Bookmarks are still loading. Try again shortly.");
    }
  }
  std::optional<BrowserLayout> layout;
  if (wants("settings")) {
    if (expected_layout_revision < 0 ||
        ReadOrMigrateLayout(prefs).revision != expected_layout_revision) {
      return base::unexpected("Settings changed. Review the transfer again.");
    }
    auto checked =
        ValidateLayout(base::Value(snapshot.FindDict("layout")->Clone()),
                       GetLayoutCapabilities(prefs));
    if (!checked.has_value()) {
      return base::unexpected("Cannot import this layout: " + checked.error());
    }
    checked->revision = expected_layout_revision;
    layout = std::move(*checked);
  }

  // No persistent mutation precedes the complete source/policy/staleness check.
  // Layout can still refuse a policy-required control; do it before metadata.
  if (layout) {
    const auto outcome = ApplyLayout(prefs, *layout, expected_layout_revision);
    if (outcome.status != "ok") {
      return base::unexpected(outcome.message);
    }
    for (const auto [name, value] : *snapshot.FindDict("preferences")) {
      if (name == "theme") {
        const auto& theme_data = value.GetDict();
        auto* theme = ThemeServiceFactory::GetForProfile(profile);
        const auto& palette = *theme_data.FindString("palette");
        if (palette != "keep" && *theme_data.FindBool("grayscale")) {
          theme->SetIsGrayscale(true);
        } else if (palette == "default") {
          theme->UseDefaultTheme();
          theme->SetBrowserColorVariant(
              static_cast<ui::mojom::BrowserColorVariant>(
                  *theme_data.FindInt("variant")));
        } else if (palette == "user") {
          theme->SetUserColorAndBrowserColorVariant(
              static_cast<SkColor>(*theme_data.FindInt("color")),
              static_cast<ui::mojom::BrowserColorVariant>(
                  *theme_data.FindInt("variant")));
        }
        theme->SetBrowserColorScheme(
            static_cast<ThemeService::BrowserColorScheme>(
                *theme_data.FindInt("mode")));
      } else {
        prefs.Set(name, value);
      }
    }
    entry.Set("settings", true);
    MarkCompleted(prefs, id, entry);
  }
  if (wants("spaces")) {
    catalog_prefs.SetList(kSpaceOrderPref, std::move(order));
    catalog_prefs.SetList(kArchivedSpacesPref, std::move(archived));
    catalog_prefs.SetDict(kSpaceAppearancePref, std::move(appearance));
    catalog_prefs.SetDict(kSpacesPref, std::move(names));
    catalog_prefs.SetList(kWorkspaceRoutesPref, std::move(merged_routes));
    base::DictValue saved_map;
    for (const auto& [source, local] : space_map) {
      saved_map.Set(source, local);
    }
    entry.Set("space_map", std::move(saved_map));
    entry.Set("spaces", true);
    MarkCompleted(prefs, id, entry);
  }
  int added_bookmarks = 0;
  if (wants("bookmarks")) {
    if (!snapshot.FindList("bookmarks")->empty()) {
      bookmarks::ScopedGroupBookmarkActions group(bookmarks);
      const auto* parent = bookmarks->other_node();
      const auto* imported = bookmarks->AddFolder(
          parent, parent->children().size(), u"Imported from Noventra");
      ApplyBookmarks(*bookmarks, imported, *snapshot.FindList("bookmarks"));
    }
    added_bookmarks = *review->FindInt("bookmarks");
    entry.Set("bookmarks", true);
    MarkCompleted(prefs, id, entry);
  }
  const int added_tabs = 0;
  if (wants("tabs")) {
    *pending = std::make_unique<PendingPortableTabs>();
    (*pending)->tabs = snapshot.FindList("tabs")->Clone();
    (*pending)->space_map = std::move(space_map);
    (*pending)->entry = entry.Clone();
    (*pending)->transfer_id = id;
    (*pending)->next = restored_tabs;
  }
  base::ListValue extension_items;
  if (Selected(*selected, "extensions")) {
    extension_items = snapshot.FindList("extensions")->Clone();
    // This is the reviewed list, not an installation success marker.
  }
  prefs.CommitPendingWrite();
  if (&catalog_prefs != &prefs) {
    catalog_prefs.CommitPendingWrite();
  }
  return base::DictValue()
      .Set("partial", false)
      .Set("bookmarks", added_bookmarks)
      .Set("spaces", added_spaces)
      .Set("routes", added_routes)
      .Set("tabs", added_tabs)
      .Set("settings", layout.has_value())
      .Set("extension_items", std::move(extension_items))
      .Set("already_imported", added_bookmarks == 0 && added_spaces == 0 &&
                                   added_tabs == 0 && !layout.has_value());
}

class PortableImportTabs : public base::RefCounted<PortableImportTabs> {
 public:
  PortableImportTabs(base::WeakPtr<BrowserWindowInterface> source,
                     std::unique_ptr<PendingPortableTabs> plan,
                     base::DictValue result,
                     std::unique_ptr<PortableImportLease> lease,
                     base::OnceCallback<void(PortableResult)> callback,
                     base::RepeatingCallback<bool()> can_continue)
      : source_(std::move(source)),
        plan_(std::move(plan)),
        result_(std::move(result)),
        lease_(std::move(lease)),
        callback_(std::move(callback)),
        can_continue_(std::move(can_continue)) {}

  void Next() {
    if (!CanContinue()) {
      Finish(true);
      return;
    }
    if (plan_->next == plan_->tabs.size()) {
      plan_->entry.Set("tabs", true);
      auto& prefs = *source_->GetProfile()->GetPrefs();
      MarkCompleted(prefs, plan_->transfer_id, plan_->entry);
      prefs.CommitPendingWrite();
      Finish(false);
      return;
    }
    const auto& tab = plan_->tabs[plan_->next].GetDict();
    const auto& id = plan_->space_map.at(*tab.FindString("space_id"));
    if (ProfileSpacesEnabled()) {
      AcquireProfileSpaceBrowser(source_.get(), id,
                                 base::BindOnce(&PortableImportTabs::Insert,
                                                base::WrapRefCounted(this)),
                                 /*select=*/false);
    } else {
      Insert(source_.get());
    }
  }

 private:
  friend class base::RefCounted<PortableImportTabs>;
  ~PortableImportTabs() = default;

  bool CanContinue() const {
    return callback_ && source_ && IsRegularBrowser(source_.get()) &&
           !source_->GetProfile()->ShutdownStarted() &&
           !source_->GetTabStripModel()->closing_all() &&
           (!can_continue_ || can_continue_.Run());
  }

  void Insert(BrowserWindowInterface* target) {
    if (!CanContinue() || !target || !IsRegularBrowser(target) ||
        target->GetProfile()->ShutdownStarted() ||
        target->GetTabStripModel()->closing_all()) {
      Finish(true);
      return;
    }
    const auto& tab = plan_->tabs[plan_->next].GetDict();
    const auto& id = plan_->space_map.at(*tab.FindString("space_id"));
    auto* profile = target->GetProfile();
    if ((ProfileSpacesEnabled() &&
         (ProfileSpaceId(profile) != id ||
          SpaceCatalogProfile(profile) !=
              SpaceCatalogProfile(source_->GetProfile()))) ||
        target->GetTabStripModel()->count() >= 2000) {
      Finish(true);
      return;
    }
    const GURL url(*tab.FindString("url"));
    const auto instance =
        id.empty() ? content::SiteInstance::CreateForURL(profile, url)
                   : SpaceSiteInstance(profile, url, id);
    content::WebContents::CreateParams create(profile, instance);
    create.initially_hidden = true;
    create.desired_renderer_state =
        content::WebContents::CreateParams::kNoRendererProcess;
    auto contents_to_insert = content::WebContents::Create(create);
    auto navigation = content::NavigationEntry::Create();
    navigation->SetURL(url);
    navigation->SetVirtualURL(url);
    std::vector<std::unique_ptr<content::NavigationEntry>> history;
    history.push_back(std::move(navigation));
    contents_to_insert->GetController().Restore(
        0, content::RestoreType::kRestored, &history);
    NavigateParams params(target, std::move(contents_to_insert));
    params.disposition = WindowOpenDisposition::NEW_BACKGROUND_TAB;
    params.tabstrip_index = target->GetTabStripModel()->count();
    params.tabstrip_add_types =
        AddTabTypes::ADD_FORCE_INDEX |
        (*tab.FindBool("pinned") ? AddTabTypes::ADD_PINNED
                                 : AddTabTypes::ADD_NONE);
    params.source_site_instance = instance;
    params.window_action = NavigateParams::WindowAction::kNoAction;
    Navigate(&params);
    auto* contents = params.navigated_or_inserted_contents.get();
    if (!contents || contents->GetBrowserContext() != profile ||
        SpaceId(contents) != id) {
      Finish(true);
      return;
    }
    if (*tab.FindBool("pinned")) {
      const auto& home = *tab.FindString("home_url");
      SetPinnedTabData(contents, home.empty() ? url : GURL(home),
                       base::UTF8ToUTF16(*tab.FindString("name")));
    }
    if (auto* service = GetAppropriateSessionServiceIfExisting(target)) {
      service->TabRestored(contents, *tab.FindBool("pinned"));
    }
    ++added_;
    ++plan_->next;
    plan_->entry.Set("tabs_completed", static_cast<int>(plan_->next));
    MarkCompleted(*source_->GetProfile()->GetPrefs(), plan_->transfer_id,
                  plan_->entry);
    base::SingleThreadTaskRunner::GetCurrentDefault()->PostTask(
        FROM_HERE,
        base::BindOnce(&PortableImportTabs::Next, base::WrapRefCounted(this)));
  }

  void Finish(bool partial) {
    if (!callback_) {
      return;
    }
    if (source_ && !source_->GetProfile()->ShutdownStarted()) {
      source_->GetProfile()->GetPrefs()->CommitPendingWrite();
    }
    result_.Set("partial", partial);
    result_.Set("tabs", added_);
    if (partial) {
      result_.Set("message",
                  "Some tabs could not be opened. Completed imports and "
                  "existing work were kept.");
    }
    if (partial || added_) {
      result_.Set("already_imported", false);
    }
    lease_.reset();
    std::move(callback_).Run(std::move(result_));
  }

  base::WeakPtr<BrowserWindowInterface> source_;
  std::unique_ptr<PendingPortableTabs> plan_;
  base::DictValue result_;
  std::unique_ptr<PortableImportLease> lease_;
  base::OnceCallback<void(PortableResult)> callback_;
  base::RepeatingCallback<bool()> can_continue_;
  int added_ = 0;
};
}  // namespace

void ApplyPortableData(BrowserWindowInterface* browser,
                       const base::DictValue& snapshot,
                       const base::DictValue& categories,
                       int expected_layout_revision,
                       base::OnceCallback<void(PortableResult)> callback,
                       base::RepeatingCallback<bool()> can_continue) {
  if (can_continue && !can_continue.Run()) {
    std::move(callback).Run(base::unexpected("Transfer canceled."));
    return;
  }
  const auto* id = snapshot.FindString("transfer_id");
  if (!IsRegularBrowser(browser) || !id || !IsUUID(*id)) {
    std::move(callback).Run(base::unexpected(
        "Choose a valid transfer in a regular browser window."));
    return;
  }
  auto lease =
      PortableImportLease::Acquire({browser->GetProfile()->GetPath(), *id});
  if (!lease) {
    std::move(callback).Run(base::unexpected(
        "This transfer is already being imported in this profile."));
    return;
  }
  std::unique_ptr<PendingPortableTabs> pending;
  auto result = PreparePortableData(browser, snapshot, categories,
                                    expected_layout_revision, &pending);
  if (!result.has_value() || !pending) {
    lease.reset();
    std::move(callback).Run(std::move(result));
    return;
  }
  auto run = base::MakeRefCounted<PortableImportTabs>(
      browser->GetWeakPtr(), std::move(pending), std::move(*result),
      std::move(lease), std::move(callback), std::move(can_continue));
  run->Next();
}

}  // namespace noventra
