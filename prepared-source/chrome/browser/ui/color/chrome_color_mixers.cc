// Copyright 2021 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/ui/color/chrome_color_mixers.h"

#include <memory>
#include <string_view>

#include "base/containers/fixed_flat_map.h"
#include "base/functional/bind.h"
#include "base/no_destructor.h"
#include "chrome/browser/ui/color/chrome_color_id.h"
#include "chrome/browser/ui/color/chrome_color_mixer.h"
#include "chrome/browser/ui/color/chrome_color_provider_utils.h"
#include "chrome/browser/ui/color/material_chrome_color_mixer.h"
#include "chrome/browser/ui/color/material_new_tab_page_color_mixer.h"
#include "chrome/browser/ui/color/material_omnibox_color_mixer.h"
#include "chrome/browser/ui/color/material_side_panel_color_mixer.h"
#include "chrome/browser/ui/color/material_tab_strip_color_mixer.h"
#include "chrome/browser/ui/color/native_chrome_color_mixer.h"
#include "chrome/browser/ui/color/new_tab_page_color_mixer.h"
#include "chrome/browser/ui/color/omnibox_color_mixer.h"
#include "chrome/browser/ui/color/product_specifications_color_mixer.h"
#include "chrome/browser/ui/color/tab_strip_color_mixer.h"
#include "ui/color/color_provider_utils.h"
#include "ui/color/color_provider.h"
#include "ui/color/color_mixer.h"
#include "ui/color/color_recipe.h"
#include "ui/color/color_id.h"
#include "ui/color/color_transform.h"
#include "third_party/skia/include/core/SkColor.h"

namespace {

// Noventra's default frame is neutral. Explicit themes, user colors, apps and
// high-contrast palettes retain their own color providers.
void AddNoventraNeutralFrame(ui::ColorProvider* provider,
                             const ui::ColorProviderKey& key) {
  // The native key may carry an OS accent even for the baseline/grayscale
  // theme. Only an accent source represents a chosen custom/device color.
  const bool custom_color = key.user_color.has_value() &&
      key.user_color_source == ui::ColorProviderKey::UserColorSource::kAccent;
  if (key.custom_theme || key.app_controller ||
      key.contrast_mode == ui::ColorProviderKey::ContrastMode::kHigh ||
      key.forced_colors != ui::ColorProviderKey::ForcedColors::kNone) {
    return;
  }
  const bool dark = key.color_mode == ui::ColorProviderKey::ColorMode::kDark;
  if (custom_color) {
    ui::ColorMixer& mixer = provider->AddMixer();
    for (const auto id : {kColorTabBackgroundActiveFrameActive,
                          kColorTabBackgroundActiveFrameInactive,
                          kColorTabBackgroundSelectedFrameActive,
                          kColorTabBackgroundSelectedFrameInactive}) {
      mixer[id] = ui::BlendTowardMaxContrast({kColorToolbar}, dark ? 40 : 24);
    }
    for (const auto id : {kColorTabForegroundActiveFrameActive,
                          kColorTabForegroundActiveFrameInactive,
                          kColorTabForegroundInactiveFrameActive,
                          kColorTabForegroundInactiveFrameInactive,
                          kColorNewTabButtonCRForegroundFrameActive,
                          kColorNewTabButtonCRForegroundFrameInactive}) {
      mixer[id] = {kColorToolbarText};
    }
    return;
  }
  const SkColor ground = dark ? SkColorSetRGB(28, 28, 28) : SK_ColorWHITE;
  const SkColor ink = dark ? SkColorSetRGB(237, 237, 237)
                           : SkColorSetRGB(23, 23, 23);
  // A variable desktop backdrop needs stronger foreground contrast than the
  // solid frame. Keep foreground pixels opaque; only the material is tinted.
  const bool glass =
      key.frame_style == ui::ColorProviderKey::FrameStyle::kGlass;
  const SkColor muted = glass ? ink
                              : (dark ? SkColorSetRGB(166, 166, 166)
                                      : SkColorSetRGB(105, 105, 105));
  const SkColor pill = dark ? SkColorSetRGB(54, 54, 54)
                            : SkColorSetRGB(234, 234, 234);
  const SkColor hover = dark ? SkColorSetRGB(38, 38, 38)
                             : SkColorSetRGB(246, 246, 246);
  ui::ColorMixer& mixer = provider->AddMixer();
  mixer[ui::kColorFrameActive] = {ground};
  mixer[ui::kColorFrameInactive] = {ground};
  mixer[kColorToolbar] = {ground};
  mixer[kColorBookmarkBarBackground] = {ground};
  mixer[kColorSidePanelBackground] = {ground};
  mixer[kColorToolbarTextDefault] = {ink};
  mixer[kColorToolbarText] = {ink};
  mixer[kColorToolbarButtonIconDefault] = {muted};
  mixer[kColorLocationBarBackground] = {pill};
  mixer[kColorLocationBarBackgroundHovered] = {hover};
  mixer[kColorTabBackgroundActiveFrameActive] = {pill};
  mixer[kColorTabBackgroundActiveFrameInactive] = {pill};
  mixer[kColorTabBackgroundInactiveFrameActive] = {SK_ColorTRANSPARENT};
  mixer[kColorTabBackgroundInactiveFrameInactive] = {SK_ColorTRANSPARENT};
  mixer[kColorTabBackgroundInactiveHoverFrameActive] = {hover};
  mixer[kColorTabBackgroundInactiveHoverFrameInactive] = {hover};
  mixer[kColorTabBackgroundSelectedFrameActive] = {pill};
  mixer[kColorTabBackgroundSelectedFrameInactive] = {pill};
  mixer[kColorTabForegroundActiveFrameActive] = {ink};
  mixer[kColorTabForegroundActiveFrameInactive] = {ink};
  mixer[kColorTabForegroundInactiveFrameActive] = {muted};
  mixer[kColorTabForegroundInactiveFrameInactive] = {muted};
  mixer[kColorTabStrokeFrameActive] = {SK_ColorTRANSPARENT};
  mixer[kColorTabStrokeFrameInactive] = {SK_ColorTRANSPARENT};
  mixer[kColorNewTabButtonCRBackgroundFrameActive] = {ground};
  mixer[kColorNewTabButtonCRBackgroundFrameInactive] = {ground};
}

void AddNoventraPopupColors(ui::ColorProvider* provider,
                           const ui::ColorProviderKey& key) {
  using ThemeType = ui::ColorProviderKey::ThemeInitializerSupplier::ThemeType;
  if (key.app_controller ||
      (key.custom_theme &&
       key.custom_theme->get_theme_type() != ThemeType::kAutogenerated) ||
      key.contrast_mode == ui::ColorProviderKey::ContrastMode::kHigh ||
      key.forced_colors != ui::ColorProviderKey::ForcedColors::kNone) {
    return;
  }
  // Browser-owned surfaces share the frame's chosen color, with opaque
  // backgrounds for readable menus over both solid and glass chrome. Extension
  // and native themes, app palettes and accessibility colors keep their rules.
  ui::ColorMixer& mixer = provider->AddMixer();
  mixer[ui::kColorBubbleBackground] = {kColorToolbar};
  mixer[ui::kColorDialogBackground] = {kColorToolbar};
  mixer[ui::kColorMenuBackground] = {kColorToolbar};
  mixer[ui::kColorBubbleFooterBackground] = {kColorToolbar};
  mixer[ui::kColorBubbleBorder] =
      ui::AlphaBlend(kColorToolbarTextDefault, kColorToolbar, 0x24);
  mixer[ui::kColorMenuBorder] = {ui::kColorBubbleBorder};
  mixer[ui::kColorBubbleFooterBorder] = {ui::kColorBubbleBorder};
  mixer[ui::kColorDialogForeground] = {kColorToolbarTextDefault};
  mixer[ui::kColorMenuItemForeground] = {kColorToolbarTextDefault};
  mixer[ui::kColorMenuIcon] = {kColorToolbarButtonIconDefault};
  mixer[ui::kColorMenuItemBackgroundHighlighted] = {
      kColorToolbarBackgroundSubtleEmphasis};
  mixer[ui::kColorMenuItemBackgroundSelected] = {
      kColorToolbarBackgroundSubtleEmphasis};
}

class ChromeColorProviderUtilsCallbacks
    : public ui::ColorProviderUtilsCallbacks {
 public:
  bool ColorIdName(ui::ColorId color_id, std::string_view* color_name) override;
  bool ColorIdToCSSColorId(ui::ColorId color_id,
                           std::string_view* css_name) override;
};

#include "ui/color/color_id_map_macros.inc"

bool ChromeColorProviderUtilsCallbacks::ColorIdName(
    ui::ColorId color_id,
    std::string_view* color_name) {
  static constexpr const auto chrome_color_id_map =
      base::MakeFixedFlatMap<ui::ColorId, const char*>({CHROME_COLOR_IDS});
  auto i = chrome_color_id_map.find(color_id);
  if (i != chrome_color_id_map.cend()) {
    *color_name = i->second;
    return true;
  }
  return false;
}

bool ChromeColorProviderUtilsCallbacks::ColorIdToCSSColorId(
    ui::ColorId color_id,
    std::string_view* css_name) {
  std::string_view name = ChromeColorIdToCSSColorId(color_id);
  if (!name.empty()) {
    *css_name = name;
    return true;
  }
  return false;
}

// Note that this second include is not redundant. The second inclusion of the
// .inc file serves to undefine the macros the first inclusion defined.
#include "ui/color/color_id_map_macros.inc"

}  // namespace

void AddChromeColorMixers(ui::ColorProvider* provider,
                          const ui::ColorProviderKey& key) {
  static base::NoDestructor<ChromeColorProviderUtilsCallbacks>
      chrome_color_provider_utils_callbacks;
  ui::SetColorProviderUtilsCallbacks(
      chrome_color_provider_utils_callbacks.get());
  AddChromeColorMixer(provider, key);
  AddNewTabPageColorMixer(provider, key);
  AddOmniboxColorMixer(provider, key);
  AddProductSpecificationsColorMixer(provider, key);
  AddTabStripColorMixer(provider, key);

  AddMaterialChromeColorMixer(provider, key);
  AddMaterialNewTabPageColorMixer(provider, key);
  AddMaterialOmniboxColorMixer(provider, key);
  AddMaterialSidePanelColorMixer(provider, key);
  AddMaterialTabStripColorMixer(provider, key);

  // Must be the last one in order to override other mixer colors.
  AddNativeChromeColorMixer(provider, key);
  AddNoventraNeutralFrame(provider, key);
  AddNoventraPopupColors(provider, key);

  if (key.custom_theme) {
    key.custom_theme->AddColorMixers(provider, key);
  }

  if (key.app_controller) {
    key.app_controller->AddColorMixers(provider, key);
  }
}
