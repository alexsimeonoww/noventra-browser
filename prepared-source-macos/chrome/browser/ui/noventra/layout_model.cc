// Copyright 2026 The Noventra Authors
// SPDX-License-Identifier: GPL-3.0-only
#include "chrome/browser/ui/noventra/layout_model.h"

#include <algorithm>
#include <limits>
#include <set>
#include <tuple>
#include "chrome/common/pref_names.h"
#include "chrome/browser/ui/actions/chrome_action_id.h"
#include "chrome/browser/ui/toolbar/toolbar_pref_names.h"
#include "ui/actions/actions.h"
#include "components/prefs/pref_service.h"

namespace noventra {
namespace {
struct NativeAction { const char* id; const char* label; int action_id; };
constexpr NativeAction kNativeActions[] = {
    {"action:settings", "Settings", kActionOptions},
    {"action:history", "History", kActionShowHistory},
    {"action:downloads", "Downloads", kActionShowDownloads},
    {"action:bookmarks", "Bookmarks", kActionSidePanelShowBookmarks},
    {"action:print", "Print", kActionPrint},
    {"action:copy-url", "Copy page link", kActionCopyUrl},
    {"action:save-page", "Save page", kActionSavePage},
    {"action:find", "Find in page", kActionFind},
    {"action:zoom-in", "Zoom in", kActionZoomPlus},
    {"action:zoom-out", "Zoom out", kActionZoomMinus},
    {"action:zoom-reset", "Reset zoom", kActionZoomNormal},
    {"action:fullscreen", "Full screen", kActionFullscreen},
    {"action:reading-mode", "Reading mode", kActionSidePanelShowReadAnything},
    {"action:task-manager", "Task manager", kActionTaskManager},
    {"action:devtools", "Developer tools", kActionDevTools},
    {"action:passwords", "Password manager", kActionShowPasswordManager},
    {"action:bookmark-manager", "Bookmark manager",
     kActionShowBookmarkManager}};
const char* VisibilityPref(std::string_view id) {
  if (id == "back") return prefs::kShowBackButton;
  if (id == "forward") return prefs::kShowForwardButton;
  if (id == "reload") return prefs::kShowReloadButton;
  if (id == "home") return prefs::kShowHomeButton;
  if (id == "profile") return prefs::kShowAvatarButton;
  if (id == "extensions") return prefs::kShowExtensionsButton;
  if (id == "menu") return prefs::kShowMenuButton;
  if (id == "new-tab") return prefs::kShowDynamicNewTabButton;
  if (id == "media") return prefs::kShowMediaButton;
  return nullptr;
}
constexpr DockEdge kEdges[] = {DockEdge::kTop, DockEdge::kLeft, DockEdge::kRight};
std::string_view EdgeName(DockEdge edge) {
  switch (edge) {
    case DockEdge::kTop: return "top";
    case DockEdge::kBottom: return "bottom";
    case DockEdge::kLeft: return "left";
    case DockEdge::kRight: return "right";
  }
}
bool ReadBool(PrefService& prefs, const char* key, bool fallback) {
  if (!prefs.FindPreference(key)) return fallback;
  if (prefs.IsManagedPreference(key)) {
    const auto* value = prefs.GetUserPrefValue(key);
    if (!value) value = prefs.GetDefaultPrefValue(key);
    return value && value->is_bool() ? value->GetBool() : fallback;
  }
  return prefs.GetBoolean(key);
}
bool IsManaged(PrefService& prefs, const char* key) {
  return prefs.FindPreference(key) && prefs.IsManagedPreference(key);
}
DockEdge SidebarEdge(PrefService& prefs) {
  return ReadBool(prefs, prefs::kHeliumVerticalRightAligned, false)
             ? DockEdge::kRight : DockEdge::kLeft;
}
BrowserLayout Migrate(PrefService& prefs) {
  BrowserLayout layout;
  const bool vertical = prefs.GetInteger(prefs::kHeliumLayout) == 2;
  const DockEdge side = SidebarEdge(prefs);
  const DockEdge controls = vertical && ReadBool(prefs, "noventra.browser.sidebar_controls", true)
                               ? side : DockEdge::kTop;
  for (const auto& capability : GetLayoutCapabilities(prefs)) {
    LayoutItem item{capability.id, controls, static_cast<int>(layout.items.size()), true};
    if (item.id == "tabs" || item.id == "new-tab") {
      item.edge = vertical ? side : DockEdge::kTop;
    }
    if (item.id == "spaces") item.edge = side;
    if (item.id == "back" || item.id == "forward" || item.id == "reload")
      item.visible = ReadBool(prefs, "noventra.browser.show_navigation_buttons", true);
    if (item.id == "home") item.visible = ReadBool(prefs, prefs::kShowHomeButton, false);
    if (const char* key = VisibilityPref(item.id))
      item.visible &= ReadBool(prefs, key, true);
    if (item.id == "address")
      item.visible = !ReadBool(prefs, "noventra.browser.address_on_demand", false);
    if (auto action = NativeDockActionId(item.id)) {
      const auto name = actions::ActionIdMap::ActionIdToString(*action);
      item.visible = name && prefs.GetList(prefs::kPinnedActions).contains(*name);
    }
    if (item.id == "bookmarks") item.visible = vertical && controls == side;
    if (item.id == "action:bookmarks" && vertical && controls == side)
      item.visible = false;
    layout.items.push_back(std::move(item));
  }
  return layout;
}

void MigrateSidebarBookmarkFolders(PrefService& prefs, BrowserLayout& layout) {
  // This predates the Zen presets. Once that upgrade has been considered,
  // an explicit custom layout must not match this old signature again.
  if (IsManaged(prefs, kDockLayoutPref) ||
      ReadBool(prefs, kZenSidebarMigratedPref, false) ||
      layout.revision == std::numeric_limits<int>::max()) return;
  auto tabs = std::ranges::find(layout.items, "tabs", &LayoutItem::id);
  auto button = std::ranges::find(layout.items, "action:bookmarks", &LayoutItem::id);
  auto library = std::ranges::find(layout.items, "bookmarks", &LayoutItem::id);
  if (tabs == layout.items.end() || button == layout.items.end() ||
      library == layout.items.end() || library->visible || !tabs->visible ||
      (tabs->edge != DockEdge::kLeft && tabs->edge != DockEdge::kRight) ||
      library->edge != tabs->edge || button->edge != tabs->edge) return;
  std::vector<const LayoutItem*> sidebar;
  for (const auto& item : layout.items)
    if (item.visible && item.edge == tabs->edge) sidebar.push_back(&item);
  std::ranges::sort(sidebar, {}, &LayoutItem::order);
  std::vector<std::string> ids;
  for (const auto* item : sidebar) ids.push_back(item->id);
  // Upgrade only the previous standard sidebar. Hiding its old button makes
  // this idempotent; an explicit later hide of the library remains respected.
  if (ids != std::vector<std::string>{"back", "forward", "reload",
          "action:bookmarks", "extensions", "menu", "address", "new-tab",
          "tabs", "spaces"}) return;
  button->visible = false;
  library->visible = true;
  ++layout.revision;
}

void MigrateSidebarNewTab(PrefService& prefs, BrowserLayout& layout) {
  // Only the former standard top-navigation arrangement moves once. An
  // individually customized layout, or a later explicit placement, wins.
  if (!prefs.FindPreference(kSidebarNewTabPlacementMigratedPref) ||
      prefs.GetBoolean(kSidebarNewTabPlacementMigratedPref) ||
      IsManaged(prefs, kDockLayoutPref)) {
    return;
  }
  prefs.SetBoolean(kSidebarNewTabPlacementMigratedPref, true);
  auto tabs = std::ranges::find(layout.items, "tabs", &LayoutItem::id);
  auto new_tab = std::ranges::find(layout.items, "new-tab", &LayoutItem::id);
  if (tabs == layout.items.end() || new_tab == layout.items.end() ||
      !tabs->visible || !new_tab->visible ||
      (tabs->edge != DockEdge::kLeft && tabs->edge != DockEdge::kRight) ||
      layout.revision == std::numeric_limits<int>::max()) {
    return;
  }
  std::vector<std::string> top;
  for (const auto& item : layout.items) {
    if (item.visible && item.edge == DockEdge::kTop) {
      top.push_back(item.id);
    }
  }
  if (top != std::vector<std::string>{"back", "forward", "reload", "address",
                                      "new-tab", "extensions", "menu"}) {
    return;
  }
  new_tab->edge = tabs->edge;
  // Put the always-reachable New Tab button above the scrolling tab region.
  // Renumber only this edge, retaining every other control's relative order.
  int order = 1;
  for (auto& item : layout.items) {
    if (&item != &*new_tab && item.edge == tabs->edge) {
      item.order = order++;
    }
  }
  new_tab->order = 0;
  ++layout.revision;
  std::ranges::sort(layout.items, [](const auto& a, const auto& b) {
    return std::tie(a.edge, a.order, a.id) < std::tie(b.edge, b.order, b.id);
  });
}

void MigrateSidebarExtensions(PrefService& prefs, BrowserLayout& layout) {
  if (!prefs.FindPreference(kSidebarExtensionsMigratedPref) ||
      prefs.GetBoolean(kSidebarExtensionsMigratedPref) ||
      IsManaged(prefs, kDockLayoutPref) ||
      IsManaged(prefs, prefs::kShowExtensionsButton)) {
    return;
  }
  // A later explicit hide must stay hidden. Only upgrade the previous standard
  // sidebar once; customized arrangements keep their saved controls and order.
  prefs.SetBoolean(kSidebarExtensionsMigratedPref, true);
  auto tabs = std::ranges::find(layout.items, "tabs", &LayoutItem::id);
  auto extensions =
      std::ranges::find(layout.items, "extensions", &LayoutItem::id);
  if (tabs == layout.items.end() || extensions == layout.items.end() ||
      !tabs->visible || extensions->visible || extensions->edge != tabs->edge ||
      (tabs->edge != DockEdge::kLeft && tabs->edge != DockEdge::kRight) ||
      layout.revision == std::numeric_limits<int>::max()) {
    return;
  }
  std::vector<LayoutItem*> sidebar;
  for (auto& item : layout.items) {
    if (item.visible && item.edge != tabs->edge) {
      return;
    }
    if (item.edge == tabs->edge) {
      sidebar.push_back(&item);
    }
  }
  std::ranges::sort(sidebar, {}, &LayoutItem::order);
  std::vector<std::string> visible;
  for (const auto* item : sidebar) {
    if (item->visible) {
      visible.push_back(item->id);
    }
  }
  if (visible != std::vector<std::string>{"back", "forward", "reload",
          "address", "new-tab", "bookmarks", "tabs", "spaces", "menu"}) {
    return;
  }
  std::erase(sidebar, &*extensions);
  const auto reload = std::ranges::find(sidebar, "reload", [](const auto* item) {
    return item->id;
  });
  sidebar.insert(reload + 1, &*extensions);
  extensions->visible = true;
  for (size_t i = 0; i < sidebar.size(); ++i) {
    sidebar[i]->order = static_cast<int>(i);
  }
  ++layout.revision;
  std::ranges::sort(layout.items, [](const auto& a, const auto& b) {
    return std::tie(a.edge, a.order, a.id) < std::tie(b.edge, b.order, b.id);
  });
}
}  // namespace

// The previous standard sidebar upgrades once. Explicit custom arrangements
// and later changes retain their saved control placement and visibility.
void MigrateZenSidebar(PrefService& prefs, BrowserLayout& layout) {
  if (!prefs.FindPreference(kZenSidebarMigratedPref) ||
      prefs.GetBoolean(kZenSidebarMigratedPref) ||
      IsManaged(prefs, kDockLayoutPref)) {
    return;
  }
  prefs.SetBoolean(kZenSidebarMigratedPref, true);
  const auto tabs = std::ranges::find(layout.items, "tabs", &LayoutItem::id);
  if (tabs == layout.items.end() || !tabs->visible ||
      (tabs->edge != DockEdge::kLeft && tabs->edge != DockEdge::kRight) ||
      layout.revision == std::numeric_limits<int>::max()) {
    return;
  }
  std::vector<const LayoutItem*> ordered;
  for (const auto& item : layout.items) {
    if (!item.visible) {
      continue;
    }
    if (item.edge != tabs->edge) {
      return;
    }
    ordered.push_back(&item);
  }
  std::ranges::sort(ordered, {}, &LayoutItem::order);
  std::vector<std::string> visible;
  for (const auto* item : ordered) {
    visible.push_back(item->id);
  }
  const std::vector<std::string> previous = {
      "back",    "forward",   "reload", "extensions", "address",
      "new-tab", "bookmarks", "tabs",   "spaces",     "menu"};
  auto without_extensions = previous;
  std::erase(without_extensions, "extensions");
  if (visible != previous && visible != without_extensions) {
    return;
  }
  constexpr std::string_view order[] = {
      "back", "forward", "reload",           "extensions", "address",
      "tabs", "new-tab", "action:bookmarks", "spaces",     "menu"};
  for (auto& item : layout.items) {
    const auto position = std::ranges::find(order, item.id);
    item.visible = position != std::end(order);
    item.edge = tabs->edge;
    item.order = item.visible ? position - std::begin(order)
                              : 20 + (&item - layout.items.data());
  }
  ++layout.revision;
  if (!IsManaged(prefs, prefs::kHeliumRoundedFrame)) {
    prefs.SetBoolean(prefs::kHeliumRoundedFrame, true);
  }
  std::ranges::sort(layout.items, [](const auto& a, const auto& b) {
    return std::tie(a.edge, a.order, a.id) < std::tie(b.edge, b.order, b.id);
  });
}

// Upgrade the known shipped sidebar arrangements inside the browser's own
// preference transaction. Arbitrary custom/top layouts and later edits stay
// put.
void MigrateRefinedZenSidebar(PrefService& prefs, BrowserLayout& layout) {
  if (!prefs.FindPreference(kZenSidebarRefinedPref) ||
      prefs.GetBoolean(kZenSidebarRefinedPref) ||
      IsManaged(prefs, kDockLayoutPref)) {
    return;
  }
  prefs.SetBoolean(kZenSidebarRefinedPref, true);
  const auto tabs = std::ranges::find(layout.items, "tabs", &LayoutItem::id);
  if (tabs == layout.items.end() || !tabs->visible ||
      (tabs->edge != DockEdge::kLeft && tabs->edge != DockEdge::kRight) ||
      layout.revision == std::numeric_limits<int>::max()) {
    return;
  }
  std::vector<const LayoutItem*> ordered;
  for (const auto& item : layout.items) {
    if (!item.visible) {
      continue;
    }
    if (item.edge != tabs->edge) {
      return;
    }
    ordered.push_back(&item);
  }
  std::ranges::sort(ordered, {}, &LayoutItem::order);
  std::vector<std::string> visible;
  for (const auto* item : ordered) {
    visible.push_back(item->id);
  }
  const std::vector<std::string> prior_zen = {
      "back", "forward", "reload",           "extensions", "address",
      "tabs", "new-tab", "action:bookmarks", "spaces",     "menu"};
  const std::vector<std::string> prior_tools = {
      "back",    "forward", "reload", "extensions", "menu",
      "address", "new-tab", "tabs",   "spaces"};
  const std::vector<std::string> prior_preview = {
      "back", "forward",          "reload", "address", "new-tab",
      "tabs", "action:bookmarks", "menu",   "spaces"};
  if (visible != prior_zen && visible != prior_tools &&
      visible != prior_preview) {
    return;
  }
  const auto edge = tabs->edge;
  const int revision = layout.revision;
  layout = MakeLayoutPreset(
      edge == DockEdge::kRight ? "minimal-right" : "minimal-left", prefs);
  layout.revision = revision + 1;
  // Old narrow defaults cut off the native address placeholder. The address is
  // now on demand; give these known presets Zen's Linux starting width, while
  // retaining wider custom widths and the user's collapsed/auto-hide state.
  if (!IsManaged(prefs, prefs::kVerticalTabsUncollapsedWidth) &&
      prefs.GetInteger(prefs::kVerticalTabsUncollapsedWidth) < 186) {
    prefs.SetInteger(prefs::kVerticalTabsUncollapsedWidth, 186);
  }
}

// Adopt the compact starting width once for known shipped sidebar defaults.
// Custom arrangements and widths keep their choice within the new resize range.
void MigrateCompactSidebar(PrefService& prefs, const BrowserLayout& layout) {
  if (!prefs.FindPreference(kCompactSidebarPref) ||
      prefs.GetBoolean(kCompactSidebarPref) ||
      IsManaged(prefs, kDockLayoutPref)) {
    return;
  }
  prefs.SetBoolean(kCompactSidebarPref, true);
  const auto tabs = std::ranges::find(layout.items, "tabs", &LayoutItem::id);
  if (tabs == layout.items.end() || !tabs->visible ||
      (tabs->edge != DockEdge::kLeft && tabs->edge != DockEdge::kRight)) {
    return;
  }
  std::vector<const LayoutItem*> ordered;
  for (const auto& item : layout.items) {
    if (!item.visible) continue;
    if (item.edge != tabs->edge) return;
    ordered.push_back(&item);
  }
  std::ranges::sort(ordered, {}, &LayoutItem::order);
  std::vector<std::string> visible;
  for (const auto* item : ordered) visible.push_back(item->id);
  const std::vector<std::string> standard = {
      "back", "forward", "reload", "extensions", "tabs", "new-tab",
      "action:bookmarks", "spaces", "menu"};
  const int width = prefs.GetInteger(prefs::kVerticalTabsUncollapsedWidth);
  if (visible == standard && (width == 155 || width == 186) &&
      !IsManaged(prefs, prefs::kVerticalTabsUncollapsedWidth)) {
    prefs.SetInteger(prefs::kVerticalTabsUncollapsedWidth, 144);
  }
}

std::optional<int> NativeDockActionId(std::string_view id) {
  for (const auto& action : kNativeActions) if (action.id == id) return action.action_id;
  return std::nullopt;
}
std::string_view NativeDockActionName(int id) {
  for (const auto& action : kNativeActions) if (action.action_id == id) return action.id;
  return {};
}
std::vector<LayoutCapability> GetLayoutCapabilities(PrefService& prefs) {
  std::vector<LayoutCapability> result;
  for (const auto& [id, label] : {
      std::pair{"back", "Back"}, {"forward", "Forward"}, {"reload", "Reload"},
      {"home", "Home"}, {"address", "Address and search"}, {"tabs", "Tabs"},
      {"spaces", "Account spaces"}, {"saved-sites", "Saved sites"},
      {"bookmarks", "Bookmarks"},
      {"new-tab", "New tab"}, {"menu", "Browser menu"},
      {"customize", "Customize layout"}, {"extensions", "Extensions"},
      {"profile", "Profile"}, {"media", "Media controls"},
      {"additional-actions", "Additional browser actions"}}) {
    result.push_back({id, label, std::vector<DockEdge>(std::begin(kEdges), std::end(kEdges)),
                      std::string_view(id) != "tabs", IsManaged(prefs, kDockLayoutPref)});
  }
  for (const auto& action : kNativeActions)
    result.push_back({action.id, action.label,
        std::vector<DockEdge>(std::begin(kEdges), std::end(kEdges)), true,
        IsManaged(prefs, kDockLayoutPref)});
  for (auto& capability : result) {
    if (capability.id == "spaces") {
      capability.edges = {DockEdge::kLeft, DockEdge::kRight};
      capability.restriction = "Spaces stay at the bottom of the left or right sidebar.";
    }
    if (const char* key = VisibilityPref(capability.id); key && IsManaged(prefs, key))
      capability.required_visibility = prefs.GetBoolean(key);
    if (capability.id == "extensions") {
      capability.restriction = "Extension buttons move together in their native region. Pin and unpin individual extensions from the Extensions menu.";
      // A forced extension pin must remain reachable even if the legacy menu
      // preference is hidden. Runtime extension management still owns its button.
      if (prefs.FindPreference("extensions.management"))
        for (const auto entry : prefs.GetDict("extensions.management"))
          if (const auto* rule = entry.second.GetIfDict(); rule &&
              rule->FindString("toolbar_pin") && *rule->FindString("toolbar_pin") == "force_pinned")
            capability.required_visibility = true;
    }
    if (capability.id == "additional-actions")
      capability.restriction = "Contextual actions and controls without an individual adapter stay in this native region.";
  }
  return result;
}

base::ListValue CapabilitiesToValue(base::span<const LayoutCapability> capabilities) {
  base::ListValue result;
  for (const auto& item : capabilities) {
    base::ListValue edges;
    for (const auto edge : item.edges) edges.Append(EdgeName(edge));
    auto value = base::DictValue().Set("id", item.id).Set("label", item.label)
        .Set("edges", std::move(edges)).Set("hideable", item.hideable)
        .Set("managed", item.managed).Set("restriction", item.restriction);
    if (item.required_visibility) value.Set("requiredVisibility", *item.required_visibility);
    result.Append(std::move(value));
  }
  return result;
}
base::DictValue LayoutToValue(const BrowserLayout& layout) {
  base::ListValue items;
  for (const auto& item : layout.items)
    items.Append(base::DictValue().Set("id", item.id).Set("edge", EdgeName(item.edge))
        .Set("order", item.order).Set("visible", item.visible));
  return base::DictValue().Set("version", layout.version).Set("revision", layout.revision)
      .Set("items", std::move(items));
}
base::DictValue ApplyResult::ToValue() const {
  return base::DictValue().Set("status", status).Set("message", message)
      .Set("layout", LayoutToValue(layout));
}

base::expected<BrowserLayout, std::string> ValidateLayout(
    const base::Value& value, base::span<const LayoutCapability> capabilities) {
  const auto* dict = value.GetIfDict();
  if (!dict || dict->FindInt("version") != 1)
    return base::unexpected("This layout version is not supported.");
  const auto revision = dict->FindInt("revision");
  const auto* items = dict->FindList("items");
  if (!revision || *revision < 0 || !items || items->size() > 256)
    return base::unexpected("The layout is incomplete or too large.");
  BrowserLayout layout;
  layout.revision = *revision;
  std::set<std::string> ids;
  std::set<std::pair<DockEdge, int>> slots;
  for (const auto& entry : *items) {
    const auto* item = entry.GetIfDict();
    if (!item) return base::unexpected("A layout item is invalid.");
    const auto* id = item->FindString("id");
    const auto* edge = item->FindString("edge");
    auto order = item->FindInt("order");
    auto visible = item->FindBool("visible");
    if (!id || !edge || !order || *order < 0 || *order >= 256 || !visible)
      return base::unexpected("A layout item is incomplete.");
    auto capability = std::ranges::find(capabilities, *id, &LayoutCapability::id);
    if (capability == capabilities.end())
      return base::unexpected("A control is no longer available. Refresh the layout.");
    auto dock = std::ranges::find_if(capability->edges, [&](DockEdge e) { return EdgeName(e) == *edge; });
    if (dock == capability->edges.end())
      return base::unexpected("This control cannot be placed on that edge.");
    if (!ids.insert(*id).second || !slots.insert({*dock, *order}).second)
      return base::unexpected("Controls and positions must be unique.");
    if (!*visible && !capability->hideable)
      return base::unexpected("This control must remain visible.");
    layout.items.push_back({*id, *dock, *order, *visible});
  }
  for (const auto& capability : capabilities)
    if (!ids.contains(capability.id))
      return base::unexpected("A control is missing. Move unused controls to the tray.");
  std::ranges::sort(layout.items, [](const auto& a, const auto& b) {
    return std::tie(a.edge, a.order, a.id) < std::tie(b.edge, b.order, b.id);
  });
  return layout;
}

BrowserLayout ReadOrMigrateLayout(PrefService& prefs) {
  const auto capabilities = GetLayoutCapabilities(prefs);
  auto stored = prefs.GetDict(kDockLayoutPref).Clone();
  // Add newly supported controls without throwing away an existing layout.
  // Unknown persisted IDs can disappear after an upgrade or extension removal.
  if (stored.FindInt("version") == 1 && stored.FindInt("revision").value_or(-1) >= 0) {
    if (auto* items = stored.FindList("items"); items && items->size() <= 256) {
      base::ListValue upgraded;
      std::set<std::string> present;
      std::set<std::pair<std::string, int>> original_slots;
      bool well_formed = true;
      bool had_visible_favorites = false;
      for (const auto& value : *items) {
        const auto* item = value.GetIfDict();
        const auto* id = item ? item->FindString("id") : nullptr;
        if (!id) { well_formed = false; break; }
        auto capability = std::ranges::find(capabilities, *id, &LayoutCapability::id);
        if (capability == capabilities.end()) {
          if (*id == "favorites" && item->FindBool("visible").value_or(false))
            had_visible_favorites = true;
          continue;
        }
        if (!present.insert(*id).second) { well_formed = false; break; }
        // Check the original slots before moving retired placements. Otherwise
        // migration could accidentally repair malformed duplicate positions.
        const auto* edge = item->FindString("edge");
        const auto order = item->FindInt("order");
        if (!edge || !order || *order < 0 || *order >= 256 ||
            !original_slots.insert({*edge, *order}).second) {
          well_formed = false;
          break;
        }
        auto copy = item->Clone();
        upgraded.Append(std::move(copy));
      }
      if (well_formed) {
        // Older layouts allowed Spaces in a horizontal toolbar. Move only that
        // control, following vertical tabs when present and otherwise the side
        // preference. Keep its visibility, revision and every other order slot.
        DockEdge spaces_edge = SidebarEdge(prefs);
        for (const auto& value : upgraded) {
          const auto& item = value.GetDict();
          const auto* id = item.FindString("id");
          const auto* edge = item.FindString("edge");
          if (id && *id == "tabs" && edge) {
            if (*edge == "left") spaces_edge = DockEdge::kLeft;
            if (*edge == "right") spaces_edge = DockEdge::kRight;
          }
        }
        for (auto& value : upgraded) {
          auto& item = value.GetDict();
          const auto* edge = item.FindString("edge");
          const auto order = item.FindInt("order");
          if (*item.FindString("id") != "spaces" || !edge ||
              (*edge != "top" && *edge != "bottom") || !order ||
              *order < 0 || *order >= 256)
            continue;
          int available_order = *order;
          const auto occupied = [&](int slot) {
            return std::ranges::any_of(upgraded, [&](const base::Value& other) {
              const auto& peer = other.GetDict();
              return peer.FindString("edge") &&
                     *peer.FindString("edge") == EdgeName(spaces_edge) &&
                     peer.FindInt("order") == slot;
            });
          };
          if (occupied(available_order)) {
            available_order = 0;
            while (occupied(available_order)) ++available_order;
          }
          item.Set("edge", EdgeName(spaces_edge));
          item.Set("order", available_order);
        }
        // Retire the bottom dock, including hidden controls. Keep existing top
        // slots and the relative order of all migrated controls. Append when
        // possible; sparse layouts ending at slot 255 use free slots in order.
        std::vector<base::DictValue*> bottom_items;
        std::set<int> top_orders;
        int next_order = 0;
        for (auto& value : upgraded) {
          auto& item = value.GetDict();
          if (*item.FindString("edge") == "bottom") {
            bottom_items.push_back(&item);
          } else if (*item.FindString("edge") == "top") {
            const int order = *item.FindInt("order");
            top_orders.insert(order);
            next_order = std::max(next_order, order + 1);
          }
        }
        std::ranges::sort(bottom_items, [](const auto* a, const auto* b) {
          return *a->FindInt("order") < *b->FindInt("order");
        });
        if (static_cast<int>(bottom_items.size()) > 256 - next_order)
          next_order = 0;
        for (auto* item : bottom_items) {
          while (top_orders.contains(next_order)) ++next_order;
          item->Set("edge", "top");
          item->Set("order", next_order++);
        }
        for (const auto& item : Migrate(prefs).items)
          if (!present.contains(item.id)) {
            int order = 0;
            while (std::ranges::any_of(upgraded, [&](const base::Value& value) {
              const auto& other = value.GetDict();
              return other.FindString("edge") && *other.FindString("edge") == EdgeName(item.edge) && other.FindInt("order") == order;
            })) ++order;
            const bool optional_library = item.id == "bookmarks";
            upgraded.Append(base::DictValue().Set("id", item.id).Set("edge", EdgeName(item.edge))
                .Set("order", order).Set("visible", !optional_library && item.visible));
          }
        stored.Set("items", std::move(upgraded));
        auto parsed = ValidateLayout(base::Value(stored.Clone()), capabilities);
        if (parsed.has_value()) {
          // Favorites made this an explicit custom layout. Removing it must
          // not make the remaining controls look like an old shipped preset,
          // now or on a later read. Consume only the standard-layout upgrades;
          // retain every other item's placement, visibility and revision.
          if (had_visible_favorites && !IsManaged(prefs, kDockLayoutPref)) {
            for (const char* key : {kSidebarNewTabPlacementMigratedPref,
                                    kSidebarExtensionsMigratedPref,
                                    kZenSidebarMigratedPref,
                                    kZenSidebarRefinedPref,
                                    kCompactSidebarPref}) {
              if (prefs.FindPreference(key) && !IsManaged(prefs, key))
                prefs.SetBoolean(key, true);
            }
          }
          MigrateSidebarNewTab(prefs, *parsed);
          MigrateSidebarBookmarkFolders(prefs, *parsed);
          MigrateSidebarExtensions(prefs, *parsed);
          MigrateZenSidebar(prefs, *parsed);
          MigrateRefinedZenSidebar(prefs, *parsed);
          MigrateCompactSidebar(prefs, *parsed);
          // Persist placement/catalog upgrades before applying the policy overlay.
          // Keeping the revision is safe: Apply independently validates the
          // catalog and edges, so older drafts cannot restore retired placements
          // or silently drop new items.
          const auto normalized = LayoutToValue(*parsed);
          if (!prefs.IsManagedPreference(kDockLayoutPref) &&
              normalized != prefs.GetDict(kDockLayoutPref))
            prefs.SetDict(kDockLayoutPref, normalized.Clone());
          for (auto& item : parsed->items) {
            const auto cap = std::ranges::find(capabilities, item.id, &LayoutCapability::id);
            if (cap->required_visibility) item.visible = *cap->required_visibility;
          }
          return std::move(*parsed);
        }
      }
    }
  }
  auto fallback = Migrate(prefs);
  if (!prefs.HasPrefPath(kDockLayoutPref) &&
      prefs.GetInteger(prefs::kHeliumLayout) == 2) {
    fallback = MakeLayoutPreset(SidebarEdge(prefs) == DockEdge::kRight
                                    ? "minimal-right"
                                    : "minimal-left",
                                prefs);
    fallback.revision = 1;
    if (!IsManaged(prefs, prefs::kHeliumRoundedFrame)) {
      prefs.SetBoolean(prefs::kHeliumRoundedFrame, true);
    }
  }
  MigrateSidebarNewTab(prefs, fallback);
  MigrateSidebarBookmarkFolders(prefs, fallback);
  MigrateSidebarExtensions(prefs, fallback);
  MigrateZenSidebar(prefs, fallback);
  MigrateRefinedZenSidebar(prefs, fallback);
  MigrateCompactSidebar(prefs, fallback);
  // Managed data is read-only even when malformed. The in-memory default keeps
  // recovery controls reachable without rewriting the administrator's policy.
  if (!prefs.IsManagedPreference(kDockLayoutPref))
    prefs.SetDict(kDockLayoutPref, LayoutToValue(fallback));
  for (auto& item : fallback.items) {
    const auto cap = std::ranges::find(capabilities, item.id, &LayoutCapability::id);
    if (cap->required_visibility) item.visible = *cap->required_visibility;
  }
  return fallback;
}

BrowserLayout MakeLayoutPreset(std::string_view name, PrefService& prefs) {
  auto layout = Migrate(prefs);
  DockEdge edge = DockEdge::kLeft;
  if (name == "minimal-right") {
    edge = DockEdge::kRight;
  }
  if (name == "classic-top" || name == "compact-top") {
    edge = DockEdge::kTop;
  }
  if (name == "sidebar-top") {
    // Use the searchable popup instead of duplicating it with an expanded
    // library heading beside the tabs.
    constexpr std::string_view side[] = {
        "action:bookmarks", "new-tab", "tabs", "spaces"};
    constexpr std::string_view top[] = {"back",     "forward",    "reload",
                                        "address",  "extensions", "menu",
                                        "customize"};
    for (auto& item : layout.items) {
      auto position = std::ranges::find(side, item.id);
      auto navigation = std::ranges::find(top, item.id);
      item.edge = position != std::end(side) ? DockEdge::kLeft : DockEdge::kTop;
      item.visible = position != std::end(side) || navigation != std::end(top);
      item.order = position != std::end(side) ? position - std::begin(side)
                   : navigation != std::end(top)
                       ? navigation - std::begin(top)
                       : 20 + static_cast<int>(&item - layout.items.data());
    }
    return layout;
  }
  if (name == "minimal-left" || name == "minimal-right") {
    // Navigation and workspace tabs, with native address entry on demand.
    // New Tab follows ordinary tabs; bookmarks opens a compact footer popup.
    constexpr std::string_view side[] = {
        "back", "forward", "reload",           "extensions", "address",
        "tabs", "new-tab", "action:bookmarks", "spaces",     "menu"};
    for (auto& item : layout.items) {
      const auto position = std::ranges::find(side, item.id);
      item.edge = edge;
      item.visible = position != std::end(side) && item.id != "address";
      item.order = position != std::end(side)
                       ? position - std::begin(side)
                       : 20 + static_cast<int>(&item - layout.items.data());
    }
    return layout;
  }
  for (auto& item : layout.items) {
    item.edge = edge;
    if (item.id == "spaces" && edge == DockEdge::kTop) {
      item.edge = SidebarEdge(prefs);
    }
    item.visible = item.id != "home" &&
        (item.id != "bookmarks" || edge != DockEdge::kTop) &&
        (!NativeDockActionId(item.id) ||
         (item.id == "action:bookmarks" && edge == DockEdge::kTop));
    // A top-only preset must not reserve an otherwise empty sidebar just
    // for its footer. Spaces remain available in Settings and in side layouts.
    if ((name == "classic-top" || name == "compact-top") &&
        item.id == "spaces") {
      item.visible = false;
    }
  }
  if (name == "classic-top" || name == "compact-top") {
    // The saved order distinguishes the conventional two-row browser from a
    // compact single row. Existing saved layouts are never reordered here.
    const std::vector<std::string_view> order = name == "classic-top"
        ? std::vector<std::string_view>{"tabs", "new-tab", "back", "forward",
                                      "reload", "address", "extensions",
                                      "additional-actions", "profile", "menu"}
        : std::vector<std::string_view>{"back", "forward", "reload", "address",
                                      "tabs", "new-tab", "extensions",
                                      "additional-actions", "profile", "menu"};
    for (auto& item : layout.items) {
      const auto position = std::ranges::find(order, item.id);
      // The address field already supplies its native bookmark star. Keep
      // secondary tools available in Settings instead of duplicating them.
      item.visible = position != order.end();
      item.order = position != order.end()
                       ? position - order.begin()
                       : 20 + static_cast<int>(&item - layout.items.data());
    }
  }
  return layout;
}

ApplyResult ApplyLayout(PrefService& prefs,
                        const BrowserLayout& layout,
                        int expected_revision) {
  const auto current = ReadOrMigrateLayout(prefs);
  if (IsManaged(prefs, kDockLayoutPref)) {
    return {"managed", "Your organization controls this layout.", current};
  }
  if (expected_revision != current.revision ||
      layout.revision != expected_revision) {
    return {"conflict",
            "The saved layout changed. Reload the current layout before saving your choice.",
            current};
  }
  auto checked = ValidateLayout(base::Value(LayoutToValue(layout)), GetLayoutCapabilities(prefs));
  if (!checked.has_value()) return {"invalid", checked.error(), current};
  if (current.revision == std::numeric_limits<int>::max())
    return {"invalid", "The layout revision limit was reached.", current};
  for (const auto& capability : GetLayoutCapabilities(prefs)) {
    if (!capability.required_visibility) continue;
    const auto item = std::ranges::find(checked->items, capability.id, &LayoutItem::id);
    if (item->visible != *capability.required_visibility)
      return {"managed", "Your organization controls this control's visibility.", current};
  }
  checked->revision = current.revision + 1;
  auto persisted = *checked;
  for (const auto& capability : GetLayoutCapabilities(prefs)) {
    if (!capability.required_visibility) continue;
    auto item = std::ranges::find(persisted.items, capability.id, &LayoutItem::id);
    if (const auto* previous = prefs.GetDict(kDockLayoutPref).FindList("items")) {
      for (const auto& value : *previous) {
        const auto* saved = value.GetIfDict();
        if (saved && saved->FindString("id") && *saved->FindString("id") == capability.id)
          item->visible = saved->FindBool("visible").value_or(item->visible);
      }
    }
  }
  prefs.SetDict(kDockLayoutPref, LayoutToValue(persisted));
  return {"ok", "Layout applied.", std::move(*checked)};
}
}  // namespace noventra
