// Copyright 2026 The Noventra contributors
// Approved raster branding for native browser product-icon aliases.
#ifndef UI_GFX_NOVENTRA_LOGO_H_
#define UI_GFX_NOVENTRA_LOGO_H_
#include "base/component_export.h"
namespace gfx {
struct VectorIcon;
COMPONENT_EXPORT(GFX) bool IsNoventraProductIcon(const VectorIcon& icon);
}  // namespace gfx
#endif
