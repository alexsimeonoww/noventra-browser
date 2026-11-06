#include "chrome/browser/ui/noventra/window_mirror_service.h"
// Copyright 2026 The Noventra Authors
// SPDX-License-Identifier: GPL-3.0-only
#include "chrome/browser/ui/views/frame/noventra_dock_controller.h"

#include <algorithm>

#include "base/functional/bind.h"
#include "base/i18n/rtl.h"
#include "base/numerics/safe_conversions.h"
#include "base/task/single_thread_task_runner.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/profiles/noventra_account_spaces.h"
#include "chrome/browser/ui/browser_commands.h"
#include "chrome/browser/ui/layout_constants.h"
#include "chrome/browser/ui/omnibox/omnibox_popup_view.h"
#include "chrome/browser/ui/omnibox/omnibox_view.h"
#include "chrome/browser/ui/tabs/vertical_tab_strip_state_controller.h"
#include "chrome/browser/ui/views/extensions/extensions_toolbar_desktop.h"
#include "chrome/browser/ui/views/frame/browser_frame_view.h"
#include "chrome/browser/ui/views/frame/browser_view.h"
#include "chrome/browser/ui/views/frame/browser_widget.h"
#include "chrome/browser/ui/views/frame/layout/browser_view_layout_params.h"
#include "chrome/browser/ui/views/frame/noventra_bookmarks_view.h"
#include "chrome/browser/ui/views/frame/noventra_dock_view.h"
#include "chrome/browser/ui/views/frame/tab_strip_region_view.h"
#include "chrome/browser/ui/views/frame/top_container_view.h"
#include "chrome/browser/ui/views/frame/vertical_tab_strip_region_view.h"
#include "chrome/browser/ui/views/helium/native_frame_materials.h"
#include "chrome/browser/ui/views/location_bar/location_bar_view.h"
#include "chrome/browser/ui/views/tabs/common/tab_strip_view.h"
#include "chrome/browser/ui/views/tabs/vertical/vertical_tab_strip_bottom_container.h"
#include "chrome/browser/ui/views/toolbar/toolbar_button.h"
#include "chrome/browser/ui/views/toolbar/toolbar_view.h"
#include "chrome/common/pref_names.h"
#include "components/bookmarks/common/bookmark_pref_names.h"
#include "components/prefs/pref_service.h"
#include "components/vector_icons/vector_icons.h"
#include "content/public/browser/web_contents.h"
#include "third_party/skia/include/core/SkPath.h"
#include "third_party/skia/include/core/SkRegion.h"
#include "ui/compositor/layer.h"
#include "ui/gfx/geometry/skia_conversions.h"
#include "ui/views/accessibility/view_accessibility.h"
#include "ui/views/layout/animating_layout_manager.h"
#include "ui/views/view.h"
#include "ui/views/view_tracker.h"
#include "ui/views/view_utils.h"
#include "ui/views/widget/widget.h"

NoventraDockController::NoventraDockController(BrowserView& browser)
    : browser_(&browser) {
  focus_manager_ = browser.GetFocusManager();
  focus_manager_->AddFocusChangeListener(this);
  registrar_.Init(browser.GetProfile()->GetPrefs());
  registrar_.Add(bookmarks::prefs::kShowBookmarkBar,
                 base::BindRepeating(
                     [](NoventraDockController* self) {
                       // On New Tab the controller can remain in SHOW state
                       // while the sidebar suppresses the automatic bar.
                       // Pin/unpin still needs a fresh layout even when that
                       // underlying state does not change.
                       if (self->HasSidebarLibrary()) {
                         self->browser_->InvalidateLayout();
                       }
                     },
                     base::Unretained(this)));
  registrar_.Add(
      helium::kNoventraGlassEnabledPref,
      base::BindRepeating(&NoventraDockController::RefreshGlassAppearance,
                          base::Unretained(this)));
  registrar_.Add(noventra::kSpaceAppearancePref,
                 base::BindRepeating(&BrowserView::RefreshNoventraSpaceTheme,
                                     browser.GetAsWeakPtr()));
  browser.RefreshNoventraSpaceTheme();
  using BatterySaver =
      performance_manager::user_tuning::BatterySaverModeManager;
  if (BatterySaver::HasInstance()) {
    battery_saver_observation_.Observe(BatterySaver::GetInstance());
  }
  registrar_.Add(noventra::kWindowMirroring,
                 base::BindRepeating(&NoventraDockController::ScheduleApply,
                                     base::Unretained(this)));
  registrar_.Add(noventra::kDockLayoutPref,
                 base::BindRepeating(&NoventraDockController::ScheduleApply,
                                     base::Unretained(this)));
  for (const char* key : {"browser.show_home_button", "helium.browser.show_back_button",
       "browser.show_forward_button", "helium.browser.show_reload_button",
       "helium.browser.show_avatar_button", "helium.browser.show_extensions_button",
       "helium.browser.show_menu_button", "helium.browser.show_media_button",
       "helium.browser.show_dynamic_new_tab_button", "extensions.management"}) {
    if (browser.GetProfile()->GetPrefs()->FindPreference(key))
      registrar_.Add(key, base::BindRepeating(&NoventraDockController::ScheduleApply, base::Unretained(this)));
  }
  // Defer reparenting beyond initialization and preference observer callbacks.
  ScheduleApply();
  RefreshGlassAppearance();
}
NoventraDockController::~NoventraDockController() {
  weak_factory_.InvalidateWeakPtrs();
  ObserveAddressAnchor(nullptr);
  registrar_.RemoveAll();
  battery_saver_observation_.Reset();
  if (focus_manager_) focus_manager_->RemoveFocusChangeListener(this);
  for (const auto& [edge, dock] : docks_) {
    dock->RestoreCompactPresentation();
  }
  RestoreEmbeddedBookmarks();
  if (address_view_ && address_view_->parent())
    browser_->toolbar()->AddChildView(address_view_->parent()->RemoveChildViewT(address_view_.get()));
  // ToolbarView's typed pointers must remain valid through its destructor.
  // Restore ownership before BrowserView tears down either native subtree.
  for (const auto& [id, view] : controls_)
    if (id != "address" && view && view->parent())
      original_parents_.at(id)->AddChildView(view->parent()->RemoveChildViewT(view.get()));
  controls_.clear();
}
void NoventraDockController::RefreshGlassAppearance() {
  base::SingleThreadTaskRunner::GetCurrentDefault()->PostTask(
      FROM_HERE, base::BindOnce(
                     [](base::WeakPtr<NoventraDockController> self) {
                       if (self) {
                         self->browser_->RefreshNoventraGlassAppearance();
                       }
                     },
                     weak_factory_.GetWeakPtr()));
}
void NoventraDockController::OnBatterySaverActiveChanged(bool is_active) {
  RefreshGlassAppearance();
}
void NoventraDockController::OnBatterySaverModeManagerDestroyed() {
  battery_saver_observation_.Reset();
  RefreshGlassAppearance();
}
void NoventraDockController::ScheduleApply() {
  if (pending_) return;
  pending_ = true;
  base::SingleThreadTaskRunner::GetCurrentDefault()->PostTask(FROM_HERE,
      base::BindOnce(&NoventraDockController::ApplySaved, weak_factory_.GetWeakPtr()));
}
void NoventraDockController::ApplySaved() {
  pending_ = false;
  auto* prefs = browser_->GetProfile()->GetPrefs();
  auto layout = noventra::ReadOrMigrateLayout(*prefs);
  Apply(layout);
}
std::vector<noventra::LayoutCapability> NoventraDockController::GetCapabilities() const {
  return noventra::GetLayoutCapabilities(*browser_->GetProfile()->GetPrefs());
}
void NoventraDockController::Apply(const noventra::BrowserLayout& layout) {
  if (layout.revision == 0 && !active_) return;
  if (!browser_->GetWidget() || browser_->GetWidget()->IsClosed()) return;
  ObserveAddressAnchor(nullptr);
  for (const auto& [edge, dock] : docks_) {
    dock->RestoreCompactPresentation();
  }
  views::ViewTracker previous_focus(browser_->GetFocusManager()->GetFocusedView());
  RestoreEmbeddedBookmarks();
  auto* location_bar = browser_->toolbar()->location_bar_view();
  const bool restore_address = previous_focus.view() && location_bar->Contains(previous_focus.view());
  auto* omnibox = location_bar->GetOmniboxView();
  const auto selection = omnibox->GetSelectionBounds();
  const bool restore_popup = restore_address && location_bar->GetOmniboxPopupView() &&
      location_bar->GetOmniboxPopupView()->IsOpen();
  active_ = true;
  std::vector<const noventra::LayoutItem*> top_items;
  for (const auto& item : layout.items) {
    if (item.visible && item.edge == noventra::DockEdge::kTop)
      top_items.push_back(&item);
  }
  std::ranges::sort(top_items, {}, [](const auto* item) { return item->order; });
  native_top_new_tab_ = top_items.size() >= 2 &&
      top_items[0]->id == "tabs" && top_items[1]->id == "new-tab";
  browser_->toolbar()->SetNoventraDockingActive();
  for (const auto edge : {noventra::DockEdge::kTop, noventra::DockEdge::kBottom,
                          noventra::DockEdge::kLeft, noventra::DockEdge::kRight}) {
    if (!docks_.contains(edge))
      docks_[edge] = browser_->AddChildViewAt(std::make_unique<NoventraDockView>(edge),
          browser_->GetIndexOf(browser_->top_container()).value() + 1);
  }
  // Change only the strip presentation. The tab model and WebContents stay put.
  for (const auto& item : layout.items) {
    if (item.id != "tabs") continue;
    auto* state = tabs::VerticalTabStripStateController::From(browser_->browser());
    const bool vertical = item.edge == noventra::DockEdge::kLeft || item.edge == noventra::DockEdge::kRight;
    if (controls_.contains("tabs") &&
        views::IsViewClass<VerticalTabStripRegionView>(controls_.at("tabs")) !=
            vertical) {
      auto* old = controls_.at("tabs").get();
      original_parents_.at("tabs")->AddChildView(old->parent()->RemoveChildViewT(old));
      controls_.erase("tabs");
    }
    if (state->ShouldDisplayVerticalTabs() != vertical) state->SetVerticalTabsEnabled(vertical);
    if (vertical) {
      auto* region = static_cast<VerticalTabStripRegionView*>(browser_->tab_strip_view());
      region->GetBottomContainer()->SetVisible(false);
      region->SetHasLeadingExclusionForLayout(false);
      region->SetFloatingStyleForLayout(
          VerticalTabStripRegionView::FloatingStyle::kNone, 0, false);
    }
    if (vertical && state->IsTabStripRightAligned() != (item.edge == noventra::DockEdge::kRight))
      state->SetTabStripRightAligned(item.edge == noventra::DockEdge::kRight);
  }
  for (const auto& item : layout.items) {
    if (!controls_.contains(item.id)) {
      std::unique_ptr<views::View> view;
      if (item.id == "address") {
        address_pane_ = browser_->AddChildViewAt(std::make_unique<NoventraDockView>(noventra::DockEdge::kTop),
            browser_->GetIndexOf(browser_->top_container()).value() + docks_.size() + 1);
        auto field = browser_->toolbar()->ReleaseNoventraControl("address");
        field->SetProperty(kNoventraDockKind, 1);
        address_view_ = address_pane_->AddChildView(std::move(field));
        auto search = std::make_unique<ToolbarButton>(base::BindRepeating(
            [](BrowserView* browser) { browser->SetFocusToLocationBar(true); },
            base::Unretained(browser_.get())));
        search->SetVectorIcon(vector_icons::kSearchIcon);
        search->SetTooltipText(u"Search or enter address (Ctrl+L)");
        search->SetAccessibleName(u"Search or enter address");
        search->SetHorizontalAlignment(gfx::ALIGN_CENTER);
        search->SetPaintToLayer();
        search->layer()->SetFillsBoundsOpaquely(false);
        view = std::move(search);
        view->SetProperty(kNoventraDockKind, 1);
      } else if (item.id == "tabs") {
        auto* region = browser_->tab_strip_view();
        original_parents_[item.id] = region->parent();
        view = region->parent()->RemoveChildViewT(region);
        view->SetProperty(kNoventraDockKind, 2);
      } else if (item.id == "action:bookmarks") {
        original_parents_[item.id] = browser_;
        view = std::make_unique<NoventraBookmarksView>(
            *browser_, false,
            NoventraBookmarksView::Presentation::kPanelButton);
        view->SetProperty(kNoventraDockBookmarkPanel, true);
      } else if (item.id == "favorites" || item.id == "bookmarks") {
        original_parents_[item.id] = browser_;
        view = std::make_unique<NoventraBookmarksView>(*browser_, item.id == "favorites");
        view->SetProperty(kNoventraDockKind, 5);
      } else if (item.id == "spaces") {
        auto* spaces = browser_->GetNoventraAccountSpacesControl();
        original_parents_[item.id] = spaces->parent();
        view = spaces->parent()->RemoveChildViewT(spaces);
        view->SetProperty(kNoventraDockKind, 3);
      } else {
        views::View* original_parent = nullptr;
        view = browser_->toolbar()->ReleaseNoventraControl(item.id, &original_parent);
        if (!view) continue;
        original_parents_[item.id] = original_parent;
        if (item.id == "extensions" || item.id == "additional-actions")
          view->SetProperty(kNoventraDockKind, 4);
      }
      controls_[item.id] = docks_.at(item.edge)->AddChildView(std::move(view));
    }
    auto* view = controls_.at(item.id).get();
    view->SetProperty(kNoventraDockOverflowable,
                      item.id == "back" || item.id == "forward" ||
                          item.id == "reload" || item.id == "extensions");
    if (auto* button = views::AsViewClass<ToolbarButton>(view)) {
      // Toolbar controls normally align to the trailing edge of a flex slot.
      // A dock owns square cells: keep the icon and ink-drop centres together.
      button->SetHorizontalAlignment(gfx::ALIGN_CENTER);
      button->SetLeadingMargin(0);
      button->SetTrailingMargin(0);
    }
    auto* dock = docks_.at(item.edge).get();
    if (view->parent() != dock)
      dock->AddChildView(view->parent()->RemoveChildViewT(view));
    dock->ReorderChildView(view, dock->children().size() - 1);
    const bool was_visible = view->GetVisible();
    if (item.id == "media") browser_->toolbar()->SetNoventraMediaVisibility(item.visible);
    else {
      view->SetVisible(
          item.visible &&
          (item.id != "new-tab" ||
           (!native_top_new_tab_ &&
            !noventra::WindowMirrorService::IsEnabled(browser_->GetProfile()))));
    }
    if (item.visible && !was_visible) {
      // Native extension animations require a fresh layout when their host
      // is shown again. Pinning while hidden can leave a constrained layout
      // with invisible actions even after the dock allocates enough width.
      if (auto* extensions = views::AsViewClass<ExtensionsToolbarDesktop>(view))
        extensions->GetAnimatingLayoutManager()->ResetLayout();
    }
  }
  // Child order is also native keyboard traversal order. Match the footer's
  // visual position even when an older layout saved Spaces before other tools.
  if (const auto spaces = controls_.find("spaces"); spaces != controls_.end()) {
    auto* view = spaces->second.get();
    view->parent()->ReorderChildView(view, view->parent()->children().size() - 1);
  }
  const auto library = controls_.find("bookmarks");
  const auto tabs = controls_.find("tabs");
  const auto address = controls_.find("address");
  if (library != controls_.end() && tabs != controls_.end() &&
      address != controls_.end() && library->second->GetVisible() &&
      tabs->second->GetVisible() && address->second->GetVisible() &&
      library->second->parent() == tabs->second->parent() &&
      address->second->parent() == tabs->second->parent()) {
    auto* dock = library->second->parent();
    const bool combined = dock->GetIndexOf(address->second).value() <
                          dock->GetIndexOf(tabs->second).value();
    if (combined) {
      if (auto* region = views::AsViewClass<VerticalTabStripRegionView>(
              tabs->second)) {
        if (views::IsViewClass<TabStripView>(region->GetTabStripView())) {
          auto* section =
              views::AsViewClass<NoventraBookmarksView>(library->second);
          section->SetInlineFolders(true);
          // Keep the library in the dock so it can sit above the space
          // controls without stretching the native tab list or New Tab row.
          dock->ReorderChildView(section, dock->children().size() - 1);
          const auto spaces = controls_.find("spaces");
          if (spaces != controls_.end() && spaces->second->parent() == dock) {
            dock->ReorderChildView(spaces->second, dock->children().size() - 1);
          }
        }
      }
    }
  }
  if (tabs != controls_.end() && address != controls_.end() &&
      tabs->second->GetVisible() &&
      tabs->second->parent() == address->second->parent()) {
    const auto new_tab = controls_.find("new-tab");
    auto* dock = tabs->second->parent();
    bool after_tabs = false;
    bool new_tab_follows_tabs = false;
    for (views::View* child : dock->children()) {
      if (child == tabs->second) {
        after_tabs = true;
        continue;
      }
      bool footer_control = false;
      for (const auto* id : {"spaces", "menu", "action:bookmarks"}) {
        const auto control = controls_.find(id);
        footer_control |=
            control != controls_.end() && control->second == child;
      }
      if (after_tabs && child->GetVisible() && !footer_control) {
        new_tab_follows_tabs =
            new_tab != controls_.end() && new_tab->second == child;
        break;
      }
    }
    if (new_tab != controls_.end() && new_tab->second->GetVisible() &&
        new_tab->second->parent() == dock && new_tab_follows_tabs &&
        (!address->second->GetVisible() ||
         dock->GetIndexOf(address->second) < dock->GetIndexOf(tabs->second)) &&
        dock->GetIndexOf(tabs->second) < dock->GetIndexOf(new_tab->second)) {
      if (auto* region =
              views::AsViewClass<VerticalTabStripRegionView>(tabs->second)) {
        if (auto* strip =
                views::AsViewClass<TabStripView>(region->GetTabStripView())) {
          strip->AttachNoventraNewTab(
              dock->RemoveChildViewT(new_tab->second.get()));
        }
      }
    }
  }
  browser_->InvalidateLayout();
  // First activation reparents the address field. Views clears focus during
  // removal; restore it only after the new native owner is laid out. Subsequent
  // moves keep the stable address pane and do not touch the user's focus.
  if (auto* focused = previous_focus.view(); focused &&
      focused != browser_->GetFocusManager()->GetFocusedView() &&
      focused->GetWidget() == browser_->GetWidget()) {
    if (restore_address) address_requested_ = true;
    browser_->DeprecatedLayoutImmediately();
    if (focused->IsDrawn()) {
      focused->RequestFocus();
      if (restore_address) {
        omnibox->SetSelectionBounds(selection);
        if (restore_popup) omnibox->UpdatePopup();
      }
    }
  }
}
void NoventraDockController::RestoreEmbeddedBookmarks() {
  const auto new_tab = controls_.find("new-tab");
  if (new_tab != controls_.end() && new_tab->second->parent() &&
      new_tab->second->parent()->GetProperty(kNoventraDockKind) == 6) {
    if (auto* strip = views::AsViewClass<TabStripView>(
            new_tab->second->parent()->parent())) {
      // Mirroring prunes the native strip. Clear inherited AX pruning before
      // moving a control out: reparenting does not clear its latched leaf
      // state.
      auto& accessibility = strip->GetViewAccessibility();
      const bool was_leaf = accessibility.views::ViewAccessibility::IsLeaf();
      accessibility.SetIsLeaf(false);
      auto control = strip->ReleaseNoventraNewTab();
      accessibility.SetIsLeaf(was_leaf);
      original_parents_.at("new-tab")->AddChildView(std::move(control));
    }
  }
  const auto library = controls_.find("bookmarks");
  if (library == controls_.end()) return;
  auto* section = views::AsViewClass<NoventraBookmarksView>(library->second);
  section->SetInlineFolders(false);
  if (auto* strip = views::AsViewClass<TabStripView>(section->parent())) {
    auto& accessibility = strip->GetViewAccessibility();
    const bool was_leaf = accessibility.views::ViewAccessibility::IsLeaf();
    accessibility.SetIsLeaf(false);
    auto view = strip->ReleaseNoventraBookmarks();
    accessibility.SetIsLeaf(was_leaf);
    original_parents_.at("bookmarks")->AddChildView(std::move(view));
  }
}
bool NoventraDockController::HasSidebarLibrary() const {
  if (!active_) return false;
  for (const auto* id : {"favorites", "bookmarks", "action:bookmarks"}) {
    const auto control = controls_.find(id);
    if (control == controls_.end() || !control->second->GetVisible()) continue;
    for (const auto edge : {noventra::DockEdge::kLeft, noventra::DockEdge::kRight})
      if (docks_.at(edge)->Contains(control->second)) return true;
  }
  return false;
}
gfx::Rect NoventraDockController::Layout(const gfx::Rect& client_bounds) {
  if (!active_) return client_bounds;
  last_client_bounds_ = client_bounds;
  has_layout_ = true;
  dock_geometry_.clear();
  const auto frame = browser_->browser_widget()->GetFrameView()->GetBrowserLayoutParams();
  const int caption_height = base::ClampCeil(std::max(
      frame.leading_exclusion.content.height(), frame.trailing_exclusion.content.height()));
  const int leading_width = base::ClampCeil(frame.leading_exclusion.ContentWithPadding().width());
  const int trailing_width = base::ClampCeil(frame.trailing_exclusion.ContentWithPadding().width());
  const int physical_left = base::i18n::IsRTL() ? trailing_width : leading_width;
  const int physical_right = base::i18n::IsRTL() ? leading_width : trailing_width;
  leading_caption_bounds_ = gfx::Rect(client_bounds.x(), client_bounds.y(),
                                     physical_left, caption_height);
  trailing_caption_bounds_ = gfx::Rect(client_bounds.right() - physical_right,
                                      client_bounds.y(), physical_right, caption_height);
  gfx::Rect remaining = client_bounds;
  gfx::Rect content = client_bounds;
  const bool zen = browser_->IsZenModeEnabledForLayout();
  force_reveal_top_ = HasFocusedControl(false);
  force_reveal_side_ = HasFocusedControl(true);
  for (const auto edge : {noventra::DockEdge::kTop, noventra::DockEdge::kBottom,
                          noventra::DockEdge::kLeft, noventra::DockEdge::kRight}) {
    auto* dock = docks_.at(edge).get();
    bool visible = dock->HasVisibleControls() && !browser_->IsFullscreen();
    dock->SetVisible(visible);
    if (!visible) continue;
    gfx::Rect bounds = remaining;
    const bool side = edge == noventra::DockEdge::kLeft || edge == noventra::DockEdge::kRight;
    const bool floating = zen && !(side ? browser_->IsZenModeSideChromePinned() : browser_->IsZenModeTopChromePinned());
    if (edge == noventra::DockEdge::kTop)
      dock->SetCaptionExclusion(
          physical_left, physical_right, caption_height);
    const int caption_inset =
        side ? std::max(0, client_bounds.y() + caption_height - remaining.y())
             : 0;
    const bool extend_side_material =
        side && helium::ShouldUseNativeFrameMaterials(browser_.get());
    // Paint the sidebar material right up to the window edge. Keep the same
    // caption reserve inside its control layout instead of leaving an untinted
    // strip above the entire sidebar. Solid frames retain their existing
    // bounds.
    dock->SetVerticalContentInset(extend_side_material ? caption_inset : 0);
    const int available_height = std::max(
        0, remaining.height() - (extend_side_material ? 0 : caption_inset));
    dock->PrepareForLayout(available_height);
    const int cross = dock->CrossAxisSize(std::max(0, remaining.width()), available_height);
    gfx::Insets inset;
    switch (edge) {
      case noventra::DockEdge::kTop:
        bounds.set_height(cross); inset = gfx::Insets::TLBR(cross,0,0,0); break;
      case noventra::DockEdge::kBottom:
        bounds.set_y(remaining.bottom()-cross); bounds.set_height(cross);
        inset = gfx::Insets::TLBR(0,0,cross,0); break;
      case noventra::DockEdge::kLeft:
        bounds.set_width(cross); inset = gfx::Insets::TLBR(0,cross,0,0); break;
      case noventra::DockEdge::kRight:
        bounds.set_x(remaining.right()-cross); bounds.set_width(cross);
        inset = gfx::Insets::TLBR(0,0,0,cross); break;
    }
    remaining.Inset(inset);
    if (!floating) content.Inset(inset);
    // Window caption controls remain frame-owned and must not be covered by a
    // dock's native input surface. Edge names are physical, including in RTL.
    if (side && !extend_side_material) {
      bounds.Inset(gfx::Insets::TLBR(caption_inset, 0, 0, 0));
    }
    dock_geometry_.push_back({edge, bounds, floating});
    PositionDock(dock_geometry_.back());
    if (dock->needs_layout()) dock->DeprecatedLayoutImmediately();
  }
  UpdateAddressPaneBounds();
  browser_->browser_widget()->GetFrameView()->SchedulePaint();
  browser_->browser_widget()->GetFrameView()->SetCaptionButtonsOpacity(
      browser_->GetTopChromeRevealRatio());
  // The frame consumes logical coordinates; the model names physical edges.
  content.set_x(browser_->GetMirroredXForRect(content));
  return content;
}

SkPath NoventraDockController::GetFrameMaterialCutout(
    const views::View* frame) const {
  if (!active_ || !has_layout_) {
    return SkPath();
  }
  const auto convert = [&](const gfx::Rect& bounds) {
    return gfx::RectToSkIRect(
        views::View::ConvertRectToTarget(browser_, frame, bounds));
  };
  SkRegion client(convert(last_client_bounds_));
  client.op(convert(leading_caption_bounds_), SkRegion::kDifference_Op);
  client.op(convert(trailing_caption_bounds_), SkRegion::kDifference_Op);
  return client.getBoundaryPath();
}
void NoventraDockController::PositionDock(const DockGeometry& geometry) {
  auto bounds = geometry.bounds;
  const auto edge = geometry.edge;
  if (geometry.floating) {
    const bool side = edge == noventra::DockEdge::kLeft ||
                      edge == noventra::DockEdge::kRight;
    const float reveal = (side ? force_reveal_side_ : force_reveal_top_) ? 1.f :
        (side ? browser_->GetSideChromeRevealRatio() :
                browser_->GetTopChromeRevealRatio());
    const int cross = side ? bounds.width() : bounds.height();
    const int offset = base::ClampRound(cross * (1.f - reveal));
    bounds.Offset(edge == noventra::DockEdge::kLeft ? -offset :
                  edge == noventra::DockEdge::kRight ? offset : 0,
                  edge == noventra::DockEdge::kTop ? -offset :
                  edge == noventra::DockEdge::kBottom ? offset : 0);
  }
  const auto physical_bounds = bounds;
  bounds.set_x(browser_->GetMirroredXForRect(bounds));
  auto* dock = docks_.at(edge).get();
  dock->SetFloatingGlass(geometry.floating);
  dock->SetBoundsRect(bounds);
  UpdateCaptionClip(dock, physical_bounds);
}
void NoventraDockController::UpdateCaptionClip(NoventraDockView* dock,
                                               const gfx::Rect& bounds) {
  auto leading = leading_caption_bounds_;
  auto trailing = trailing_caption_bounds_;
  leading.Offset(-bounds.x(), -bounds.y());
  trailing.Offset(-bounds.x(), -bounds.y());
  dock->SetCaptionClip(leading, trailing);
}
void NoventraDockController::ObserveAddressAnchor(views::View* anchor) {
  if (address_anchor_observation_.GetSource() == anchor) {
    return;
  }
  SetAddressAnchorHidden(false);
  address_anchor_notifications_.reset();
  address_anchor_observation_.Reset();
  if (anchor) {
    address_anchor_observation_.Observe(anchor);
    address_anchor_notifications_ = std::make_unique<
        views::View::ScopedNotifyObserversOnVisibleBoundsChanged>(*anchor);
  }
}

void NoventraDockController::SetAddressAnchorHidden(bool hidden) {
  auto* anchor = address_anchor_observation_.GetSource();
  if (!anchor || address_anchor_hidden_ == hidden) {
    return;
  }
  address_anchor_hidden_ = hidden;
  if (hidden) {
    address_anchor_layer_created_ = !anchor->layer();
    if (address_anchor_layer_created_) {
      anchor->SetPaintToLayer();
      anchor->layer()->SetFillsBoundsOpaquely(false);
    }
    address_anchor_opacity_ = anchor->layer()->opacity();
    anchor->layer()->SetOpacity(0.f);
  } else {
    anchor->layer()->SetOpacity(address_anchor_opacity_);
    if (address_anchor_layer_created_) {
      anchor->DestroyLayer();
    }
  }
}

void NoventraDockController::ScheduleAddressPaneUpdate() {
  if (address_update_pending_) {
    return;
  }
  address_update_pending_ = true;
  base::SingleThreadTaskRunner::GetCurrentDefault()->PostTask(
      FROM_HERE, base::BindOnce(
                     [](base::WeakPtr<NoventraDockController> self) {
                       if (self) {
                         self->address_update_pending_ = false;
                         self->UpdateAddressPaneBounds();
                       }
                     },
                     weak_factory_.GetWeakPtr()));
}

void NoventraDockController::OnViewBoundsChanged(views::View* view) {
  ScheduleAddressPaneUpdate();
}
void NoventraDockController::OnViewVisibleBoundsChanged(views::View* view) {
  ScheduleAddressPaneUpdate();
}
void NoventraDockController::OnViewVisibilityChanged(views::View* view,
                                                     views::View* starting_view,
                                                     bool visible) {
  ScheduleAddressPaneUpdate();
}
void NoventraDockController::OnViewHierarchyWillBeDeleted(views::View* view) {
  // The real field has stable BrowserView ownership and must never be deleted
  // with a tab. Stop observing before Views deletes the title's ancestors.
  ObserveAddressAnchor(nullptr);
  if (address_pane_) {
    address_pane_->SetVisible(false);
  }
  ScheduleAddressPaneUpdate();
}

void NoventraDockController::UpdateAddressPaneBounds() {
  if (!address_pane_ || !has_layout_) {
    return;
  }
  const auto& client_bounds = last_client_bounds_;
  if (browser_->IsNoventraTabDragActive()) {
    // Tab drag transforms animate both the source and neighboring rows without
    // moving their layout bounds. Restore tab labels until that gesture ends.
    ObserveAddressAnchor(nullptr);
    command_popup_visible_ = false;
    address_pane_->SetVisible(false);
    return;
  }
  auto* slot = controls_.at("address").get();
  auto* field = browser_->toolbar()->location_bar_view();
  const bool editing =
      address_requested_ ||
      address_view_->Contains(browser_->GetFocusManager()->GetFocusedView());
  const bool new_tab_edit =
      editing && address_edit_intent_ == AddressEditIntent::kNewTab;
  gfx::Rect bounds =
      views::View::ConvertRectToTarget(slot, browser_, slot->GetLocalBounds());
  NoventraDockView* side_dock = nullptr;
  bool tab_address_sidebar = false;
  views::View* anchor = nullptr;
  for (const auto& [edge, dock] : docks_) {
    if (slot->parent() == dock && (edge == noventra::DockEdge::kLeft ||
                                   edge == noventra::DockEdge::kRight)) {
      side_dock = dock;
      bool address_before_tabs = false;
      for (views::View* child : dock->children()) {
        if (child == slot) {
          address_before_tabs = slot->GetVisible();
        }
        if (auto* tabs =
                views::AsViewClass<VerticalTabStripRegionView>(child)) {
          // Hiding the resting address row keeps keyboard editing in the
          // centered native editor instead of overlaying an active tab title.
          tab_address_sidebar = (address_before_tabs || !slot->GetVisible()) &&
                                tabs->GetVisible();
          if (!tab_address_sidebar) anchor = tabs->GetNoventraAddressAnchor();
          break;
        }
      }
      break;
    }
  }
  ObserveAddressAnchor(anchor);
  const bool merged =
      anchor && anchor->IsDrawn() && !anchor->GetLocalBounds().IsEmpty() &&
      anchor->parent()->GetVisibleBounds().Contains(gfx::Rect(
          anchor->x(), 0, anchor->width(), anchor->parent()->height()));
  if (merged) {
    // Follow the active tab's actual native title bounds, including scrolling,
    // RTL and close/audio indicators. Its favicon remains a tab drag target.
    bounds = views::View::ConvertRectToTarget(
        anchor, browser_,
        gfx::Rect(0, -anchor->y(), anchor->width(),
                  anchor->parent()->height()));
  } else if (anchor && side_dock) {
    // In the icon rail the title is hidden, but the active row still anchors
    // its click-to-edit popup and keyboard editing.
    bounds = views::View::ConvertRectToTarget(
        anchor->parent(), browser_, anchor->parent()->GetLocalBounds());
  }
  bool inline_field = !new_tab_edit && !tab_address_sidebar &&
                      (merged || (slot->IsDrawn() &&
                                  bounds.width() >= (side_dock ? 160 : 100) &&
                                  client_bounds.Intersects(bounds)));
  // A combined sidebar has its own resting address card. Focus moves the
  // same native field into the floating editor without reparenting it.
  if (tab_address_sidebar && !editing && slot->IsDrawn()) {
    bounds.Inset(gfx::Insets::VH(0, 5));
    inline_field = bounds.width() >= 100;
  }
  if (inline_field && side_dock && !merged && !tab_address_sidebar) {
    bounds.Inset(gfx::Insets::VH(0, 6));
  }
  // Combined sidebars rest as native tab rows. Custom standalone address
  // placements keep their existing field or shortcut, as does the top bar.
  const bool show = inline_field || editing;
  command_popup_visible_ = new_tab_edit || (tab_address_sidebar && editing);
  if (editing && !inline_field && side_dock && !tab_address_sidebar &&
      !new_tab_edit) {
    // Keyboard editing in an icon rail remains attached to the sidebar.
    auto dock_bounds = views::View::ConvertRectToTarget(
        side_dock, browser_, side_dock->GetLocalBounds());
    const int field_width = std::min(280, client_bounds.width());
    const int field_height = std::min(
        anchor ? anchor->parent()->height() : 40, client_bounds.height());
    const int x = side_dock == docks_.at(noventra::DockEdge::kRight)
                      ? dock_bounds.right() - field_width
                      : dock_bounds.x();
    bounds = gfx::Rect(
        std::clamp(x, client_bounds.x(), client_bounds.right() - field_width),
        std::clamp(bounds.y(), client_bounds.y(),
                   std::max(client_bounds.y(),
                            client_bounds.bottom() - field_height)),
        field_width, field_height);
  } else if ((tab_address_sidebar && editing) || (editing && !inline_field)) {
    gfx::Rect available = client_bounds;
    for (const auto& geometry : dock_geometry_) {
      switch (geometry.edge) {
        case noventra::DockEdge::kTop:
          available.Inset(gfx::Insets::TLBR(
              std::max(0, geometry.bounds.bottom() - available.y()), 0, 0, 0));
          break;
        case noventra::DockEdge::kBottom:
          available.Inset(gfx::Insets::TLBR(
              0, 0, std::max(0, available.bottom() - geometry.bounds.y()), 0));
          break;
        case noventra::DockEdge::kLeft:
          available.Inset(gfx::Insets::TLBR(
              0, std::max(0, geometry.bounds.right() - available.x()), 0, 0));
          break;
        case noventra::DockEdge::kRight:
          available.Inset(gfx::Insets::TLBR(
              0, 0, 0, std::max(0, available.right() - geometry.bounds.x())));
          break;
      }
    }
    const int caption_bottom = std::max(leading_caption_bounds_.bottom(),
                                        trailing_caption_bounds_.bottom());
    available.Inset(gfx::Insets::TLBR(
        std::max(0, caption_bottom - available.y()), 0, 0, 0));
    available.Inset(gfx::Insets::VH(8, 16));
    // Keep the real omnibox and native suggestions in the safe page area.
    // Combined sidebars use a centered command popup at every width. Retain
    // its hidden geometry so native page-action bubbles keep their anchor.
    constexpr int kMaximumFloatingAddressWidth = 640;
    const int field_width =
        std::min(kMaximumFloatingAddressWidth, available.width());
    const int field_height =
        std::min(available.height(),
                 command_popup_visible_
                     ? LocationBarView::kNoventraCommandPopupHeight
                     : GetLayoutConstant(LayoutConstant::kLocationBarHeight) +
                           GetLayoutInsets(TOOLBAR_INTERIOR_MARGIN).height());
    bounds =
        gfx::Rect(available.x() + (available.width() - field_width) / 2,
                  available.y() + (command_popup_visible_
                                       ? (available.height() - field_height) / 2
                                       : 0),
                  field_width, field_height);
  }
  SetAddressAnchorHidden(merged && show);
  field->SetNoventraSidebarInline(side_dock != nullptr &&
                                  !command_popup_visible_);
  field->SetNoventraCommandPopup(command_popup_visible_);
  address_pane_->SetCommandPopup(command_popup_visible_);
  address_pane_->SetProperty(kNoventraDockUnifiedAddress,
                             tab_address_sidebar && !editing);
  const auto physical_bounds = bounds;
  bounds.set_x(browser_->GetMirroredXForRect(bounds));
  const bool shortcut = !show && !slot->GetLocalBounds().IsEmpty();
  slot->SetFocusBehavior(shortcut ? views::View::FocusBehavior::ALWAYS
                                  : views::View::FocusBehavior::NEVER);
  slot->GetViewAccessibility().SetIsIgnored(!shortcut);
  slot->layer()->SetOpacity(show ? 0.f : 1.f);
  address_pane_->SetVisible(show);
  address_pane_->SetPaintGlassMaterial(!inline_field);
  address_pane_->SetFloatingGlass(!inline_field);
  address_pane_->SetBoundsRect(bounds);
  UpdateCaptionClip(address_pane_, physical_bounds);
}
bool NoventraDockController::UpdateRevealAnimation() {
  // Reuse measured geometry while moving overlays. Structural changes and
  // dirty native children still fall back to the normal BrowserView layout.
  if (!active_ || !has_layout_ || browser_->needs_layout() ||
      browser_->IsFullscreen()) return false;
  for (const auto& geometry : dock_geometry_)
    if (docks_.at(geometry.edge)->needs_layout()) return false;
  for (const auto& geometry : dock_geometry_) PositionDock(geometry);
  UpdateAddressPaneBounds();
  return true;
}

bool NoventraDockController::ContainsPoint(const gfx::Point& point) const {
  auto hit = [&](NoventraDockView* dock) {
    if (!dock || !dock->GetVisible()) return false;
    auto local = point;
    views::View::ConvertPointToTarget(browser_, dock, &local);
    return dock->HitTestPoint(local);
  };
  if (hit(address_pane_)) return true;
  for (const auto& [edge, dock] : docks_)
    if (hit(dock)) return true;
  return false;
}
bool NoventraDockController::IsVisibleCaptionPoint(const gfx::Point& point) const {
  return active_ && !browser_->IsFullscreen() && browser_->GetTopChromeRevealRatio() > 0.f &&
      (leading_caption_bounds_.Contains(point) || trailing_caption_bounds_.Contains(point));
}

std::vector<const views::View*> NoventraDockController::GetChromeRoots(
    bool side) const {
  std::vector<const views::View*> roots;
  for (const auto& [edge, dock] : docks_) {
    const bool side_dock = edge == noventra::DockEdge::kLeft ||
                           edge == noventra::DockEdge::kRight;
    if (side_dock == side) roots.push_back(dock.get());
  }
  if (address_pane_ && IsControlOnSide("address") == side)
    roots.push_back(address_pane_);
  return roots;
}

bool NoventraDockController::IsControlOnSide(std::string_view id) const {
  const auto control = controls_.find(std::string(id));
  if (control == controls_.end()) return false;
  for (const auto edge : {noventra::DockEdge::kLeft,
                          noventra::DockEdge::kRight}) {
    if (docks_.at(edge)->Contains(control->second)) return true;
  }
  return false;
}

bool NoventraDockController::HasFocusedControl(bool side) const {
  auto* focus = browser_->GetFocusManager()->GetFocusedView();
  for (const auto* root : GetChromeRoots(side)) {
    if (root->Contains(focus)) return true;
  }
  return address_requested_ && IsControlOnSide("address") == side;
}

bool NoventraDockController::HasOpenPopup(bool side) const {
  // Wayland popups can be separate native surfaces rather than owned widgets.
  // Query the view that owns the popup without walking every tab's subtree.
  for (const auto& [id, view] : controls_) {
    if (const auto* library = views::AsViewClass<NoventraBookmarksView>(view);
        library && library->HasOpenPopup() && IsControlOnSide(id) == side) {
      return true;
    }
  }
  return false;
}

bool NoventraDockController::ShouldReveal(const gfx::Point& point,
                                          bool side) const {
  if (address_pane_ && address_pane_->GetVisible() &&
      IsControlOnSide("address") == side &&
      views::View::ConvertRectToTarget(address_pane_, browser_,
                                       address_pane_->GetLocalBounds())
          .Contains(point)) return true;
  for (const auto& [edge, dock] : docks_) {
    const bool side_dock = edge == noventra::DockEdge::kLeft ||
                           edge == noventra::DockEdge::kRight;
    if (side_dock != side || !dock->HasVisibleControls()) continue;
    if (dock->GetVisible() && views::View::ConvertRectToTarget(
            dock, browser_, dock->GetLocalBounds()).Contains(point)) return true;
    // An edge should be easy to acquire without requiring a three-pixel aim.
    // Keep the browser's existing top/side precedence at shared corners.
    constexpr int kEdgeTrigger = 6;
    if ((edge == noventra::DockEdge::kTop && point.y() < kEdgeTrigger) ||
        (edge == noventra::DockEdge::kBottom &&
         point.y() >= browser_->height() - kEdgeTrigger) ||
        (edge == noventra::DockEdge::kLeft && point.x() < kEdgeTrigger) ||
        (edge == noventra::DockEdge::kRight &&
         point.x() >= browser_->width() - kEdgeTrigger)) return true;
  }
  return !side && IsVisibleCaptionPoint(point);
}
void NoventraDockController::AppendAccessiblePanes(std::vector<views::View*>& panes) const {
  if (address_pane_ && address_pane_->GetVisible()) panes.push_back(address_pane_);
  for (const auto& [edge, dock] : docks_)
    if (dock->GetVisible()) panes.push_back(dock);
}
bool NoventraDockController::ShowNewTabPopup(BrowserView* browser) {
  auto* controller = browser ? browser->noventra_dock_controller() : nullptr;
  if (!controller || !controller->active_ || !controller->address_pane_ ||
      !browser->GetActiveWebContents()) {
    return false;
  }
  if (controller->address_edit_intent_ == AddressEditIntent::kNewTab &&
      controller->command_popup_visible_) {
    controller->EndAddressEdit(true);
    return true;
  }
  controller->address_edit_intent_ = AddressEditIntent::kNewTab;
  controller->BeginAddressEdit();
  auto* field = browser->toolbar()->location_bar_view();
  field->FocusLocation(/*is_user_initiated=*/true,
                       /*clear_focus_if_failed=*/true);
  field->GetOmniboxView()->SetUserText(u"", /*update_popup=*/true);
  return true;
}

bool NoventraDockController::OpenEmptyNewTab() {
  if (address_edit_intent_ != AddressEditIntent::kNewTab ||
      !command_popup_visible_) {
    return false;
  }
  EndAddressEdit(true);
  chrome::NewTab(browser_->browser(), NewTabTypes::kNewTabCommand);
  return true;
}

WindowOpenDisposition NoventraDockController::ResolveAddressDisposition(
    WindowOpenDisposition disposition) {
  if (address_edit_intent_ != AddressEditIntent::kNewTab) {
    return disposition;
  }
  if (disposition != WindowOpenDisposition::NEW_BACKGROUND_TAB) {
    address_edit_intent_ = AddressEditIntent::kCurrentTab;
    address_requested_ = false;
    browser_->InvalidateLayout();
  }
  return disposition == WindowOpenDisposition::CURRENT_TAB
             ? WindowOpenDisposition::NEW_FOREGROUND_TAB
             : disposition;
}

void NoventraDockController::BeginAddressEdit() {
  if (!active_ || !address_pane_) return;
  address_requested_ = true;
  if (auto* anchor = address_anchor_observation_.GetSource()) {
    anchor->parent()->ScrollViewToVisible();
  }
  address_pane_->SetVisible(true);
  browser_->InvalidateLayout();
  browser_->DeprecatedLayoutImmediately();
  // A hidden field can retain an empty visible-bounds layer clip. Showing it
  // at the same bounds does not refresh that clip; recalculate after layout.
  if (address_view_->GetClipLayerToVisibleBounds()) {
    address_view_->SetClipLayerToVisibleBounds(false);
    address_view_->SetClipLayerToVisibleBounds(true);
  }
}
void NoventraDockController::EndAddressEdit(bool restore_focus) {
  address_requested_ = false;
  address_edit_intent_ = AddressEditIntent::kCurrentTab;
  if (restore_focus) {
    // Escape closes the whole sidebar editor, including when the first native
    // Escape only dismissed suggestions. Restore the page's resting address.
    if (command_popup_visible_ && address_view_) {
      browser_->toolbar()->location_bar_view()->GetOmniboxView()->RevertAll();
    }
    if (auto* contents = browser_->GetActiveWebContents()) {
      contents->Focus();
    } else if (focus_manager_) {
      focus_manager_->ClearFocus();
    }
  }
  browser_->InvalidateLayout();
}
void NoventraDockController::OnDidChangeFocus(views::View* before, views::View* now) {
  if (!active_ || !address_view_) return;
  const bool was_editing = address_view_->Contains(before);
  const bool editing = address_view_->Contains(now);
  if (was_editing && !editing && command_popup_visible_) {
    address_edit_intent_ = AddressEditIntent::kCurrentTab;
    browser_->toolbar()->location_bar_view()->GetOmniboxView()->RevertAll();
  }
  bool dock_focus_changed = false;
  if (browser_->IsZenModeEnabledForLayout()) {
    bool was_in_dock = false;
    bool in_dock = false;
    for (const auto& [edge, dock] : docks_) {
      was_in_dock |= dock->Contains(before);
      in_dock |= dock->Contains(now);
    }
    dock_focus_changed = was_in_dock != in_dock ||
                         (was_in_dock && in_dock && before != now);
  }
  if (was_editing != editing) address_requested_ = editing;
  if (was_editing != editing || dock_focus_changed) browser_->InvalidateLayout();
}

void NoventraDockController::OnFocusManagerDestroying(views::FocusManager* manager) {
  focus_manager_ = nullptr;
}
