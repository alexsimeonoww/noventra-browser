// Copyright 2026 The Helium Authors
// You can use, redistribute, and/or modify this source code under
// the terms of the GPL-3.0 license that can be found in the LICENSE file.

#ifndef CHROME_BROWSER_UI_VIEWS_HELIUM_NATIVE_FRAME_MATERIALS_H_
#define CHROME_BROWSER_UI_VIEWS_HELIUM_NATIVE_FRAME_MATERIALS_H_

#include "third_party/skia/include/core/SkColor.h"

class BrowserView;
class BrowserWindowInterface;

namespace views {
class View;
}  // namespace views

namespace helium {

inline constexpr char kNoventraGlassEnabledPref[] =
    "noventra.browser.glass_enabled";

// Requested appearance and accessibility/theme eligibility, independent of
// whether this window's native backend can currently supply a backdrop.
bool WantsNoventraGlass(const BrowserView* browser_view);
SkColor GetNoventraGlassTint(const views::View* view);

bool ShouldUseNativeFrameMaterials(const BrowserView* browser_view);
bool ShouldUseNativeFrameMaterials(const BrowserWindowInterface* browser);
bool ShouldUseNativeFrameMaterials(const views::View* view);

}  // namespace helium

#endif  // CHROME_BROWSER_UI_VIEWS_HELIUM_NATIVE_FRAME_MATERIALS_H_
