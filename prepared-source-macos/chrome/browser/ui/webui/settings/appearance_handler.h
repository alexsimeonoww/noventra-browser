// Copyright 2015 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_UI_WEBUI_SETTINGS_APPEARANCE_HANDLER_H_
#define CHROME_BROWSER_UI_WEBUI_SETTINGS_APPEARANCE_HANDLER_H_

#include "base/memory/raw_ptr.h"
#include "base/scoped_observation.h"
#include "components/prefs/pref_change_registrar.h"
#include "extensions/browser/extension_registry.h"
#include "extensions/browser/extension_registry_observer.h"
#include "base/memory/weak_ptr.h"
#include "build/build_config.h"
#include "chrome/browser/themes/theme_service.h"
#include "chrome/browser/ui/webui/settings/settings_page_ui_handler.h"

namespace content {
class WebUI;
}

class Profile;

namespace settings {

// Chrome "Appearance" settings page UI handler.
class AppearanceHandler : public SettingsPageUIHandler, public extensions::ExtensionRegistryObserver {
 public:
  explicit AppearanceHandler(content::WebUI* webui);

  AppearanceHandler(const AppearanceHandler&) = delete;
  AppearanceHandler& operator=(const AppearanceHandler&) = delete;

  ~AppearanceHandler() override;

  // SettingsPageUIHandler implementation.
  void RegisterMessages() override;
  void OnJavascriptAllowed() override;
  void OnJavascriptDisallowed() override;

 private:
  base::DictValue NoventraLayoutState();
  void NotifyNoventraLayoutState();
  void NotifyNoventraSpacesChanged();
  void OnExtensionLoaded(content::BrowserContext*, const extensions::Extension*) override;
  void OnExtensionUnloaded(content::BrowserContext*, const extensions::Extension*, extensions::UnloadedExtensionReason) override;
  void OnShutdown(extensions::ExtensionRegistry*) override;
  PrefChangeRegistrar layout_registrar_;
  PrefChangeRegistrar spaces_registrar_;
  base::ScopedObservation<extensions::ExtensionRegistry, extensions::ExtensionRegistryObserver> extension_observation_{this};
  void HandleNoventraGetLayout(const base::ListValue& args);
  void HandleNoventraApplyLayout(const base::ListValue& args);
  void HandleNoventraSpaces(const base::ListValue& args);
  void HandleNoventraWorkspaceTools(const base::ListValue& args);
  void HandleNoventraWorkspaceSync(const base::ListValue& args);
  void HandleNoventraCustomization(const base::ListValue& args);
  // Changes the UI theme to the specified `system_theme`.
  void HandleUseTheme(ui::SystemTheme system_theme,
                      const base::ListValue& args);
  // Opens the Customize Chrome side panel.
  void OpenCustomizeChrome(const base::ListValue& args);
  // Opens the Customize Chrome side panel to the toolbar section.
  void OpenCustomizeChromeToolbarSection(const base::ListValue& args);
  // Reset toolbar pinning to the default settings.
  void ResetPinnedToolbarActions(const base::ListValue& args);
  // Whether toolbar pinning is in its default state or not.
  void PinnedToolbarActionsAreDefault(const base::ListValue& args);
  // Records the vertical tab strip mode change.
  void HandleRecordVerticalTabStripModeChanged(const base::ListValue& args);

  raw_ptr<Profile> profile_;  // Weak pointer.

  base::WeakPtrFactory<AppearanceHandler> weak_ptr_factory_{this};
};

}  // namespace settings

#endif  // CHROME_BROWSER_UI_WEBUI_SETTINGS_APPEARANCE_HANDLER_H_
