// Copyright 2026 The Noventra Authors
// SPDX-License-Identifier: GPL-3.0-only
#ifndef CHROME_BROWSER_UI_NOVENTRA_LAYOUT_MODEL_H_
#define CHROME_BROWSER_UI_NOVENTRA_LAYOUT_MODEL_H_

#include <optional>
#include <string>
#include <vector>
#include "base/containers/span.h"
#include "base/types/expected.h"
#include "base/values.h"

class PrefService;
namespace noventra {
inline constexpr char kDockLayoutPref[] = "noventra.browser.dock_layout";
inline constexpr char kSidebarNewTabPlacementMigratedPref[] =
    "noventra.browser.sidebar_new_tab_placement_migrated";
inline constexpr char kSidebarExtensionsMigratedPref[] =
    "noventra.browser.sidebar_extensions_migrated";
inline constexpr char kZenSidebarMigratedPref[] =
    "noventra.browser.zen_sidebar_migrated";
inline constexpr char kZenSidebarRefinedPref[] =
    "noventra.browser.zen_sidebar_refined";
inline constexpr char kCompactSidebarPref[] =
    "noventra.browser.compact_sidebar_migrated";
enum class DockEdge { kTop, kBottom, kLeft, kRight };
struct LayoutItem {
  std::string id;
  DockEdge edge = DockEdge::kTop;
  int order = 0;
  bool visible = true;
};
struct BrowserLayout {
  int version = 1;
  int revision = 0;
  std::vector<LayoutItem> items;
};
struct LayoutCapability {
  std::string id;
  std::string label;
  std::vector<DockEdge> edges;
  bool hideable = true;
  bool managed = false;
  std::optional<bool> required_visibility;
  std::string restriction;
};
struct ApplyResult {
  std::string status;
  std::string message;
  BrowserLayout layout;
  base::DictValue ToValue() const;
};
std::optional<int> NativeDockActionId(std::string_view id);
std::string_view NativeDockActionName(int action_id);
std::vector<LayoutCapability> GetLayoutCapabilities(PrefService& prefs);
base::ListValue CapabilitiesToValue(base::span<const LayoutCapability> capabilities);
base::DictValue LayoutToValue(const BrowserLayout& layout);
base::expected<BrowserLayout, std::string> ValidateLayout(
    const base::Value& value, base::span<const LayoutCapability> capabilities);
BrowserLayout ReadOrMigrateLayout(PrefService& prefs);
BrowserLayout MakeLayoutPreset(std::string_view name, PrefService& prefs);
ApplyResult ApplyLayout(PrefService& prefs, const BrowserLayout& layout,
                        int expected_revision);
}  // namespace noventra
#endif
