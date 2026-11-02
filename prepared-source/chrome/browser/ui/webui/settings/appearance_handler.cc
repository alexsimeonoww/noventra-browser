#include "chrome/browser/profiles/noventra_account_spaces.h"
#include "chrome/browser/ui/noventra/layout_model.h"
#include "chrome/browser/ui/noventra/workspace_routes.h"
#include "chrome/browser/ui/noventra/workspace_sync_service.h"
#include "chrome/browser/ui/noventra_commands.h"
// Copyright 2015 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <algorithm>
#include <map>
#include <set>

#include "base/check.h"
#include "base/functional/bind.h"
#include "base/json/json_reader.h"
#include "base/strings/string_util.h"
#include "base/values.h"
#include "build/build_config.h"
#include "chrome/app/chrome_command_ids.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/themes/theme_service_factory.h"
#include "chrome/browser/ui/browser_commands.h"
#include "chrome/browser/ui/browser_window/public/browser_window_interface.h"
#include "chrome/browser/ui/browser_window/public/global_browser_collection.h"
#include "chrome/browser/ui/helium/helium_layout_state_controller.h"
#include "chrome/browser/ui/side_panel/side_panel_action_callback.h"
#include "chrome/browser/ui/side_panel/side_panel_enums.h"
#include "chrome/browser/ui/tabs/vertical_tab_strip_metrics.h"
#include "chrome/browser/ui/tabs/vertical_tab_strip_state_controller.h"
#include "chrome/browser/ui/toolbar/pinned_toolbar/pinned_toolbar_actions_model.h"
#include "chrome/browser/ui/webui/settings/appearance_handler.h"
#include "chrome/common/pref_names.h"
#include "components/prefs/pref_service.h"
#include "components/themes/pref_names.h"
#include "content/public/browser/web_ui.h"
#include "extensions/common/extension.h"
#include "ui/actions/actions.h"

namespace settings {

AppearanceHandler::AppearanceHandler(content::WebUI* webui)
    : profile_(Profile::FromWebUI(webui)) {}

AppearanceHandler::~AppearanceHandler() = default;

void AppearanceHandler::OnJavascriptAllowed() {
  layout_registrar_.Init(profile_->GetPrefs());
  for (const char* key : {noventra::kDockLayoutPref, "extensions.management",
       "browser.show_home_button", "helium.browser.show_extensions_button",
       "helium.browser.show_back_button", "browser.show_forward_button",
       "helium.browser.show_reload_button", "helium.browser.show_avatar_button",
       "helium.browser.show_menu_button", "helium.browser.show_media_button",
       "helium.browser.show_dynamic_new_tab_button"})
    if (profile_->GetPrefs()->FindPreference(key))
      layout_registrar_.Add(key, base::BindRepeating(&AppearanceHandler::NotifyNoventraLayoutState, base::Unretained(this)));
  for (const char* key :
       {noventra::kSpacesPref, noventra::kSpaceOrderPref,
        noventra::kArchivedSpacesPref, noventra::kWorkspaceRoutesPref,
        noventra::kSpaceAppearancePref}) {
    layout_registrar_.Add(
        key,
        base::BindRepeating(&AppearanceHandler::NotifyNoventraSpacesChanged,
                            base::Unretained(this)));
  }
  // Refresh theme availability when policy or the browser-wide appearance
  // changes while this Settings page stays open. Other Space choices remain.
  for (const char* key : {
           themes::kPolicyThemeColor,
           prefs::kBrowserColorScheme,
           prefs::kUserColor,
           prefs::kGrayscaleThemeEnabled,
           prefs::kBrowserColorVariant,
           prefs::kCurrentThemeID,
           "noventra.browser.glass_enabled",
#if BUILDFLAG(IS_LINUX)
           prefs::kSystemTheme,
#endif
       }) {
    if (profile_->GetPrefs()->FindPreference(key)) {
      layout_registrar_.Add(
          key,
          base::BindRepeating(&AppearanceHandler::NotifyNoventraSpacesChanged,
                              base::Unretained(this)));
    }
  }
  extension_observation_.Observe(extensions::ExtensionRegistry::Get(profile_));
}
void AppearanceHandler::OnJavascriptDisallowed() {
  weak_ptr_factory_.InvalidateWeakPtrs();
  layout_registrar_.RemoveAll();
  extension_observation_.Reset();
}
void AppearanceHandler::OnExtensionLoaded(content::BrowserContext*, const extensions::Extension*) { NotifyNoventraLayoutState(); }
void AppearanceHandler::OnExtensionUnloaded(content::BrowserContext*, const extensions::Extension*, extensions::UnloadedExtensionReason) { NotifyNoventraLayoutState(); }
void AppearanceHandler::OnShutdown(extensions::ExtensionRegistry*) { extension_observation_.Reset(); }
void AppearanceHandler::NotifyNoventraLayoutState() {
  if (IsJavascriptAllowed()) FireWebUIListener("noventra-layout-changed", NoventraLayoutState());
}

void AppearanceHandler::NotifyNoventraSpacesChanged() {
  if (IsJavascriptAllowed()) {
    FireWebUIListener("noventra-spaces-changed");
  }
}

void AppearanceHandler::RegisterMessages() {
  web_ui()->RegisterMessageCallback("noventraGetLayout", base::BindRepeating(
      &AppearanceHandler::HandleNoventraGetLayout, base::Unretained(this)));
  web_ui()->RegisterMessageCallback("noventraApplyLayout", base::BindRepeating(
      &AppearanceHandler::HandleNoventraApplyLayout, base::Unretained(this)));
  web_ui()->RegisterMessageCallback("noventraSpaces",
      base::BindRepeating(&AppearanceHandler::HandleNoventraSpaces, base::Unretained(this)));
  web_ui()->RegisterMessageCallback(
      "noventraWorkspaceTools",
      base::BindRepeating(&AppearanceHandler::HandleNoventraWorkspaceTools,
                          base::Unretained(this)));
  web_ui()->RegisterMessageCallback(
      "noventraWorkspaceSync",
      base::BindRepeating(&AppearanceHandler::HandleNoventraWorkspaceSync,
                          base::Unretained(this)));
  web_ui()->RegisterMessageCallback(
      "noventraCustomization",
      base::BindRepeating(&AppearanceHandler::HandleNoventraCustomization,
                          base::Unretained(this)));
  web_ui()->RegisterMessageCallback(
      "useDefaultTheme",
      base::BindRepeating(&AppearanceHandler::HandleUseTheme,
                          base::Unretained(this), ui::SystemTheme::kDefault));
#if BUILDFLAG(IS_LINUX)
  web_ui()->RegisterMessageCallback(
      "useGtkTheme",
      base::BindRepeating(&AppearanceHandler::HandleUseTheme,
                          base::Unretained(this), ui::SystemTheme::kGtk));
  web_ui()->RegisterMessageCallback(
      "useQtTheme",
      base::BindRepeating(&AppearanceHandler::HandleUseTheme,
                          base::Unretained(this), ui::SystemTheme::kQt));
#endif

  web_ui()->RegisterMessageCallback(
      "openCustomizeChrome",
      base::BindRepeating(&AppearanceHandler::OpenCustomizeChrome,
                          base::Unretained(this)));
  web_ui()->RegisterMessageCallback(
      "openCustomizeChromeToolbarSection",
      base::BindRepeating(&AppearanceHandler::OpenCustomizeChromeToolbarSection,
                          base::Unretained(this)));
  web_ui()->RegisterMessageCallback(
      "resetPinnedToolbarActions",
      base::BindRepeating(&AppearanceHandler::ResetPinnedToolbarActions,
                          base::Unretained(this)));
  web_ui()->RegisterMessageCallback(
      "pinnedToolbarActionsAreDefault",
      base::BindRepeating(&AppearanceHandler::PinnedToolbarActionsAreDefault,
                          base::Unretained(this)));
  web_ui()->RegisterMessageCallback(
      "recordVerticalTabStripModeChanged",
      base::BindRepeating(
          &AppearanceHandler::HandleRecordVerticalTabStripModeChanged,
          base::Unretained(this)));
}

base::DictValue AppearanceHandler::NoventraLayoutState() {
  auto& prefs = *profile_->GetPrefs();
  const auto layout = noventra::ReadOrMigrateLayout(prefs);
  auto capabilities = noventra::CapabilitiesToValue(noventra::GetLayoutCapabilities(prefs));
  for (auto& value : capabilities) {
    auto& capability = value.GetDict();
    if (*capability.FindString("id") != "extensions") continue;
    base::ListValue members;
    for (const auto& extension : extensions::ExtensionRegistry::Get(profile_)->enabled_extensions())
      if (!extension->is_theme())
        members.Append(base::DictValue().Set("id", "extension:" + extension->id())
            .Set("label", extension->name()).Set("independent", false));
    capability.Set("members", std::move(members));
  }
  base::DictValue presets;
  for (const char* name : {"sidebar-top", "minimal-left", "minimal-right",
                           "classic-top", "compact-top"})
    presets.Set(name, noventra::LayoutToValue(noventra::MakeLayoutPreset(name, prefs)));
  return base::DictValue().Set("layout", noventra::LayoutToValue(layout))
      .Set("capabilities", std::move(capabilities)).Set("presets", std::move(presets));
}
void AppearanceHandler::HandleNoventraGetLayout(const base::ListValue& args) {
  if (args.size() != 1 || !args[0].is_string()) return;
  AllowJavascript();
  ResolveJavascriptCallback(args[0], base::Value(NoventraLayoutState()));
}

void AppearanceHandler::HandleNoventraApplyLayout(const base::ListValue& args) {
  if (args.empty() || !args[0].is_string()) return;
  AllowJavascript();
  auto& prefs = *profile_->GetPrefs();
  auto reject = [&](std::string message) {
    ResolveJavascriptCallback(args[0], base::Value(noventra::ApplyResult{
        "invalid", std::move(message), noventra::ReadOrMigrateLayout(prefs)}.ToValue()));
  };
  if (args.size() != 2 || !args[1].is_dict()) {
    reject("Invalid layout request."); return;
  }
  const auto& request = args[1].GetDict();
  auto revision = request.FindInt("expectedRevision");
  const auto* value = request.Find("layout");
  if (!revision || *revision < 0 || !value) {
    reject("Invalid layout revision."); return;
  }
  auto layout = noventra::ValidateLayout(*value, noventra::GetLayoutCapabilities(prefs));
  if (!layout.has_value()) { reject(layout.error()); return; }
  ResolveJavascriptCallback(args[0], base::Value(
      noventra::ApplyLayout(prefs, *layout, *revision).ToValue()));
}

void AppearanceHandler::HandleNoventraCustomization(const base::ListValue& args) {
  if (args.size() != 3 || !args[0].is_string() || !args[1].is_string() ||
      !args[2].is_int()) {
    return;
  }
  AllowJavascript();
  auto* browser = GlobalBrowserCollection::GetInstance()->FindBrowserWithTab(
      web_ui()->GetWebContents());
  auto* controller = browser
      ? tabs::VerticalTabStripStateController::From(browser) : nullptr;
  if (!controller) {
    RejectJavascriptCallback(args[0], base::Value("Browser window unavailable"));
    return;
  }
  const auto& action = args[1].GetString();
  auto* prefs = profile_->GetPrefs();
  if (action == "preset") {
    for (const char* key : {prefs::kHeliumLayout, prefs::kHeliumZenMode,
         "helium.browser.rounded_frame", "noventra.browser.address_on_demand",
         "noventra.browser.show_navigation_buttons", "noventra.browser.tab_letters",
         "noventra.browser.sidebar_controls"}) {
      if (prefs->IsManagedPreference(key)) {
        RejectJavascriptCallback(args[0], base::Value("Appearance is managed by your organization"));
        return;
      }
    }
    prefs->SetBoolean(prefs::kHeliumZenMode, false);
    prefs->SetBoolean("helium.browser.rounded_frame", false);
    prefs->SetBoolean("noventra.browser.address_on_demand", true);
    prefs->SetBoolean("noventra.browser.show_navigation_buttons", true);
    prefs->SetBoolean("noventra.browser.tab_letters", false);
    prefs->SetBoolean("noventra.browser.sidebar_controls", true);
    if (auto* layout = HeliumLayoutStateController::From(browser)) {
      layout->SetBrowserLayout(HeliumLayoutType::kVertical);
    }
    controller->SetUncollapsedWidth(232);
    controller->RequestCollapse(false);
  } else if (action == "width") {
    controller->SetUncollapsedWidth(std::clamp(args[2].GetInt(), 200, 440));
  } else if (action != "state") {
    RejectJavascriptCallback(args[0], base::Value("Unknown customization action"));
    return;
  }
  ResolveJavascriptCallback(args[0], base::Value(base::DictValue().Set(
      "width", controller->GetUncollapsedWidth())));
}

void AppearanceHandler::HandleUseTheme(ui::SystemTheme system_theme,
                                       const base::ListValue& args) {
  DCHECK(system_theme != ui::SystemTheme::kDefault || !profile_->IsChild());
  ThemeServiceFactory::GetForProfile(profile_)->UseTheme(system_theme);
}

void AppearanceHandler::OpenCustomizeChrome(const base::ListValue& args) {
  BrowserWindowInterface* browser =
      GlobalBrowserCollection::GetInstance()->GetLastActiveBrowser();
  if (!browser) {
    return;
  }
  chrome::ExecuteCommand(browser, IDC_SHOW_CUSTOMIZE_CHROME_SIDE_PANEL);
}

void AppearanceHandler::OpenCustomizeChromeToolbarSection(
    const base::ListValue& args) {
  BrowserWindowInterface* browser =
      GlobalBrowserCollection::GetInstance()->GetLastActiveBrowser();
  CHECK(browser);
  chrome::ExecuteCommand(browser, IDC_SHOW_CUSTOMIZE_CHROME_TOOLBAR);
}

void AppearanceHandler::ResetPinnedToolbarActions(const base::ListValue& args) {
  PinnedToolbarActionsModel::Get(profile_)->ResetToDefault();
}

void AppearanceHandler::PinnedToolbarActionsAreDefault(
    const base::ListValue& args) {
  CHECK_EQ(1U, args.size());
  const base::Value& callback_id = args[0];
  const bool are_default =
      PinnedToolbarActionsModel::Get(profile_)->IsDefault();

  AllowJavascript();
  ResolveJavascriptCallback(callback_id, base::Value(are_default));
}

void AppearanceHandler::HandleRecordVerticalTabStripModeChanged(
    const base::ListValue& args) {
  CHECK_EQ(1U, args.size());
  const bool is_vertical = args[0].GetBool();
  tabs::RecordVerticalTabStripModeChanged(
      is_vertical, tabs::VerticalTabStripEntryPoint::kSettings);
}

}  // namespace settings

namespace settings {
void AppearanceHandler::HandleNoventraSpaces(const base::ListValue& args) {
  if (args.empty() || !args[0].is_string()) {
    return;
  }
  AllowJavascript();
  if (args.size() != 4 || !args[1].is_string() || !args[2].is_string() ||
      !args[3].is_string()) {
    RejectJavascriptCallback(args[0], base::Value("Invalid space request"));
    return;
  }
  auto* browser = GlobalBrowserCollection::GetInstance()->FindBrowserWithTab(web_ui()->GetWebContents());
  if (!browser) { RejectJavascriptCallback(args[0], base::Value("Browser window unavailable")); return; }
  auto* prefs = profile_->GetPrefs();
  const std::string action = args[1].GetString();
  std::string id = args[2].GetString();
  const std::string& name_or_target = args[3].GetString();
  if (action == "create" || action == "rename") {
    if (action == "create") {
      id = base::Uuid::GenerateRandomV4().AsLowercaseString();
    }
    if (!noventra::SetSpaceName(browser, id, name_or_target,
                                action == "create")) {
      RejectJavascriptCallback(
          args[0], base::Value("Enter a name of 1–160 bytes for an existing "
                               "space, or create a new one. Managed spaces "
                               "cannot be changed."));
      return;
    }
  } else if (action == "move-before" || action == "move-after") {
    if (!noventra::MoveSpace(browser, id, name_or_target,
                             action == "move-after")) {
      RejectJavascriptCallback(
          args[0], base::Value("Choose two different existing spaces. Managed "
                               "spaces cannot be reordered."));
      return;
    }
  } else if (action == "archive" || action == "restore") {
    if (!noventra::SetSpaceArchived(browser, id, action == "archive")) {
      RejectJavascriptCallback(
          args[0],
          base::Value("Choose an existing account space. Personal and "
                      "managed spaces cannot be archived or restored."));
      return;
    }
  } else if (action == "appearance") {
    const auto appearance = base::JSONReader::ReadDict(
        name_or_target, base::JSON_PARSE_RFC);
    const auto* color = appearance ? appearance->FindString("color") : nullptr;
    const auto* icon = appearance ? appearance->FindString("icon") : nullptr;
    const auto* theme = appearance ? appearance->FindString("theme") : nullptr;
    // Merge a single changed field with the current native value. Another
    // Settings window may have changed the other fields since this page read
    // it.
    const auto current = noventra::GetSpaceAppearance(profile_, id);
    if (!appearance || appearance->size() != 1 || (!color && !icon && !theme) ||
        (theme && !noventra::CanCustomizeSpaceTheme(browser)) ||
        !noventra::SetSpaceAppearance(
            browser, id, color ? *color : current.color,
            icon ? *icon : current.icon, theme ? *theme : current.theme)) {
      RejectJavascriptCallback(
          args[0], base::Value("Choose a supported color, icon or theme for an "
                               "existing space. Managed appearance cannot be "
                               "changed."));
      return;
    }
  } else if (action == "switch") {
    const auto visible = noventra::SpaceIds(browser);
    if (std::ranges::find(visible, id) == visible.end()) {
      RejectJavascriptCallback(
          args[0], base::Value("This space is unavailable. Restore an archived "
                               "space before switching to it."));
      return;
    }
  } else if (action != "list") {
    RejectJavascriptCallback(args[0], base::Value("Unknown space action")); return;
  }
  base::ListValue spaces;
  for (const auto& space_id : noventra::SpaceIds(browser, true)) {
    const auto* label = prefs->GetDict(noventra::kSpacesPref).FindString(space_id);
    const auto appearance = noventra::GetSpaceAppearance(profile_, space_id);
    spaces.Append(
        base::DictValue()
            .Set("id", space_id)
            .Set("name", label              ? *label
                         : space_id.empty() ? "Personal"
                                            : "Account space")
            .Set("archived", noventra::IsSpaceArchived(profile_, space_id))
            .Set("color", appearance.color)
            .Set("icon", appearance.icon)
            .Set("theme", appearance.theme)
            .Set("appearanceEditable", noventra::CanManageSpaces(profile_) &&
                                           !prefs->IsManagedPreference(
                                               noventra::kSpaceAppearancePref))
            .Set("themeEditable", noventra::CanCustomizeSpaceTheme(browser)));
  }
  ResolveJavascriptCallback(args[0], spaces);
  if (action == "switch") noventra::SwitchSpace(browser, id);
}
}  // namespace settings

namespace settings {
void AppearanceHandler::HandleNoventraWorkspaceSync(
    const base::ListValue& args) {
  if (args.empty() || !args[0].is_string()) {
    return;
  }
  AllowJavascript();
  if (args.size() != 3 || !args[1].is_string() || !args[2].is_dict()) {
    RejectJavascriptCallback(args[0],
                             base::Value("Invalid workspace sync request."));
    return;
  }
  auto* service = noventra::WorkspaceSyncService::GetForProfile(profile_);
  if (!service) {
    RejectJavascriptCallback(
        args[0],
        base::Value("Workspace sync is available in regular windows."));
    return;
  }
  if (args[1].GetString() == "status") {
    ResolveJavascriptCallback(args[0], service->Status());
    return;
  }
  auto* browser = GlobalBrowserCollection::GetInstance()->FindBrowserWithTab(
      web_ui()->GetWebContents());
  if (!browser) {
    RejectJavascriptCallback(args[0],
                             base::Value("Browser window unavailable."));
    return;
  }
  service->Handle(
      browser, args[1].GetString(), args[2].GetDict(),
      base::BindOnce(
          [](base::WeakPtr<AppearanceHandler> handler, base::Value callback_id,
             base::expected<base::DictValue, std::string> result) {
            if (!handler || !handler->IsJavascriptAllowed()) {
              return;
            }
            if (result.has_value()) {
              handler->ResolveJavascriptCallback(callback_id, result.value());
            } else {
              handler->RejectJavascriptCallback(callback_id,
                                                base::Value(result.error()));
            }
          },
          weak_ptr_factory_.GetWeakPtr(), args[0].Clone()));
}

void AppearanceHandler::HandleNoventraWorkspaceTools(
    const base::ListValue& args) {
  if (args.empty() || !args[0].is_string()) {
    return;
  }
  AllowJavascript();
  const auto reject = [&](std::string error) {
    RejectJavascriptCallback(args[0], base::Value(std::move(error)));
  };
  if (args.size() != 3 || !args[1].is_string() || !args[2].is_dict()) {
    reject("Invalid workspace settings request.");
    return;
  }
  auto* browser = GlobalBrowserCollection::GetInstance()->FindBrowserWithTab(
      web_ui()->GetWebContents());
  if (!browser) {
    reject("Browser window unavailable.");
    return;
  }
  auto& prefs = *profile_->GetPrefs();
  const auto& action = args[1].GetString();
  const auto& request = args[2].GetDict();
  const auto routes_value = [&]() {
    base::ListValue routes;
    for (const auto& route : noventra::GetWorkspaceRoutes(profile_)) {
      routes.Append(base::DictValue()
                        .Set("host", route.host)
                        .Set("space_id", route.space_id));
    }
    return routes;
  };
  if (action == "list") {
    ResolveJavascriptCallback(
        args[0],
        base::DictValue()
            .Set("routes", routes_value())
            .Set("manageable", noventra::CanManageWorkspaceRoutes(profile_))
            .Set("importable",
                 !profile_->IsOffTheRecord() &&
                     noventra::CanManageSpaces(profile_) &&
                     noventra::CanManageWorkspaceRoutes(profile_)));
    return;
  }
  if (profile_->IsOffTheRecord()) {
    reject("Manage workspace settings in a regular window.");
    return;
  }
  if (action == "set-route" || action == "remove-route") {
    const auto* host = request.FindString("host");
    const auto* id = request.FindString("space_id");
    if (!host || (action == "set-route" && !id)) {
      reject("Choose a hostname and an available space.");
      return;
    }
    auto outcome = action == "set-route"
                       ? noventra::SetWorkspaceRoute(browser, *host, *id)
                       : noventra::RemoveWorkspaceRoute(profile_, *host);
    if (!outcome.has_value()) {
      reject(outcome.error());
      return;
    }
    ResolveJavascriptCallback(args[0], routes_value());
    return;
  }
  if (action == "export") {
    base::ListValue spaces;
    const auto ids = noventra::SpaceIds(browser, true);
    for (const auto& id : ids) {
      const auto* name = prefs.GetDict(noventra::kSpacesPref).FindString(id);
      spaces.Append(
          base::DictValue()
              .Set("id", id)
              .Set("name", name         ? *name
                           : id.empty() ? "Personal"
                                        : "Account space")
              .Set("archived", noventra::IsSpaceArchived(profile_, id)));
    }
    base::ListValue routes;
    for (const auto& route : noventra::GetWorkspaceRoutes(profile_)) {
      if (std::ranges::find(ids, route.space_id) != ids.end()) {
        routes.Append(base::DictValue()
                          .Set("host", route.host)
                          .Set("space_id", route.space_id));
      }
    }
    ResolveJavascriptCallback(
        args[0], base::DictValue()
                     .Set("format", "noventra-workspace-settings")
                     .Set("version", 1)
                     .Set("spaces", std::move(spaces))
                     .Set("routes", std::move(routes))
                     .Set("layout", noventra::LayoutToValue(
                                        noventra::ReadOrMigrateLayout(prefs))));
    return;
  }
  if (action != "import") {
    reject("Unknown workspace settings action.");
    return;
  }
  if (!noventra::CanManageSpaces(profile_) ||
      !noventra::CanManageWorkspaceRoutes(profile_)) {
    reject("Your organization manages workspace settings.");
    return;
  }
  const auto* text = request.FindString("text");
  const auto apply_layout = request.FindBool("applyLayout");
  if (!text || text->size() > 2 * 1024 * 1024 || !apply_layout) {
    reject("Choose a workspace settings file smaller than 2 MB.");
    return;
  }
  auto document = base::JSONReader::ReadDict(*text, base::JSON_PARSE_RFC, 32);
  if (!document || !document->FindString("format") ||
      *document->FindString("format") != "noventra-workspace-settings" ||
      document->FindInt("version") != 1) {
    reject("This is not a supported Noventra workspace settings file.");
    return;
  }
  const auto* spaces = document->FindList("spaces");
  const auto* routes = document->FindList("routes");
  if (!spaces || spaces->empty() || spaces->size() > 1000 || !routes ||
      routes->size() > noventra::kMaxWorkspaceRoutes) {
    reject("The file must contain at most 1,000 spaces and 100 domain rules.");
    return;
  }
  struct ImportedSpace {
    std::string id;
    std::string name;
    bool archived;
  };
  std::vector<ImportedSpace> imported;
  std::map<std::string, std::string> id_map;
  for (const auto& value : *spaces) {
    const auto* row = value.GetIfDict();
    const auto* id = row ? row->FindString("id") : nullptr;
    const auto* name = row ? row->FindString("name") : nullptr;
    const auto archived = row ? row->FindBool("archived") : std::nullopt;
    std::string trimmed;
    if (name) {
      base::TrimWhitespaceASCII(*name, base::TRIM_ALL, &trimmed);
    }
    if (!id || (!id->empty() && !base::Uuid::ParseLowercase(*id).is_valid()) ||
        id_map.contains(*id) || trimmed.empty() || trimmed.size() > 160 ||
        !base::IsStringUTF8(trimmed) || !archived ||
        (id->empty() && *archived)) {
      reject("The file contains an invalid or duplicate workspace.");
      return;
    }
    const std::string new_id =
        id->empty() ? "" : base::Uuid::GenerateRandomV4().AsLowercaseString();
    id_map.emplace(*id, new_id);
    if (!id->empty()) {
      imported.push_back({new_id, trimmed, *archived});
    }
  }
  if (!id_map.contains("")) {
    reject("The workspace file must include Personal.");
    return;
  }
  // Validate every record before changing any preference. Existing host rules
  // take precedence; imported accounts always receive fresh storage identities.
  std::set<std::string> imported_hosts;
  std::vector<noventra::WorkspaceRoute> new_routes;
  for (const auto& value : *routes) {
    const auto* row = value.GetIfDict();
    const auto* host = row ? row->FindString("host") : nullptr;
    const auto* id = row ? row->FindString("space_id") : nullptr;
    if (!host || !id || !id_map.contains(*id)) {
      reject("A domain rule refers to an invalid workspace.");
      return;
    }
    auto canonical = noventra::CanonicalWorkspaceRouteHost(*host);
    if (!canonical.has_value() || !imported_hosts.insert(*canonical).second) {
      reject("The file contains an invalid or duplicate hostname.");
      return;
    }
    new_routes.push_back({*canonical, id_map.at(*id)});
  }
  auto merged_routes = routes_value();
  std::set<std::string> existing_hosts;
  for (const auto& value : merged_routes) {
    existing_hosts.insert(*value.GetDict().FindString("host"));
  }
  size_t added_routes = 0;
  for (const auto& route : new_routes) {
    if (existing_hosts.insert(route.host).second) {
      merged_routes.Append(base::DictValue()
                               .Set("host", route.host)
                               .Set("space_id", route.space_id));
      ++added_routes;
    }
  }
  if (merged_routes.size() > noventra::kMaxWorkspaceRoutes) {
    reject("Import would exceed 100 domain rules. Remove some rules first.");
    return;
  }
  std::optional<noventra::BrowserLayout> layout;
  if (*apply_layout) {
    const auto* raw = document->Find("layout");
    const auto revision = request.FindInt("expectedRevision");
    if (!raw || !revision || *revision < 0) {
      reject("Reload Settings before importing the layout.");
      return;
    }
    auto parsed =
        noventra::ValidateLayout(*raw, noventra::GetLayoutCapabilities(prefs));
    if (!parsed.has_value()) {
      reject("Cannot import layout: " + parsed.error() +
             " You can import spaces without the layout.");
      return;
    }
    parsed->revision = *revision;
    layout = std::move(*parsed);
  }
  // No existing space is renamed, archived, selected or reloaded by import.
  // Layout application can reject stale revisions/policy before metadata
  // writes.
  if (layout) {
    const auto applied =
        noventra::ApplyLayout(prefs, *layout, layout->revision);
    if (applied.status != "ok") {
      reject(applied.message);
      return;
    }
  }
  noventra::SpaceIds(browser, true);  // Seed existing order before new names.
  auto names = prefs.GetDict(noventra::kSpacesPref).Clone();
  auto order = prefs.GetList(noventra::kSpaceOrderPref).Clone();
  auto archived = prefs.GetList(noventra::kArchivedSpacesPref).Clone();
  for (const auto& space : imported) {
    names.Set(space.id, space.name);
    order.Append(space.id);
    if (space.archived) {
      archived.Append(space.id);
    }
  }
  prefs.SetList(noventra::kSpaceOrderPref, std::move(order));
  prefs.SetList(noventra::kArchivedSpacesPref, std::move(archived));
  prefs.SetDict(noventra::kSpacesPref, std::move(names));
  prefs.SetList(noventra::kWorkspaceRoutesPref, std::move(merged_routes));
  ResolveJavascriptCallback(
      args[0],
      base::DictValue()
          .Set("spaces", static_cast<int>(imported.size()))
          .Set("routes", static_cast<int>(added_routes))
          .Set("keptRules", static_cast<int>(new_routes.size() - added_routes))
          .Set("layoutApplied", *apply_layout));
}
}  // namespace settings
