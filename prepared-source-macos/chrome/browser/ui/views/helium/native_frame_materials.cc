// Copyright 2026 The Helium Authors
// You can use, redistribute, and/or modify this source code under
// the terms of the GPL-3.0 license that can be found in the LICENSE file.

#include "chrome/browser/ui/views/helium/native_frame_materials.h"

#include "chrome/browser/performance_manager/public/user_tuning/battery_saver_mode_manager.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/themes/theme_service.h"
#include "chrome/browser/themes/theme_service_factory.h"
#include "chrome/browser/ui/browser_window/public/browser_window_interface.h"
#include "chrome/browser/ui/color/chrome_color_id.h"
#include "chrome/browser/ui/noventra_commands.h"
#include "chrome/browser/ui/views/frame/browser_view.h"
#include "components/prefs/pref_service.h"
#include "ui/color/color_provider.h"
#include "ui/native_theme/native_theme.h"
#include "ui/views/view.h"
#include "ui/views/widget/widget.h"

namespace helium {

bool WantsNoventraGlass(const BrowserView* browser_view) {
  // Color-provider creation can precede attachment to the widget tree.
  if (!browser_view || !browser_view->GetIsNormalType() ||
      !browser_view->GetWidget()) {
    return false;
  }
  Profile* profile = browser_view->GetProfile();
  if (!profile || profile->ShutdownStarted()) {
    return false;
  }
  // Catalog lookup takes a mutable window identity; this query only reads its
  // selected Space and appearance.
  const auto* space_theme = noventra::GetSpaceTheme(
      const_cast<BrowserWindowInterface*>(browser_view->browser()));
  if (!(space_theme
            ? space_theme->glass
            : profile->GetPrefs()->GetBoolean(kNoventraGlassEnabledPref))) {
    return false;
  }
  const auto* theme_service = ThemeServiceFactory::GetForProfile(profile);
  if (!space_theme && theme_service && theme_service->UsingExtensionTheme()) {
    return false;
  }
  const auto* native_theme = browser_view->GetWidget()->GetNativeTheme();
  if (native_theme->prefers_reduced_transparency() ||
      native_theme->forced_colors() !=
          ui::ColorProviderKey::ForcedColors::kNone ||
      native_theme->preferred_contrast() ==
          ui::NativeTheme::PreferredContrast::kMore) {
    return false;
  }
  using BatterySaver =
      performance_manager::user_tuning::BatterySaverModeManager;
  return !BatterySaver::HasInstance() ||
         !BatterySaver::GetInstance()->IsBatterySaverActive();
}

bool ShouldUseNativeFrameMaterials(const BrowserView* browser_view) {
  return WantsNoventraGlass(browser_view) &&
         browser_view->noventra_desktop_glass_available();
}

SkColor GetNoventraGlassTint(const views::View* view) {
  const bool dark = view->GetWidget()->GetColorMode() ==
                    ui::ColorProviderKey::ColorMode::kDark;
  // Tint the material, never the foreground controls or WebContents. Theme
  // colors stay opaque elsewhere, including menus, pages and loading backings.
  return SkColorSetA(view->GetColorProvider()->GetColor(kColorToolbar),
                     dark ? 176 : 184);
}

bool ShouldUseNativeFrameMaterials(const BrowserWindowInterface* browser) {
  return ShouldUseNativeFrameMaterials(
      browser ? BrowserView::GetBrowserViewForBrowser(browser) : nullptr);
}

bool ShouldUseNativeFrameMaterials(const views::View* view) {
  const views::Widget* widget = view ? view->GetWidget() : nullptr;
  return ShouldUseNativeFrameMaterials(
      widget ? BrowserView::GetBrowserViewForNativeWindow(
                   widget->GetNativeWindow())
             : nullptr);
}

}  // namespace helium
