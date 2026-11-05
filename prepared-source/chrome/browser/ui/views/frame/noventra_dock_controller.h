// Copyright 2026 The Noventra Authors
// SPDX-License-Identifier: GPL-3.0-only
#ifndef CHROME_BROWSER_UI_VIEWS_FRAME_NOVENTRA_DOCK_CONTROLLER_H_
#define CHROME_BROWSER_UI_VIEWS_FRAME_NOVENTRA_DOCK_CONTROLLER_H_
#include <map>
#include <memory>
#include <string_view>
#include <vector>

#include "base/memory/raw_ptr.h"
#include "base/memory/weak_ptr.h"
#include "base/scoped_observation.h"
#include "chrome/browser/performance_manager/public/user_tuning/battery_saver_mode_manager.h"
#include "chrome/browser/ui/noventra/layout_model.h"
#include "components/prefs/pref_change_registrar.h"
#include "ui/base/window_open_disposition.h"
#include "ui/gfx/geometry/rect.h"
#include "ui/views/focus/focus_manager.h"
#include "ui/views/view.h"
#include "ui/views/view_observer.h"
class BrowserView;
class NoventraDockView;
class SkPath;
namespace views { class View; }
class NoventraDockController : public views::FocusChangeListener,
                               public views::ViewObserver,
                               public performance_manager::user_tuning::
                                   BatterySaverModeManager::Observer {
 public:
  explicit NoventraDockController(BrowserView& browser);
  ~NoventraDockController();
  void Apply(const noventra::BrowserLayout& layout);
  std::vector<noventra::LayoutCapability> GetCapabilities() const;
  gfx::Rect Layout(const gfx::Rect& client_bounds);
  bool UpdateRevealAnimation();
  static bool ShowNewTabPopup(BrowserView* browser);
  WindowOpenDisposition ResolveAddressDisposition(
      WindowOpenDisposition disposition);
  void BeginAddressEdit();
  void EndAddressEdit(bool restore_focus);
  void OnDidChangeFocus(views::View* before, views::View* now) override;
  void OnFocusManagerDestroying(views::FocusManager* manager) override;
  void OnViewBoundsChanged(views::View* view) override;
  void OnViewVisibleBoundsChanged(views::View* view) override;
  void OnViewVisibilityChanged(views::View* view,
                               views::View* starting_view,
                               bool visible) override;
  void OnViewHierarchyWillBeDeleted(views::View* view) override;
  void OnBatterySaverActiveChanged(bool is_active) override;
  void OnBatterySaverModeManagerDestroyed() override;
  bool active() const { return active_; }
  bool IsCommandPopupVisible() const { return command_popup_visible_; }
  bool UsesNativeTopNewTabButton() const { return native_top_new_tab_; }
  // Strip resets can run synchronously in another window's pref callback.
  void RestoreEmbeddedBookmarks();
  // Client-owned material, excluding frame-owned caption controls. Coordinates
  // are converted from physical BrowserView space to the supplied frame view.
  SkPath GetFrameMaterialCutout(const views::View* frame) const;
  bool HasSidebarLibrary() const;
  bool ContainsPoint(const gfx::Point& point) const;
  bool IsVisibleCaptionPoint(const gfx::Point& point) const;
  bool ShouldReveal(const gfx::Point& point, bool side) const;
  bool IsControlOnSide(std::string_view id) const;
  bool HasFocusedControl(bool side) const;
  bool HasOpenPopup(bool side) const;
  std::vector<const views::View*> GetChromeRoots(bool side) const;
  void AppendAccessiblePanes(std::vector<views::View*>& panes) const;
 private:
  enum class AddressEditIntent { kCurrentTab, kNewTab };
  struct DockGeometry {
    noventra::DockEdge edge;
    gfx::Rect bounds;
    bool floating;
  };
  void PositionDock(const DockGeometry& geometry);
  void UpdateCaptionClip(NoventraDockView* dock, const gfx::Rect& bounds);
  void UpdateAddressPaneBounds();
  void ObserveAddressAnchor(views::View* anchor);
  void SetAddressAnchorHidden(bool hidden);
  void ScheduleAddressPaneUpdate();
  void ScheduleApply();
  void ApplySaved();
  void RefreshGlassAppearance();
  const raw_ptr<BrowserView> browser_;
  std::map<noventra::DockEdge, raw_ptr<NoventraDockView>> docks_;
  std::map<std::string, raw_ptr<views::View>> controls_;
  std::map<std::string, raw_ptr<views::View>> original_parents_;
  PrefChangeRegistrar registrar_;
  base::ScopedObservation<
      performance_manager::user_tuning::BatterySaverModeManager,
      performance_manager::user_tuning::BatterySaverModeManager::Observer>
      battery_saver_observation_{this};
  raw_ptr<views::FocusManager> focus_manager_ = nullptr;
  raw_ptr<NoventraDockView> address_pane_ = nullptr;
  raw_ptr<views::View> address_view_ = nullptr;
  base::ScopedObservation<views::View, views::ViewObserver>
      address_anchor_observation_{this};
  std::unique_ptr<views::View::ScopedNotifyObserversOnVisibleBoundsChanged>
      address_anchor_notifications_;
  bool address_anchor_hidden_ = false;
  bool address_anchor_layer_created_ = false;
  float address_anchor_opacity_ = 1.f;
  bool address_update_pending_ = false;
  gfx::Rect last_client_bounds_;
  gfx::Rect leading_caption_bounds_;
  gfx::Rect trailing_caption_bounds_;
  std::vector<DockGeometry> dock_geometry_;
  bool force_reveal_top_ = false;
  bool force_reveal_side_ = false;
  bool has_layout_ = false;
  bool address_requested_ = false;
  AddressEditIntent address_edit_intent_ = AddressEditIntent::kCurrentTab;
  bool command_popup_visible_ = false;
  bool native_top_new_tab_ = false;
  bool active_ = false;
  bool pending_ = false;
  base::WeakPtrFactory<NoventraDockController> weak_factory_{this};
};
#endif
