// Copyright 2026 The Helium Authors
// You can use, redistribute, and/or modify this source code under
// the terms of the GPL-3.0 license that can be found in the LICENSE file.

#import "chrome/browser/ui/views/helium/native_frame_materials_mac.h"

#include "base/apple/foundation_util.h"

namespace helium {
namespace {

// Low-alpha black or white overlay applied before the browser theme tint.
constexpr CGFloat kFrameMaterialNeutralOverlayAlpha = 0.06;

// Menu material is slightly brighter and more translucent in light mode,
// so we use it for the light theme instead of Sidebar.
NSVisualEffectMaterial GetFrameMaterial(bool is_dark) {
  return is_dark ? NSVisualEffectMaterialSidebar
                 : NSVisualEffectMaterialMenu;
}

// Keep the desktop visible through the material while retaining the browser's
// theme color (including incognito). These restrained tints need visual tuning
// on macOS; they are not a substitute for AppKit's accessibility fallback.
CGFloat GetFrameMaterialThemeTintOverlayAlpha(bool is_dark) {
  return is_dark ? 0.24 : 0.18;
}

NSColor* GetFrameMaterialNeutralOverlayColor(bool is_dark) {
  const CGFloat component = is_dark ? 0.0 : 1.0;
  return [NSColor colorWithSRGBRed:component
                             green:component
                              blue:component
                             alpha:kFrameMaterialNeutralOverlayAlpha];
}

// Applies the browser theme color over the black/white neutral overlay while
// keeping enough translucency for AppKit's material effect to show through.
NSColor* GetFrameMaterialThemeTintOverlayColor(SkColor theme_color,
                                               bool is_dark) {
  const CGFloat red = SkColorGetR(theme_color) / 255.0;
  const CGFloat green = SkColorGetG(theme_color) / 255.0;
  const CGFloat blue = SkColorGetB(theme_color) / 255.0;
  return [NSColor colorWithSRGBRed:red
                             green:green
                              blue:blue
                             alpha:GetFrameMaterialThemeTintOverlayAlpha(
                                       is_dark)];
}

NSView* CreateFrameMaterialTintView(NSRect frame, NSColor* color) {
  NSView* tint_view = [[NSView alloc] initWithFrame:frame];
  tint_view.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
  tint_view.wantsLayer = YES;
  tint_view.layer.backgroundColor = [color CGColor];
  return tint_view;
}

}  // namespace
}  // namespace helium

// NSVisualEffectView intercepts hit testing even when added below the
// WebContents NSViews. Returning nil here lets clicks pass through to the
// sibling subviews that are supposed to receive them.
@interface HeliumFrameMaterialBackgroundView : NSVisualEffectView
@property(nonatomic, strong) NSView* neutralOverlay;
@property(nonatomic, strong) NSView* themeTintOverlay;
@end

@implementation HeliumFrameMaterialBackgroundView
- (NSView*)hitTest:(NSPoint)point {
  return nil;
}
@end

namespace helium {

NSView* CreateNativeFrameMaterialBackgroundView(NSRect frame) {
  HeliumFrameMaterialBackgroundView* material_view =
      [[HeliumFrameMaterialBackgroundView alloc] initWithFrame:frame];
  material_view.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
  material_view.blendingMode = NSVisualEffectBlendingModeBehindWindow;
  material_view.state = NSVisualEffectStateActive;

  material_view.neutralOverlay =
      CreateFrameMaterialTintView(material_view.bounds, [NSColor clearColor]);
  [material_view addSubview:material_view.neutralOverlay];
  material_view.themeTintOverlay =
      CreateFrameMaterialTintView(material_view.bounds, [NSColor clearColor]);
  [material_view addSubview:material_view.themeTintOverlay];
  return material_view;
}

void UpdateNativeFrameMaterialBackgroundView(
    NSView* background_view,
    ui::ColorProviderKey::ColorMode color_mode,
    SkColor theme_color) {
  auto* material_view =
      base::apple::ObjCCastStrict<HeliumFrameMaterialBackgroundView>(
          background_view);
  const bool is_dark = color_mode == ui::ColorProviderKey::ColorMode::kDark;
  material_view.material = GetFrameMaterial(is_dark);
  material_view.appearance =
      [NSAppearance appearanceNamed:is_dark ? NSAppearanceNameVibrantDark
                                            : NSAppearanceNameVibrantLight];
  material_view.neutralOverlay.layer.backgroundColor =
      GetFrameMaterialNeutralOverlayColor(is_dark).CGColor;
  material_view.themeTintOverlay.layer.backgroundColor =
      GetFrameMaterialThemeTintOverlayColor(theme_color, is_dark).CGColor;
}

}  // namespace helium
