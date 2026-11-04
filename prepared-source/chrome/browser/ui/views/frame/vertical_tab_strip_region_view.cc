#include "chrome/app/chrome_command_ids.h"
#include "chrome/browser/ui/noventra/incognito_space_host.h"
#include "chrome/browser/ui/noventra/window_mirror_service.h"
#include "chrome/browser/ui/noventra_commands.h"
#include "chrome/browser/ui/toolbar/app_menu_model.h"
#include "components/prefs/pref_change_registrar.h"
#include "ui/views/background.h"
#include "ui/views/layout/box_layout.h"
// Copyright 2025 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <algorithm>
#include <optional>
#include <utility>

#include "base/callback_list.h"
#include "base/functional/bind.h"
#include "base/functional/callback.h"
#include "base/i18n/number_formatting.h"
#include "base/i18n/rtl.h"
#include "base/metrics/histogram_functions.h"
#include "base/metrics/user_metrics.h"
#include "base/metrics/user_metrics_action.h"
#include "base/numerics/safe_conversions.h"
#include "base/strings/utf_string_conversions.h"
#include "base/task/single_thread_task_runner.h"
#include "base/time/time.h"
#include "cc/paint/paint_flags.h"
#include "chrome/app/vector_icons/vector_icons.h"
#include "chrome/browser/profiles/noventra_account_spaces.h"
#include "chrome/browser/profiles/noventra_space_profile_registry.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/ui/actions/chrome_action_id.h"
#include "chrome/browser/ui/animation/browser_animation_controller.h"
#include "chrome/browser/ui/animation/browser_animation_types.h"
#include "chrome/browser/ui/browser_actions.h"
#include "chrome/browser/ui/browser_commands.h"
#include "chrome/browser/ui/browser_element_identifiers.h"
#include "chrome/browser/ui/browser_window/public/browser_window_features.h"
#include "chrome/browser/ui/browser_window/public/browser_window_interface.h"
#include "chrome/browser/ui/color/chrome_color_id.h"
#include "chrome/browser/ui/layout_constants.h"
#include "chrome/browser/ui/navigator/browser_navigator.h"
#include "chrome/browser/ui/navigator/browser_navigator_params.h"
#include "chrome/browser/ui/omnibox/omnibox_tab_helper.h"
#include "chrome/browser/ui/tabs/features.h"
#include "chrome/browser/ui/tabs/hover_tab_selector.h"
#include "chrome/browser/ui/tabs/tab_group_model.h"
#include "chrome/browser/ui/tabs/tab_strip_model.h"
#include "chrome/browser/ui/tabs/tab_strip_model_observer.h"
#include "chrome/browser/ui/tabs/vertical_tab_strip_state.h"
#include "chrome/browser/ui/tabs/vertical_tab_strip_state_controller.h"
#include "chrome/browser/ui/ui_features.h"
#include "chrome/browser/ui/view_ids.h"
#include "chrome/browser/ui/views/animations/tab_strip_animations.h"
#include "chrome/browser/ui/views/frame/base_tab_strip_region_view.h"
#include "chrome/browser/ui/views/frame/browser_view.h"
#include "chrome/browser/ui/views/frame/custom_corners_background.h"
#include "chrome/browser/ui/views/frame/frame_separator.h"
#include "chrome/browser/ui/views/frame/noventra_dock_controller.h"
#include "chrome/browser/ui/views/frame/noventra_dock_view.h"
#include "chrome/browser/ui/views/frame/safe_invoke/safe_invoke.h"
#include "chrome/browser/ui/views/frame/shadow_frame_view.h"
#include "chrome/browser/ui/views/frame/tab_strip_region_view.h"
#include "chrome/browser/ui/views/frame/vertical_tab_strip_region_view.h"
#include "chrome/browser/ui/views/tabs/common/pinned_tab_container_view.h"
#include "chrome/browser/ui/views/tabs/common/root_tab_collection_node.h"
#include "chrome/browser/ui/views/tabs/common/tab_collection_node.h"
#include "chrome/browser/ui/views/tabs/common/tab_drag_handler.h"
#include "chrome/browser/ui/views/tabs/common/tab_strip_collection_controller.h"
#include "chrome/browser/ui/views/tabs/common/tab_strip_view.h"
#include "chrome/browser/ui/views/tabs/common/tab_view.h"
#include "chrome/browser/ui/views/tabs/common/unpinned_tab_container_view.h"
#include "chrome/browser/ui/views/tabs/organizer/organizer_panel_utils.h"
#include "chrome/browser/ui/views/tabs/shared/drop_arrow.h"
#include "chrome/browser/ui/views/tabs/vertical/vertical_tab_strip_bottom_container.h"
#include "chrome/browser/ui/views/tabs/vertical/vertical_tab_strip_focus_swipe_controller.h"
#include "chrome/browser/ui/web_applications/app_browser_controller.h"
#include "chrome/grit/generated_resources.h"
#include "components/constrained_window/constrained_window_views.h"
#include "components/prefs/pref_service.h"
#include "components/prefs/scoped_user_pref_update.h"
#include "components/tabs/public/tab_group.h"
#include "components/tabs/public/tab_interface.h"
#include "components/vector_icons/vector_icons.h"
#include "ui/base/dragdrop/os_exchange_data.h"
#include "ui/base/interaction/element_tracker.h"
#include "ui/base/l10n/l10n_util.h"
#include "ui/base/metadata/metadata_impl_macros.h"
#include "ui/base/models/dialog_model.h"
#include "ui/base/models/image_model.h"
#include "ui/color/color_provider.h"
#include "ui/compositor/layer.h"
#include "ui/display/screen.h"
#include "ui/events/event.h"
#include "ui/events/keycodes/keyboard_codes.h"
#include "ui/gfx/canvas.h"
#include "ui/gfx/color_palette.h"
#include "ui/gfx/color_utils.h"
#include "ui/gfx/geometry/point.h"
#include "ui/gfx/geometry/vector2d.h"
#include "ui/gfx/paint_vector_icon.h"
#include "ui/menus/simple_menu_model.h"
#include "ui/native_theme/native_theme.h"
#include "ui/views/background.h"
#include "ui/views/controls/button/label_button.h"
#include "ui/views/controls/button/md_text_button.h"
#include "ui/views/controls/focus_ring.h"
#include "ui/views/controls/highlight_path_generator.h"
#include "ui/views/controls/menu/menu_runner.h"
#include "ui/views/controls/resize_area.h"
#include "ui/views/focus/focus_manager.h"
#include "ui/views/interaction/element_tracker_views.h"
#include "ui/views/layout/delegating_layout_manager.h"
#include "ui/views/layout/flex_layout.h"
#include "ui/views/layout/flex_layout_types.h"
#include "ui/views/layout/flex_layout_view.h"
#include "ui/views/layout/layout_types.h"
#include "ui/views/layout/proposed_layout.h"
#include "ui/views/view.h"
#include "ui/views/view_class_properties.h"
#include "ui/views/view_utils.h"

namespace {
DEFINE_LOCAL_ELEMENT_IDENTIFIER_VALUE(kSpaceNameField);

constexpr int kSpaceTargetSize = 24;
constexpr int kSpaceRailRowHeight = 36;
constexpr int kSpaceFooterPadding = 4;
// One workspace target and its own More target fit in a compact row.
constexpr int kSpaceRowMinWidth =
    2 * kSpaceTargetSize + 2 * kSpaceFooterPadding;
constexpr size_t kMaxRailSpaceDots = 4;

enum class SpaceDragPhase { kMove, kDrop, kCancel };

const gfx::VectorIcon* SpaceIcon(const std::string& icon) {
  if (icon == "incognito") {
    return &kIncognitoIcon;
  }
  if (icon == "work") {
    return &vector_icons::kWorkIcon;
  }
  if (icon == "home") {
    return &vector_icons::kHomeIcon;
  }
  if (icon == "code") {
    return &vector_icons::kCodeIcon;
  }
  if (icon == "book") {
    return &kMenuBookIcon;
  }
  if (icon == "globe") {
    return &vector_icons::kGlobeIcon;
  }
  if (icon == "music") {
    return &kQueueMusicIcon;
  }
  if (icon == "star") {
    return &kStarIcon;
  }
  return nullptr;
}

SkColor SpaceColor(const views::View& view,
                   const std::string& color,
                   bool menu = false,
                   bool highlighted = false) {
  const auto* provider = view.GetColorProvider();
  const SkColor foreground = menu ? provider->GetColor(ui::kColorMenuIcon)
                                  : provider->GetColor(kColorToolbarButtonIcon);
  const auto* theme = view.GetNativeTheme();
  if (color == "default" ||
      (theme &&
       theme->forced_colors() != ui::ColorProviderKey::ForcedColors::kNone)) {
    return foreground;
  }
  SkColor accent = foreground;
  if (color == "red") {
    accent = gfx::kGoogleRed600;
  }
  if (color == "orange") {
    accent = gfx::kGoogleOrange600;
  }
  if (color == "yellow") {
    accent = gfx::kGoogleYellow700;
  }
  if (color == "green") {
    accent = gfx::kGoogleGreen600;
  }
  if (color == "teal") {
    accent = gfx::kGoogleCyan600;
  }
  if (color == "blue") {
    accent = gfx::kGoogleBlue600;
  }
  if (color == "purple") {
    accent = gfx::kGooglePurple600;
  }
  if (color == "pink") {
    accent = gfx::kGooglePink600;
  }
  const SkColor background =
      menu ? provider->GetColor(ui::kColorMenuBackground)
           : provider->GetColor(highlighted
                                    ? kColorToolbarBackgroundSubtleEmphasis
                                    : kColorToolbar);
  return color_utils::BlendForMinContrast(accent, background, std::nullopt, 3.f)
      .color;
}

// The glyph stays small; the native button and focus ring retain a full target.
class AccountSpaceDotButton : public views::Button {
  METADATA_HEADER(AccountSpaceDotButton, views::Button)

 public:
  using DragCallback = base::RepeatingCallback<void(const gfx::Point&, SpaceDragPhase)>;

  AccountSpaceDotButton(PressedCallback callback, bool menu,
                        DragCallback drag_callback = {})
      : views::Button(std::move(callback)), menu_(menu),
        drag_callback_(std::move(drag_callback)) {
    views::InstallRoundRectHighlightPathGenerator(this, gfx::Insets(1), 6);
    views::FocusRing::Get(this)->SetOutsetFocusRingDisabled(true);
    if (menu_) {
      GetViewAccessibility().SetHasPopup(ax::mojom::HasPopup::kMenu);
    } else {
      GetViewAccessibility().SetRole(ax::mojom::Role::kTab);
    }
  }

  void UpdateSpace(const std::u16string& name,
                   bool selected,
                   const noventra::SpaceAppearance& appearance) {
    SetAccessibleName(u"Workspace: " + name);
    SetTooltipText(name);
    GetViewAccessibility().SetIsSelected(selected);
    if (selected_ != selected || color_ != appearance.color ||
        icon_ != appearance.icon) {
      selected_ = selected;
      color_ = appearance.color;
      icon_ = appearance.icon;
      SchedulePaint();
    }
  }

  gfx::Size CalculatePreferredSize(const views::SizeBounds&) const override {
    return gfx::Size(kSpaceTargetSize, kSpaceTargetSize);
  }

  void SetDropIndicator(bool vertical, int edge) {
    if (indicator_vertical_ == vertical && indicator_edge_ == edge) return;
    indicator_vertical_ = vertical;
    indicator_edge_ = edge;
    SchedulePaint();
  }

  bool OnMousePressed(const ui::MouseEvent& event) override {
    dragged_ = cancelled_ = false;
    press_ = drag_callback_ && GetEnabled() && event.IsOnlyLeftMouseButton()
        ? std::make_optional(event.location()) : std::nullopt;
    return views::Button::OnMousePressed(event);
  }

  bool OnMouseDragged(const ui::MouseEvent& event) override {
    if (cancelled_) return true;
    if (press_ && !dragged_ && ExceededDragThreshold(event.location() - *press_)) {
      dragged_ = true;
      // Keyboard Escape cancels a held drag; a normal click keeps its usual
      // native focus behavior and only switches on release.
      RequestFocus();
    }
    if (!dragged_) return views::Button::OnMouseDragged(event);
    SetState(STATE_PRESSED);
    drag_callback_.Run(views::View::ConvertPointToScreen(this, event.location()),
                       SpaceDragPhase::kMove);
    return true;
  }

  void OnMouseReleased(const ui::MouseEvent& event) override {
    press_.reset();
    if (dragged_ || cancelled_) {
      const bool drop = !cancelled_;
      dragged_ = cancelled_ = false;
      SetState(STATE_NORMAL);
      drag_callback_.Run(views::View::ConvertPointToScreen(this, event.location()),
                         drop ? SpaceDragPhase::kDrop : SpaceDragPhase::kCancel);
      return;  // A drag must never become a workspace-switch click.
    }
    views::Button::OnMouseReleased(event);
  }

  void OnMouseCaptureLost() override {
    const bool cancel = dragged_ || cancelled_;
    press_.reset();
    dragged_ = cancelled_ = false;
    views::Button::OnMouseCaptureLost();
    if (cancel) drag_callback_.Run(gfx::Point(), SpaceDragPhase::kCancel);
  }

  bool OnKeyPressed(const ui::KeyEvent& event) override {
    if (dragged_ || cancelled_) {
      if (event.key_code() == ui::VKEY_ESCAPE) {
        cancelled_ = true;
        SetState(STATE_NORMAL);
        drag_callback_.Run(gfx::Point(), SpaceDragPhase::kCancel);
      }
      return true;
    }
    return views::Button::OnKeyPressed(event);
  }

  bool OnKeyReleased(const ui::KeyEvent& event) override {
    return dragged_ || cancelled_ || views::Button::OnKeyReleased(event);
  }

  void PaintButtonContents(gfx::Canvas* canvas) override {
    cc::PaintFlags flags;
    flags.setAntiAlias(true);
    if (GetState() == STATE_HOVERED || GetState() == STATE_PRESSED) {
      flags.setColor(GetColorProvider()->GetColor(
          kColorToolbarBackgroundSubtleEmphasis));
      gfx::RectF bounds(GetLocalBounds());
      bounds.Inset(1);
      canvas->DrawRoundRect(bounds, 6, flags);
    }
    const gfx::PointF center(width() / 2.f, height() / 2.f);
    flags.setColor(
        SpaceColor(*this, color_, false,
                   GetState() == STATE_HOVERED || GetState() == STATE_PRESSED));
    if (menu_) {
      for (int offset : {-4, 0, 4}) {
        canvas->DrawCircle(gfx::PointF(center.x() + offset, center.y()), 1, flags);
      }
    } else if (const auto* icon = SpaceIcon(icon_)) {
      canvas->Save();
      canvas->Translate(gfx::Vector2d(width() / 2 - 7, height() / 2 - 7));
      gfx::PaintVectorIcon(canvas, *icon, 14, flags.getColor());
      canvas->Restore();
      if (selected_) {
        // A shape marker retains selection when colors cannot be distinguished.
        canvas->DrawRoundRect(gfx::RectF(center.x() - 4, height() - 3, 8, 2), 1,
                              flags);
      }
    } else {
      if (!selected_) {
        flags.setStyle(cc::PaintFlags::kStroke_Style);
        flags.setStrokeWidth(1.5f);
      }
      // Filled versus hollow also distinguishes the active space without color.
      canvas->DrawCircle(center, selected_ ? 4.f : 2.25f, flags);
    }
    if (indicator_edge_) {
      flags.setStyle(cc::PaintFlags::kFill_Style);
      const int edge = indicator_edge_ < 0 ? 1 : kSpaceTargetSize - 3;
      canvas->DrawRect(indicator_vertical_ ? gfx::Rect(3, edge, 18, 2)
                                           : gfx::Rect(edge, 3, 2, 18), flags);
    }
  }

 private:
  const bool menu_;
  DragCallback drag_callback_;
  std::optional<gfx::Point> press_;
  bool dragged_ = false;
  bool cancelled_ = false;
  bool indicator_vertical_ = false;
  int indicator_edge_ = 0;
  bool selected_ = false;
  std::string color_ = "default";
  std::string icon_ = "default";
};

BEGIN_METADATA(AccountSpaceDotButton)
END_METADATA

// Native sidebar switcher. The model owns every tab; switching only changes
// visibility and selection, so background chats keep their live document state.
class AccountSpacesButton : public views::View, public TabStripModelObserver {
  METADATA_HEADER(AccountSpacesButton, views::View)
 public:
  explicit AccountSpacesButton(BrowserView* browser)
      : browser_(browser) {
    dots_ = AddChildView(std::make_unique<views::View>());
    dots_->GetViewAccessibility().SetRole(ax::mojom::Role::kTabList);
    dots_->GetViewAccessibility().SetName(u"Workspaces");
    more_ = AddChildView(std::make_unique<AccountSpaceDotButton>(
        base::BindRepeating(&AccountSpacesButton::ShowMenu, base::Unretained(this)),
        true));
    registrar_.Init(
        noventra::SpaceCatalogProfile(browser_->GetProfile())->GetPrefs());
    registrar_.Add(noventra::kSpacesPref, base::BindRepeating(&AccountSpacesButton::ScheduleRefresh, base::Unretained(this)));
    registrar_.Add(noventra::kSpaceOrderPref, base::BindRepeating(&AccountSpacesButton::ScheduleRefresh, base::Unretained(this)));
    registrar_.Add(noventra::kArchivedSpacesPref, base::BindRepeating(&AccountSpacesButton::ScheduleRefresh, base::Unretained(this)));
    registrar_.Add(noventra::kSpaceAppearancePref,
                   base::BindRepeating(&AccountSpacesButton::ScheduleRefresh,
                                       base::Unretained(this)));
    if (auto* mirror = noventra::WindowMirrorService::GetForProfile(
            browser_->GetProfile())) {
      mirror_changed_ = mirror->RegisterChanged(base::BindRepeating(
          &AccountSpacesButton::ScheduleRefresh, weak_factory_.GetWeakPtr()));
    }
    incognito_changed_ = noventra::ObserveIncognitoSpaces(base::BindRepeating(
        &AccountSpacesButton::ScheduleRefresh, weak_factory_.GetWeakPtr()));
    ScheduleRefresh();
    Model()->AddObserver(this);
  }
  ~AccountSpacesButton() override = default;
  gfx::Size CalculatePreferredSize(const views::SizeBounds& available) const override {
    const int width = available.width().value_or(180);
    const bool vertical =
        (width < kSpaceRowMinWidth && (!SharedMenu() || HasPrivateSpaces()));
    const int rows =
        vertical
            ? static_cast<int>(VisibleDotCount(width)) + (SharedMenu() ? 0 : 1)
            : 1;
    return gfx::Size(180, vertical ? rows * kSpaceRailRowHeight
                                  : 2 * kSpaceFooterPadding + kSpaceTargetSize);
  }
  void AfterPropertyChange(const void* key, int64_t old_value) override {
    views::View::AfterPropertyChange(key, old_value);
    if (key != kNoventraDockCompactOverflow) {
      return;
    }
    const bool move_focus = SharedMenu() && more_->HasFocus();
    more_->SetVisible(!SharedMenu());
    if (move_focus) {
      if (auto* dock = views::AsViewClass<NoventraDockView>(parent())) {
        if (auto* anchor = dock->CompactMenuAnchor()) {
          anchor->RequestFocus();
        }
      }
    }
    PreferredSizeChanged();
  }
  std::unique_ptr<ui::SimpleMenuModel> CreateMenu(bool shared) {
    return std::make_unique<SpaceMenuModel>(weak_factory_.GetWeakPtr(), shared);
  }
  void Layout(PassKey) override {
    const bool vertical =
        (width() < kSpaceRowMinWidth && (!SharedMenu() || HasPrivateSpaces()));
    const size_t count = VisibleDotCount(width());
    std::vector<size_t> shown;
    const auto active = std::ranges::find(space_ids_, CurrentSpace());
    if (count && active != space_ids_.end()) {
      shown.push_back(active - space_ids_.begin());
    }
    // A live private Space remains directly reachable after returning to a
    // regular Space. Saved spaces may overflow into More; hiding the private
    // shortcut makes a still-running session look as though it was closed.
    for (size_t i = 0; i < space_ids_.size() && shown.size() < count; ++i) {
      if (IsPrivate(space_ids_[i]) && !std::ranges::contains(shown, i)) {
        shown.push_back(i);
      }
    }
    for (size_t i = 0; i < space_ids_.size() && shown.size() < count; ++i) {
      if (!std::ranges::contains(shown, i)) {
        shown.push_back(i);
      }
    }
    std::ranges::sort(shown);
    const int cell_height = vertical ? kSpaceRailRowHeight : kSpaceTargetSize;
    const int cell_inset = (cell_height - kSpaceTargetSize) / 2;
    const int extent = static_cast<int>(count) * cell_height;
    // Keep the standalone menu in the same trailing slot as the shared
    // sidebar menu, independently of the centered workspace dots.
    const int menu_slot_width =
        !vertical && !SharedMenu() ? kSpaceRailRowHeight : 0;
    const int start = std::max(
        0, (width() - (vertical ? kSpaceTargetSize : extent + menu_slot_width)) /
               2);
    dots_->SetBounds(start, vertical ? 0 : kSpaceFooterPadding,
                     vertical ? kSpaceTargetSize : extent,
                     vertical ? extent : kSpaceTargetSize);
    for (size_t i = 0; i < buttons_.size(); ++i) {
      const auto slot = std::ranges::find(shown, i);
      buttons_[i]->SetVisible(slot != shown.end());
      if (slot == shown.end()) {
        buttons_[i]->SetBoundsRect(gfx::Rect());
        continue;
      }
      const int offset = (slot - shown.begin()) * cell_height;
      buttons_[i]->SetBounds(vertical ? 0 : offset,
                             vertical ? offset + cell_inset : 0,
                             kSpaceTargetSize, kSpaceTargetSize);
    }
    if (SharedMenu()) {
      more_->SetBoundsRect(gfx::Rect());
    } else {
      const int menu_inset = (kSpaceRailRowHeight - kSpaceTargetSize) / 2;
      const int more_x =
          vertical ? start
                   : std::max(0, width() - menu_inset - kSpaceTargetSize);
      more_->SetBounds(more_x,
                       vertical ? extent + cell_inset : kSpaceFooterPadding,
                       kSpaceTargetSize, kSpaceTargetSize);
    }
  }

  void OnTabStripModelChanged(TabStripModel*, const TabStripModelChange& change,
                             const TabStripSelectionChange& selection) override {
    // Selecting another tab in the same partition does not change the space
    // controls or which account's tabs are visible. Keep native controls alive
    // instead of reallocating them and invalidating the entire tab subtree.
    // Structural changes still refresh restored-space discovery and visibility.
    if (change.type() == TabStripModelChange::kSelectionOnly &&
        noventra::SpaceId(selection.old_contents) ==
            noventra::SpaceId(selection.new_contents)) {
      return;
    }
    // Do not mutate or recursively lay out during a tab-model notification.
    ScheduleRefresh();
  }

 private:
  bool SharedMenu() const { return GetProperty(kNoventraDockCompactOverflow); }

  // Standalone and browser menus use identical actions, each with its own
  // frozen identity list. The model may outlive an Apply or window teardown;
  // no command dereferences a destroyed space control.
  class SpaceMenuModel : public ui::SimpleMenuModel,
                         public ui::SimpleMenuModel::Delegate {
   public:
    SpaceMenuModel(base::WeakPtr<AccountSpacesButton> owner, bool shared)
        : ui::SimpleMenuModel(this),
          owner_(owner),
          shared_(shared),
          ids_(owner->SpaceIds()),
          current_(owner->CurrentSpace()) {
      for (size_t i = 0; i < ids_.size(); ++i) {
        AddCheckItem(Command(100 + i), owner_->Name(ids_[i]));
      }
      AddSeparator(ui::NORMAL_SEPARATOR);
      AddItem(Command(6), u"New incognito space");
      AddItem(Command(1), u"New account space…");
      AddItem(Command(2), u"Rename current space…");
      AddItem(Command(4), u"Archive current space");
      const auto persistent = noventra::SpaceIds(owner_->RegularBrowser(), true);
      if (std::ranges::any_of(persistent, [&](const auto& id) {
            return noventra::IsSpaceArchived(owner_->RegularBrowser()->GetProfile(), id);
          })) {
        AddItem(Command(5), u"Restore archived spaces…");
      }
      AddItem(Command(3), u"Manage spaces in Settings…");
    }
    bool IsCommandIdChecked(int command) const override {
      command = Action(command);
      return owner_ && command >= 100 &&
             static_cast<size_t>(command - 100) < ids_.size() &&
             ids_[command - 100] == owner_->CurrentSpace();
    }
    bool IsItemForCommandIdDynamic(int command) const override {
      command = Action(command);
      return command >= 100 && static_cast<size_t>(command - 100) < ids_.size();
    }
    std::u16string GetLabelForCommandId(int command) const override {
      command = Action(command);
      return owner_ && command >= 100 &&
                     static_cast<size_t>(command - 100) < ids_.size()
                 ? owner_->Name(ids_[command - 100])
                 : std::u16string();
    }
    ui::ImageModel GetIconForCommandId(int command) const override {
      command = Action(command);
      if (!owner_ || command < 100 ||
          static_cast<size_t>(command - 100) >= ids_.size()) {
        return {};
      }
      const auto appearance = owner_->Appearance(ids_[command - 100]);
      const auto* icon = SpaceIcon(appearance.icon);
      if (!icon && appearance.color == "default") {
        return {};
      }
      return ui::ImageModel::FromVectorIcon(
          icon ? *icon : kCircleFilledIcon,
          SpaceColor(*owner_, appearance.color, true), icon ? 16 : 10);
    }
    bool IsCommandIdEnabled(int command) const override {
      if (!owner_) {
        return false;
      }
      command = Action(command);
      if (command == 6) {
        return chrome::IsCommandEnabled(owner_->RegularBrowser(),
                                        IDC_NEW_INCOGNITO_WINDOW);
      }
      if (command == 1 || command == 2 || command == 4) {
        return !owner_->browser_->GetProfile()->IsOffTheRecord() &&
               !owner_->IsPrivate(current_) &&
               noventra::CanManageSpaces(owner_->browser_->GetProfile()) &&
               (command != 4 || !current_.empty());
      }
      return command == 3 || command == 5 ||
             (command >= 100 &&
              static_cast<size_t>(command - 100) < ids_.size());
    }
    void ExecuteCommand(int command, int) override {
      if (!IsCommandIdEnabled(command)) {
        return;
      }
      command = Action(command);
      if (command >= 100) {
        owner_->Switch(ids_[command - 100]);
      } else if (command == 6) {
        chrome::NewIncognitoWindow(owner_->browser_->GetProfile());
      } else if (command == 3 || command == 5) {
        noventra::ShowIncognitoPresentation(owner_->RegularBrowser());
        chrome::ShowSettingsSubPage(owner_->RegularBrowser(),
                                    "appearance?section=spaces");
      } else if (command == 4) {
        base::SingleThreadTaskRunner::GetCurrentDefault()->PostTask(
            FROM_HERE, base::BindOnce(&AccountSpacesButton::ArchiveSpace,
                                      owner_, current_));
      } else {
        base::SingleThreadTaskRunner::GetCurrentDefault()->PostTask(
            FROM_HERE, base::BindOnce(&AccountSpacesButton::EditName, owner_,
                                      command == 1, current_));
      }
    }

   private:
    int Command(int action) const {
      return shared_ ? AppMenuModel::kMinAccountSpacesCommandId +
                           action * AppMenuModel::kNumUnboundedMenuTypes
                     : action;
    }
    int Action(int command) const {
      if (!shared_) {
        return command;
      }
      const int offset = command - AppMenuModel::kMinAccountSpacesCommandId;
      return offset >= 0 && offset % AppMenuModel::kNumUnboundedMenuTypes == 0
                 ? offset / AppMenuModel::kNumUnboundedMenuTypes
                 : -1;
    }
    const base::WeakPtr<AccountSpacesButton> owner_;
    const bool shared_;
    const std::vector<std::string> ids_;
    const std::string current_;
  };
  size_t VisibleDotCount(int width) const {
    if (SharedMenu() && GetProperty(kNoventraDockCompact) &&
        HasPrivateSpaces()) {
      return std::min(space_ids_.size(), kMaxRailSpaceDots);
    }
    const size_t capacity = SharedMenu()
        ? static_cast<size_t>(std::clamp(
              width / kSpaceTargetSize, 1,
              GetProperty(kNoventraDockCompact) ? 1 : 4))
        : (width < kSpaceRowMinWidth && !SharedMenu())
            ? kMaxRailSpaceDots
            : static_cast<size_t>(std::max(
                  1, (width - 2 * kSpaceFooterPadding) / kSpaceTargetSize - 1));
    return std::min(space_ids_.size(), capacity);
  }

  bool HasPrivateSpaces() const {
    return !noventra::IncognitoSpaces(browser_->browser()).empty();
  }

  BrowserWindowInterface* RegularBrowser() const {
    auto* host = noventra::IncognitoSpaceHost(browser_->browser());
    return host ? host : browser_->browser();
  }

  bool IsPrivate(const std::string& id) const {
    const auto spaces = noventra::IncognitoSpaces(browser_->browser());
    return std::ranges::any_of(spaces, [&](const auto& space) {
      return space.id == id;
    });
  }

  noventra::SpaceAppearance Appearance(const std::string& id) const {
    if (IsPrivate(id)) {
      return {.color = "purple", .icon = "incognito", .theme = "browser"};
    }
    return noventra::GetSpaceAppearance(RegularBrowser()->GetProfile(), id);
  }

  std::vector<std::string> SpaceIds() const {
    return noventra::SpaceIds(browser_->browser());
  }

  void ClearDropIndicators() {
    for (const auto& button : buttons_)
      if (button) button->SetDropIndicator(false, 0);
  }

  void DragSpace(const std::string& id, const gfx::Point& point,
                 SpaceDragPhase phase) {
    ClearDropIndicators();
    if (IsPrivate(id) || browser_->GetProfile()->IsOffTheRecord() ||
        phase == SpaceDragPhase::kCancel ||
        !noventra::CanManageSpaces(browser_->GetProfile())) return;
    const bool vertical =
        (width() < kSpaceRowMinWidth && !SharedMenu());
    for (size_t i = 0; i < buttons_.size(); ++i) {
      auto* target = buttons_[i].get();
      const gfx::Rect bounds = target->GetBoundsInScreen();
      if (!target->GetVisible() || IsPrivate(space_ids_[i]) ||
          space_ids_[i] == id || !bounds.Contains(point))
        continue;
      const bool trailing_edge = vertical
          ? point.y() >= bounds.CenterPoint().y()
          : point.x() >= bounds.CenterPoint().x();
      // Child order is mirrored in RTL, while the marker is painted at the
      // physical pointer edge. Convert only the saved insertion direction.
      const bool after = !vertical && dots_->GetMirrored()
          ? !trailing_edge : trailing_edge;
      if (phase == SpaceDragPhase::kMove) {
        target->SetDropIndicator(vertical, trailing_edge ? 1 : -1);
      } else {
        noventra::MoveSpace(browser_->browser(), id, space_ids_[i], after);
      }
      return;
    }
  }

  void ArchiveSpace(const std::string& id) {
    noventra::SetSpaceArchived(browser_->browser(), id, true);
  }

  class NameDelegate : public ui::DialogModelDelegate {
   public:
    NameDelegate(base::WeakPtr<AccountSpacesButton> owner, bool create,
                 std::string id) : owner_(owner), create_(create), id_(std::move(id)) {}
    bool Save() {
      if (!owner_) return true;
      auto name = base::UTF16ToUTF8(dialog_model()->GetTextfieldByUniqueId(kSpaceNameField)->text());
      if (name.empty() || name.size() > 160) return false;
      if (create_) id_ = base::Uuid::GenerateRandomV4().AsLowercaseString();
      if (!noventra::SetSpaceName(owner_->browser_->browser(), id_, name, create_))
        return false;
      if (create_) owner_->Switch(id_);
      owner_->Refresh();
      return true;
    }
   private:
    base::WeakPtr<AccountSpacesButton> owner_;
    bool create_;
    std::string id_;
  };
  std::string CurrentSpace() const {
    return noventra::CurrentSpaceId(browser_->browser());
  }
  TabStripModel* Model() const { return browser_->browser()->GetTabStripModel(); }
  std::u16string Name(const std::string& id) const {
    for (const auto& space : noventra::IncognitoSpaces(browser_->browser())) {
      if (space.id == id) return space.name;
    }
    const auto* name = RegularBrowser()->GetProfile()->GetPrefs()->GetDict(noventra::kSpacesPref).FindString(id);
    return name ? base::UTF8ToUTF16(*name) : id.empty() ? u"Personal" : u"Account space";
  }
  void ScheduleRefresh() {
    if (refresh_pending_) return;
    refresh_pending_ = true;
    base::SingleThreadTaskRunner::GetCurrentDefault()->PostTask(FROM_HERE,
        base::BindOnce([](base::WeakPtr<AccountSpacesButton> self) {
          if (!self) return;
          self->refresh_pending_ = false;
          self->Refresh();
        }, weak_factory_.GetWeakPtr()));
  }
  void UpdateDots() {
    const auto ids = SpaceIds();
    if (ids != space_ids_) {
      ClearDropIndicators();
      std::vector<raw_ptr<AccountSpaceDotButton>> next;
      for (const auto& id : ids) {
        const auto old = std::ranges::find(space_ids_, id);
        if (old != space_ids_.end()) {
          next.push_back(buttons_[old - space_ids_.begin()]);
        } else {
          next.push_back(dots_->AddChildView(
              std::make_unique<AccountSpaceDotButton>(
                  base::BindRepeating(&AccountSpacesButton::Switch,
                                      weak_factory_.GetWeakPtr(), id), false,
                  base::BindRepeating(&AccountSpacesButton::DragSpace,
                                      weak_factory_.GetWeakPtr(), id))));
        }
      }
      for (size_t i = 0; i < space_ids_.size(); ++i) {
        if (std::ranges::find(ids, space_ids_[i]) != ids.end()) continue;
        auto* removed = buttons_[i].get();
        buttons_[i] = nullptr;
        dots_->RemoveChildViewT(removed);
      }
      buttons_ = std::move(next);
      space_ids_ = ids;
      for (size_t i = 0; i < buttons_.size(); ++i)
        dots_->ReorderChildView(buttons_[i], i);
      PreferredSizeChanged();
    }
    const auto active = CurrentSpace();
    for (size_t i = 0; i < space_ids_.size(); ++i)
      buttons_[i]->UpdateSpace(
          Name(space_ids_[i]), space_ids_[i] == active,
          Appearance(space_ids_[i]));
    more_->SetAccessibleName(u"Account spaces: " + Name(active));
    more_->SetTooltipText(Name(active) + u" — all workspaces, create or manage");
    InvalidateLayout();
  }
  void Refresh() {
    UpdateDots();
    // Space visibility is derived from each tab's authoritative partition.
    auto invalidate = [](auto&& self, views::View* view) -> void {
      if (!view) return;
      for (const auto& child : view->children()) self(self, child.get());
      view->InvalidateLayout();
    };
    invalidate(invalidate, browser_->tab_strip_view());
    browser_->InvalidateLayout();
  }
  void ShowMenu(const ui::Event& event) {
    runner_.reset();
    menu_ = CreateMenu(false);
    runner_ = std::make_unique<views::MenuRunner>(menu_.get(), views::MenuRunner::HAS_MNEMONICS);
    runner_->RunMenuAt(GetWidget(), nullptr, more_->GetBoundsInScreen(),
                      views::MenuAnchorPosition::kTopLeft,
                      event.IsKeyEvent() ? ui::mojom::MenuSourceType::kKeyboard
                                         : ui::mojom::MenuSourceType::kMouse);
  }
  void EditName(bool create, std::string id) {
    auto delegate = std::make_unique<NameDelegate>(weak_factory_.GetWeakPtr(), create, id);
    auto* ptr = delegate.get();
    auto model =
        ui::DialogModel::Builder(std::move(delegate))
            .SetTitle(create ? u"New account space" : u"Rename account space")
            .AddParagraph(ui::DialogModelLabel(
                noventra::ProfileSpacesEnabled()
                    ? u"Each space has its own browser profile, including "
                      u"history, bookmarks, passwords, extensions and website "
                      u"data."
                    : u"Each space has its own cookies, website logins and "
                      u"site storage. History, saved passwords, bookmarks, "
                      u"extensions and browser settings are shared."))
            .AddTextfield(kSpaceNameField, u"Name", create ? u"Work" : Name(id))
            .AddOkButton(
                base::BindRepeating(&NameDelegate::Save, base::Unretained(ptr)))
            .AddCancelButton(base::DoNothing())
            .SetInitiallyFocusedField(kSpaceNameField)
            .Build();
    // A naming form must keep focus while Wayland tears down the space menu.
    // Use the standard browser-modal host rather than a transient popup.
    constrained_window::ShowBrowserModal(
        std::move(model), browser_->GetWidget()->GetNativeWindow());
  }
  void Switch(const std::string& id) {
    noventra::SwitchSpace(browser_->browser(), id);
    ScheduleRefresh();
  }
  bool refresh_pending_ = false;
  raw_ptr<views::View> dots_;
  raw_ptr<AccountSpaceDotButton> more_;
  std::vector<raw_ptr<AccountSpaceDotButton>> buttons_;
  std::vector<std::string> space_ids_;
  PrefChangeRegistrar registrar_;
  raw_ptr<BrowserView> browser_;
  std::unique_ptr<ui::SimpleMenuModel> menu_;
  std::unique_ptr<views::MenuRunner> runner_;
  base::CallbackListSubscription mirror_changed_;
  base::CallbackListSubscription incognito_changed_;
  base::WeakPtrFactory<AccountSpacesButton> weak_factory_{this};
};

BEGIN_METADATA(AccountSpacesButton)
END_METADATA

// The native resize target also serves the dock's full-height hit strip. Keep
// its input behavior and geometry unchanged; only report hover/drag feedback to
// the current ancestor dock, including after a layout reparents the tab strip.
class NoventraSidebarResizeArea : public views::ResizeArea {
  METADATA_HEADER(NoventraSidebarResizeArea, views::ResizeArea)

 public:
  explicit NoventraSidebarResizeArea(views::ResizeAreaDelegate* delegate)
      : views::ResizeArea(delegate) {}

  void OnMouseEntered(const ui::MouseEvent& event) override {
    views::ResizeArea::OnMouseEntered(event);
    hovered_ = true;
    UpdateFeedback();
  }
  void OnMouseMoved(const ui::MouseEvent& event) override {
    views::ResizeArea::OnMouseMoved(event);
    hovered_ = true;
    UpdateFeedback();
  }
  void OnMouseExited(const ui::MouseEvent& event) override {
    views::ResizeArea::OnMouseExited(event);
    hovered_ = false;
    UpdateFeedback();
  }
  bool OnMousePressed(const ui::MouseEvent& event) override {
    const bool handled = views::ResizeArea::OnMousePressed(event);
    if (handled) {
      pressed_ = true;
      UpdateFeedback();
    }
    return handled;
  }
  void OnMouseReleased(const ui::MouseEvent& event) override {
    views::ResizeArea::OnMouseReleased(event);
    pressed_ = false;
    UpdateFeedback();
  }
  void OnMouseCaptureLost() override {
    views::ResizeArea::OnMouseCaptureLost();
    hovered_ = false;
    pressed_ = false;
    UpdateFeedback();
  }
  void OnFocus() override {
    views::ResizeArea::OnFocus();
    RepaintDock();
  }
  void OnBlur() override {
    views::ResizeArea::OnBlur();
    RepaintDock();
  }

 private:
  void UpdateFeedback() {
    const bool active = hovered_ || pressed_;
    if (GetProperty(kNoventraDockResizeActive) == active) {
      return;
    }
    SetProperty(kNoventraDockResizeActive, active);
    RepaintDock();
  }
  void RepaintDock() {
    for (views::View* ancestor = parent(); ancestor;
         ancestor = ancestor->parent()) {
      if (auto* dock = views::AsViewClass<NoventraDockView>(ancestor)) {
        dock->UpdateResizeEdge();
        break;
      }
    }
  }

  bool hovered_ = false;
  bool pressed_ = false;
};

BEGIN_METADATA(NoventraSidebarResizeArea)
END_METADATA

constexpr int kResizeAreaWidth = 8;
constexpr int kCollapsedResizeAreaWidth = 2;
constexpr int kKeyboardResizeWidth = 50;
constexpr int kBottomContainerGap = 2;

// Shadow is used in expand-on-hover mode. Shadow radius and opacity are dynamic
// and set by the layout.
constexpr int kExpandOnHoverShadowElevation = 4;
constexpr ShadowFrameView::ShadowAlpha kExpandOnHoverShadowAlpha(
    {.light_key = 0.3,
     .light_ambient = 0.0,
     .dark_key = 0.6,
     .dark_ambient = 0.0});
}  // namespace

std::unique_ptr<ui::SimpleMenuModel>
VerticalTabStripRegionView::CreateAccountSpacesMenu(views::View* control) {
  auto* spaces = views::AsViewClass<AccountSpacesButton>(control);
  return spaces ? spaces->CreateMenu(true) : nullptr;
}

DEFINE_CLASS_CUSTOM_ELEMENT_EVENT_TYPE(VerticalTabStripRegionView,
                                       kAnimationCompletedEvent);

bool VerticalTabStripRegionView::CanUnifyAddress() const {
  const auto* active = tab_strip_model()->GetActiveTab();
  return active != nullptr;
}

views::View* VerticalTabStripRegionView::GetNoventraAddressAnchor() {
  const auto* active = tab_strip_model()->GetActiveTab();
  auto* tab =
      active
          ? views::AsViewClass<TabView>(GetTabAnchorView(active->GetHandle()))
          : nullptr;
  return tab ? tab->GetNoventraAddressAnchor() : nullptr;
}

void VerticalTabStripRegionView::SetNoventraUnifiedAddress(bool enabled) {
  if (GetProperty(kNoventraDockUnifiedAddress) == enabled) {
    return;
  }
  SetProperty(kNoventraDockUnifiedAddress, enabled);
  for (int i = 0; i < tab_strip_model()->count(); ++i) {
    if (auto* tab = views::AsViewClass<TabView>(GetTabViewAt(i))) {
      tab->RefreshNoventraAddressTitle();
    }
  }
}

VerticalTabStripRegionView::VerticalTabStripRegionView(
    tabs::VerticalTabStripStateController* state_controller,
    actions::ActionItem* root_action_item,
    BrowserView* browser_view)
    : BaseTabStripRegionView(browser_view,
                             root_action_item,
                             TabStripOrientation::kVertical),
      resize_area_width_(kResizeAreaWidth),
      state_controller_(state_controller) {
  // For z-ordering purposes this needs to be on a layer.
  SetPaintToLayer();
  // Because corners may be transparent, this must be set to false.
  layer()->SetFillsBoundsOpaquely(false);

  SetLayoutManager(std::make_unique<views::DelegatingLayoutManager>(this));

  // Create child views.
  auto content_area_view = std::make_unique<views::FlexLayoutView>();
  content_area_view->SetOrientation(views::LayoutOrientation::kVertical);
  content_area_view->SetCollapseMargins(true);
  content_area_view->SetDefault(
      views::kFlexBehaviorKey,
      views::FlexSpecification(views::LayoutOrientation::kVertical,
                               views::MinimumFlexSizeRule::kPreferred,
                               views::MaximumFlexSizeRule::kPreferred));
  content_area_view->SetProperty(
      views::kFlexBehaviorKey,
      views::FlexSpecification(content_area_view->GetDefaultFlexRule()));
  content_area_view_ = AddChildView(std::move(content_area_view));
  content_area_view_->SetPaintToLayer();
  content_area_view_->layer()->SetFillsBoundsOpaquely(false);

  // --------------
  // Views that go into the content area below

  const int region_horizontal_padding =
      GetLayoutConstant(LayoutConstant::kVerticalTabStripHorizontalPadding);

  bottom_button_container_ = content_area_view_->AddChildView(
      std::make_unique<VerticalTabStripBottomContainer>(
          state_controller_, root_action_item, browser_view->browser(),
          base::BindRepeating(
              &VerticalTabStripRegionView::RecordNewTabButtonPressed,
              base::Unretained(this))));
  bottom_button_container_->SetProperty(
      views::kFlexBehaviorKey,
      views::FlexSpecification(views::MinimumFlexSizeRule::kPreferred,
                               views::MaximumFlexSizeRule::kUnbounded));
  bottom_button_container_->SetProperty(
      views::kMarginsKey,
      gfx::Insets::TLBR(kBottomContainerGap, region_horizontal_padding,
                        0, region_horizontal_padding));

  gemini_button_ =
      content_area_view_->AddChildView(std::make_unique<views::View>());
  auto* spaces = content_area_view_->AddChildView(
      std::make_unique<AccountSpacesButton>(browser_view));
  account_spaces_control_ = spaces;
  spaces->SetProperty(views::kMarginsKey, gfx::Insets::VH(2, 8));

  // Views that go into the content area above
  // --------------

  resize_area_ = AddChildView(std::make_unique<NoventraSidebarResizeArea>(this));
  resize_area_->SetProperty(views::kViewIgnoredByLayoutKey, true);
  resize_area_->SetTooltipText(u"Drag to resize sidebar");
  resize_area_->SetFocusBehavior(views::View::FocusBehavior::ACCESSIBLE_ONLY);
  views::FocusRing::Install(resize_area_);
  resize_area_->GetViewAccessibility().SetName(
      l10n_util::GetStringUTF16(IDS_VERTICAL_RESIZE_AREA));
  resize_area_->GetViewAccessibility().SetRole(ax::mojom::Role::kSlider);

  state_controller_->SetDelegate(this);
  target_collapse_state_ = state_controller_->GetState();
  OnCollapseStateChanged(state_controller_->GetCollapseState());
  collapsed_state_changed_subscription_ =
      state_controller_->RegisterOnCollapseChanged(base::BindRepeating(
          &VerticalTabStripRegionView::OnCollapseStateChanged,
          base::Unretained(this)));

  SetProperty(views::kElementIdentifierKey, kTabStripRegionElementId);

  GetViewAccessibility().SetRole(ax::mojom::Role::kTabList);

  SetBackground(std::make_unique<CustomCornersBackground>(
      *this, *browser_view,
      /*primary_color=*/CustomCornersBackground::ToolbarTheme(),
      /*corner_color=*/CustomCornersBackground::ToolbarTheme()));

  shadow_frame_ = AddChildView(std::make_unique<ShadowFrameView>(
      kExpandOnHoverShadowElevation, kExpandOnHoverShadowAlpha));
  shadow_frame_->SetProperty(views::kViewIgnoredByLayoutKey, true);

  if (base::FeatureList::IsEnabled(features::kTabGroupsFocusing)) {
    focus_swipe_controller_ =
        std::make_unique<VerticalTabStripFocusSwipeController>(this);
  }

  SetNotifyEnterExitOnChild(true);
  UpdateInteriorMargin();
}

VerticalTabStripRegionView::~VerticalTabStripRegionView() {
  for (const auto& lock : hover_locks_) {
    lock->ClearRegionViewOnDestruction();
  }
  hover_locks_.clear();

  // Prevent dangling pointer.
  state_controller_->SetDelegate(nullptr);
}

bool VerticalTabStripRegionView::IsAnimatingSize() const {
  return BrowserAnimationController::From(browser_view()->browser())
      ->IsAnimating(TabStripAnimations::kVerticalTabStrip);
}

void VerticalTabStripRegionView::OnAnimationProgressed(
    const BrowserAnimationController* controller,
    BrowserAnimationUpdate status) {
  switch (status) {
    case BrowserAnimationUpdate::kStarted: {
      const auto motion =
          controller->GetCurrentMotion(TabStripAnimations::kVerticalTabStrip);
      if (motion == TabStripAnimations::kExpand) {
        update_state_controller_collapsed_callback_.Run(false);
      }
      hover_card_animation_lock_ =
          hover_card_controller()->GetHoverCardHideLock();
      if (tab_strip_view()) {
        tab_strip_view()->SetIsAnimatingSize(true);
      }
      break;
    }
    case BrowserAnimationUpdate::kProgressed:
      InvalidateLayout();
      break;
    case BrowserAnimationUpdate::kEnded: {
      hover_card_animation_lock_.reset();
      if (tab_strip_view()) {
        tab_strip_view()->SetIsAnimatingSize(false);
      }
      const auto motion =
          controller->GetCurrentMotion(TabStripAnimations::kVerticalTabStrip);
      if (motion == TabStripAnimations::kCollapse ||
          motion == TabStripAnimations::kExpand) {
        views::ElementTrackerViews::GetInstance()->NotifyCustomEvent(
            kAnimationCompletedEvent, this);
      }
      if (motion == TabStripAnimations::kCollapse) {
        update_state_controller_collapsed_callback_.Run(true);
      }
      InvalidateLayout();
      break;
    }
    case BrowserAnimationUpdate::kCanceled:
      hover_card_animation_lock_.reset();
      if (tab_strip_view()) {
        tab_strip_view()->SetIsAnimatingSize(false);
      }

      // If the collapse animation is cancelled by requesting the tabstrip to
      // expand, then we will not receive a BrowserAnimationUpdate::kEnded event
      // which is usually where we update collapsed state of the controller to
      // true.
      // The current motion in the animation controller has already been cleared
      // at this point, so instead check that the target collapse state is
      // expanded. As a result, this also gets executed when cancelling expand
      // on hover animations, but since the collapse state must have already
      // been true in that case, this would be a no-op.
      if (!target_collapse_state_.collapsed) {
        update_state_controller_collapsed_callback_.Run(true);
      }
      break;
  }
}

bool VerticalTabStripRegionView::IsPositionInWindowCaption(
    const gfx::Point& point) {
  if (browser_view()->IsNoventraMirrorTabPoint(this, point)) {
    return false;
  }
  // Check the resize area first, it should always take precedence over other
  // children regardless of order.
  if (IsHitInView(resize_area_, point)) {
    return false;
  }

  if (IsHitInView(bottom_button_container_, point)) {
    gfx::Point point_in_child = point;
    views::View::ConvertPointToTarget(this, bottom_button_container_,
                                      &point_in_child);
    return bottom_button_container_->IsPositionInWindowCaption(point_in_child);
  }

  // For any of the other children, absorb the click as non window caption.
  for (views::View* child : children()) {
    if (child == content_area_view_) {
      for (views::View* grandchild : child->children()) {
        if (grandchild->GetVisible() && IsHitInView(grandchild, point)) {
          return false;
        }
      }
    } else if (child->GetVisible() && IsHitInView(child, point)) {
      return false;
    }
  }

  // If the click doesn't fall under any view,then it counts as window caption.
  return true;
}

void VerticalTabStripRegionView::SetHasLeadingExclusionForLayout(
    bool has_leading_exclusion) {
  if (has_leading_exclusion_ == has_leading_exclusion) {
    return;
  }
  has_leading_exclusion_ = has_leading_exclusion;
  UpdateInteriorMargin();
}

void VerticalTabStripRegionView::SetFloatingStyleForLayout(FloatingStyle style,
                                                           int corner_radius,
                                                           bool below_toolbar) {
  const bool floating = style != FloatingStyle::kNone;
  if (floating_style_ != style || floating_corner_radius_ != corner_radius ||
      floating_below_toolbar_ != below_toolbar) {
    floating_style_ = style;
    floating_corner_radius_ = corner_radius;
    floating_below_toolbar_ = below_toolbar;
    UpdateResizeAreaVisibility();
    SetClipLayerToVisibleBounds(floating);
    UpdateInteriorMargin();
  }

  auto* const background =
      static_cast<CustomCornersBackground*>(this->background());
  if (!background) {
    return;
  }

  CustomCornersBackground::Corners corners;
  CustomCornersBackground::Outline outline;
  const bool rounded = browser_view()->browser()->GetProfile()->GetPrefs()
                           ->GetBoolean("helium.browser.rounded_frame");
  if (floating && rounded) {
    CustomCornersBackground::Corner rounded_corner;
    rounded_corner.type = CustomCornersBackground::CornerType::kRounded;
    rounded_corner.radius = corner_radius;

    // An attached tab strip keeps the side along the window edge square, and
    // below the toolbar its top joins it like the unexpanded strip does.
    const bool round_top = style == FloatingStyle::kDetached || !below_toolbar;
    const bool round_leading = style != FloatingStyle::kAttachedLeading;
    const bool round_trailing = style != FloatingStyle::kAttachedTrailing;
    if (round_leading) {
      corners[CornerOrientation::kBottomLeading] = rounded_corner;
      if (round_top) {
        corners[CornerOrientation::kTopLeading] = rounded_corner;
      }
    }
    if (round_trailing) {
      corners[CornerOrientation::kBottomTrailing] = rounded_corner;
      if (round_top) {
        corners[CornerOrientation::kTopTrailing] = rounded_corner;
      }
    }
  }
  if (floating) {
    outline.color.color = kColorToolbarContentAreaSeparator;
    outline.top = style == FloatingStyle::kDetached || !below_toolbar;
    outline.leading = style != FloatingStyle::kAttachedLeading;
    outline.bottom = true;
    outline.trailing = style != FloatingStyle::kAttachedTrailing;
  }

  background->SetCorners(corners);
  background->SetOutline(outline);
  background->SetPaintWithNativeFrameMaterials(floating);
}

void VerticalTabStripRegionView::AddedToWidget() {
  BaseTabStripRegionView::AddedToWidget();
  paint_as_active_subscription_ =
      GetWidget()->RegisterPaintAsActiveChangedCallback(base::BindRepeating(
          &VerticalTabStripRegionView::UpdateExpandOnHoverState,
          base::Unretained(this), /*hovered=*/std::nullopt));
  if (GetFocusManager()) {
    GetFocusManager()->AddFocusChangeListener(&focus_listener_);
  }
}

void VerticalTabStripRegionView::RemovedFromWidget() {
  if (GetFocusManager()) {
    GetFocusManager()->RemoveFocusChangeListener(&focus_listener_);
  }
  BaseTabStripRegionView::RemovedFromWidget();
}

views::ProposedLayout VerticalTabStripRegionView::CalculateProposedLayout(
    const views::SizeBounds& size_bounds) const {
  views::ProposedLayout layout;

  const gfx::Size tab_strip_size =
      tab_strip_view() ? tab_strip_view()->GetPreferredSize() : gfx::Size();
  const gfx::Size organizer_panel_size =
      organizer_panel_view_ ? organizer_panel_view_->GetPreferredSize()
                            : gfx::Size();

  // Host size will be based on given bounds, or on preferred sizes if not
  // specified.
  layout.host_size = gfx::Size(
      size_bounds.width().value_or(
          std::max(tab_strip_size.width(), organizer_panel_size.width())),
      size_bounds.height().value_or(
          std::max(tab_strip_size.height(), organizer_panel_size.height()) +
          interior_margin_.height()));
  gfx::Rect available(layout.host_size);

  available.Inset(interior_margin_);

  // Just so that there's never a zero-size tabstrip.
  available.set_height(std::max(1, available.height()));

  // Lay out the contents and organizer panels (if present) in the remaining
  // space. A region that is entirely slid out of the visible area is hidden.
  gfx::Rect contents_bounds = available;
  gfx::Rect organizer_bounds = available;
  const int adjustment =
      base::ClampRound(available.width() * organizer_panel_show_percent_);
  contents_bounds.Offset(-adjustment, 0);
  organizer_bounds.Offset(available.width() - adjustment, 0);
  layout.child_layouts.push_back({.child_view = content_area_view_.get(),
                                  .visible = adjustment < available.width(),
                                  .bounds = contents_bounds});
  if (organizer_panel_view_) {
    layout.child_layouts.push_back({.child_view = organizer_panel_view_.get(),
                                    .visible = adjustment > 0,
                                    .bounds = organizer_bounds});
  }

  return layout;
}

void VerticalTabStripRegionView::Layout(PassKey) {
  LayoutSuperclass<BaseTabStripRegionView>(this);

  // When animating the organizer panel in, as soon as it is visible, focus it.
  const bool organizer_panel_visible =
      organizer_panel_view_ && organizer_panel_view_->GetVisible();
  if (!organizer_panel_was_visible_ && organizer_panel_visible) {
    // This may also trigger expand-on-hover.
    GetFocusManager()->SetFocusedView(organizer_panel_view_);
  }
  organizer_panel_was_visible_ = organizer_panel_visible;

  // Manually position the resize area as it overlaps views handled by the flex
  // layout.
  const int resize_area_x = state_controller_->IsTabStripRightAligned()
                                ? 0
                                : width() - resize_area_width_;
  resize_area_->SetBoundsRect(
      gfx::Rect(resize_area_x, 0, resize_area_width_, bounds().height()));
  shadow_frame_->SetBoundsRect(GetLocalBounds());

  // Ensure that we update the drop arrow position so that it does not render in
  // the collapsed state when expand on hover is active.
  if (drop_arrow()) {
    drop_arrow()->SetIndex(drop_arrow()->index());
  }

  // Managed docks reparent this region away from BrowserView, whose normal
  // post-layout clip refresh then no longer runs. Refresh after our own child
  // layout so an old narrow clip cannot hide widened tabs or their controls.
  UpdatePanelClips();
}

views::View* VerticalTabStripRegionView::GetDefaultFocusableChild() {
  if (auto* mirror = browser_view()->GetNoventraMirrorDefaultFocusableChild()) {
    return mirror;
  }
  tabs::TabInterface* active_tab = tab_strip_model()->GetActiveTab();
  if (active_tab) {
    return GetTabAnchorView(active_tab->GetHandle());
  }

  return bottom_button_container_;
}

gfx::Size VerticalTabStripRegionView::GetMinimumSize() const {
  auto min_size = BaseTabStripRegionView::GetMinimumSize();
  min_size.set_width((state_controller_->IsCollapsed() || IsAnimatingSize())
                         ? kCollapsedWidth
                         : kUncollapsedMinWidth);
  return min_size;
}

gfx::Size VerticalTabStripRegionView::CalculatePreferredSize(
    const views::SizeBounds& available_size) const {
  auto size = BaseTabStripRegionView::CalculatePreferredSize(available_size);
  const auto* controller =
      BrowserAnimationController::From(browser_view()->browser());
  const auto motion =
      controller->GetCurrentMotion(TabStripAnimations::kVerticalTabStrip);
  const int expand_amount =
      std::max(0, target_collapse_state_.uncollapsed_width - kCollapsedWidth);
  const double expand_percent =
      *controller->GetCurrentValue(TabStripAnimations::kVerticalTabStrip,
                                   TabStripAnimations::kTabStripWidth);
  size.set_width(kCollapsedWidth +
                 (motion == TabStripAnimations::kExpand
                      ? std::floor<double>
                      : std::ceil<double>)(expand_amount * expand_percent));
  return size;
}

bool VerticalTabStripRegionView::OnKeyPressed(const ui::KeyEvent& event) {
  if (resize_area_->HasFocus()) {
    int resize_amount = 0;

    if (event.key_code() == ui::VKEY_LEFT) {
      resize_amount =
          base::i18n::IsRTL() ? kKeyboardResizeWidth : -kKeyboardResizeWidth;
    } else if (event.key_code() == ui::VKEY_RIGHT) {
      resize_amount =
          base::i18n::IsRTL() ? -kKeyboardResizeWidth : kKeyboardResizeWidth;
    }

    if (resize_amount != 0) {
      const bool right_aligned = state_controller_->IsTabStripRightAligned();
      const int width_delta = right_aligned ? -resize_amount : resize_amount;
      if (width_delta < 0 && width() <= kUncollapsedMinWidth) {
        // Cross the gap between the minimum expanded width and the icon rail
        // in one key press, rather than clamping every press to the minimum.
        resize_amount = (right_aligned ? 1 : -1) *
                        (width() - kCollapsedWidth);
      }
      OnResize(resize_amount, /*done_resizing=*/true);

      float tab_strip_width = width();
      float total_width = GetWidget()->GetRootView()->width();

      int tab_strip_percentage = (tab_strip_width / total_width) * 100;
      int content_percentage = 100 - tab_strip_percentage;

      resize_area_->GetViewAccessibility().AnnounceText(
          l10n_util::GetStringFUTF16(IDS_VERTICAL_TAB_RESIZE_ACCESSIBLE_ALERT,
                                     base::FormatPercent(tab_strip_percentage),
                                     base::FormatPercent(content_percentage)));

      return true;
    }
  }

  return BaseTabStripRegionView::OnKeyPressed(event);
}

void VerticalTabStripRegionView::OnMouseEntered(const ui::MouseEvent& event) {
  if (mouse_exit_timer_.IsRunning()) {
    mouse_exit_timer_.Stop();
    return;
  }
  UpdateExpandOnHoverState(true);
}

void VerticalTabStripRegionView::OnMouseMoved(const ui::MouseEvent& event) {
  if (mouse_exit_timer_.IsRunning()) {
    mouse_exit_timer_.Stop();
  }
  UpdateExpandOnHoverState(true);
}

void VerticalTabStripRegionView::OnMouseExited(const ui::MouseEvent& event) {
  HandleMouseExited();
}

void VerticalTabStripRegionView::SetOrganizerPanelView(
    std::unique_ptr<views::View> panel_view) {
  CHECK(!organizer_panel_view_);
  panel_view->SetVisible(false);
  organizer_panel_was_visible_ = false;
  organizer_panel_view_ =
      AddChildViewAt(std::move(panel_view), *GetIndexOf(content_area_view_));
}

std::unique_ptr<views::View>
VerticalTabStripRegionView::TakeOrganizerPanelView() {
  CHECK(organizer_panel_view_);
  organizer_panel_show_percent_ = 0.0;
  organizer_panel_was_visible_ = false;
  return RemoveChildViewT(std::exchange(organizer_panel_view_, nullptr));
}

bool VerticalTabStripRegionView::HasOrganizerPanelView() const {
  return organizer_panel_view_ != nullptr;
}

void VerticalTabStripRegionView::HandleMouseExited() {
  // On Windows, we get mouse exit events when moving between the caption area
  // and client as well as when we transition between web contents area
  // underneath the expanded on hover overlay to outside it.
#if BUILDFLAG(IS_WIN)
  constexpr base::TimeDelta kMouseExitDebounceTimer = base::Milliseconds(100);
  if (IsMouseHovered()) {
    mouse_exit_timer_.Start(
        FROM_HERE, kMouseExitDebounceTimer,
        base::BindOnce(&VerticalTabStripRegionView::HandleMouseExited,
                       base::Unretained(this)));
    return;
  }
#endif
  UpdateExpandOnHoverState(false);
}

std::unique_ptr<ExpandOnHoverLock>
VerticalTabStripRegionView::GetExpandOnHoverLock(
    ExpandOnHoverLockType lock_type) {
  return std::make_unique<VerticalTabStripExpandOnHoverLock>(this, lock_type);
}

void VerticalTabStripRegionView::HandleDragUpdate(
    const std::optional<BrowserRootView::DropIndex>& index) {
  SetLinkDropArrow(index);
  UpdateExpandOnHoverState(true);
  if (is_expanded_on_hover_ && !link_drag_lock_) {
    link_drag_lock_ =
        GetExpandOnHoverLock(ExpandOnHoverLockType::kKeepCurrentState);
  }
}

void VerticalTabStripRegionView::HandleDragExited() {
  SetLinkDropArrow(std::nullopt);
  link_drag_lock_.reset();
  UpdateExpandOnHoverState(false);
}

gfx::Point VerticalTabStripRegionView::GetLinkDropArrowPosition(
    const BrowserRootView::DropIndex& drop_index,
    DropArrow::Direction* direction) {
  int target_x = GetBoundsInScreen().x();
  int target_y = 0;

  // By default have the arrow outside of the browser window pointing towards
  // the tab strip.
  *direction = base::i18n::IsRTL() ? DropArrow::Direction::kLeft
                                   : DropArrow::Direction::kRight;

  const bool replace_index =
      drop_index.relative_to_index ==
      BrowserRootView::DropIndex::RelativeToIndex::kReplaceIndex;

  if (drop_index.index < tab_strip_model()->count()) {
    tabs::TabInterface* tab =
        tab_strip_model()->GetTabAtIndex(drop_index.index);
    views::View* target_view = GetTabViewAt(drop_index.index);
    if (!target_view) {
      return GetBoundsInScreen().origin();
    }

    // In the expanded view, pinned and split tabs will have an arrow pointing
    // down at the tab. We don't support dropping a tab in the middle of a
    // split view.
    if ((tab->IsPinned() || (tab->IsSplit() && replace_index)) &&
        !state_controller_->IsCollapsed()) {
      if (replace_index) {
        target_x = target_view->GetBoundsInScreen().CenterPoint().x();
      } else {
        target_x = base::i18n::IsRTL()
                       ? target_view->GetBoundsInScreen().right()
                       : target_view->GetBoundsInScreen().x();
      }
      target_y = target_view->GetBoundsInScreen().y();
      *direction = DropArrow::Direction::kDown;
    } else if (replace_index) {
      // When a tab is being replaced, point at the middle of the tab.
      target_y = target_view->GetBoundsInScreen().CenterPoint().y();
    } else {
      // Otherwise, we are pointing at the slot before the tab.
      if (IsDropBeforeGroupHeader(drop_index, tab)) {
        // Drop before the group header.
        views::View* header_view = GetGroupHeaderView(tab->GetGroup().value());
        target_y =
            (header_view ? header_view : target_view)->GetBoundsInScreen().y();
      } else {
        target_y = target_view->GetBoundsInScreen().y();
      }
    }
  } else {
    // Drop at the end of the unpinned container.
    if (auto* unpinned_container = GetUnpinnedTabsContainer()) {
      target_y = unpinned_container->GetBoundsInScreen().bottom();
    } else {
      target_y = tab_strip_view()->GetBoundsInScreen().bottom();
    }
  }

  // In RTL, shift `target_x` to the other side of the tab strip.
  if (*direction == DropArrow::Direction::kLeft) {
    target_x += GetBoundsInScreen().width();
  }

  return gfx::Point(target_x, target_y);
}

views::View* VerticalTabStripRegionView::GetNoventraResizeTarget() const {
  return resize_area_->IsDrawn() && resize_area_->GetEnabled()
             ? resize_area_.get()
             : nullptr;
}

void VerticalTabStripRegionView::OnResize(int resize_amount,
                                          bool done_resizing) {
  CHECK(tab_strip_view());
  if (!starting_width_on_resize_.has_value()) {
    starting_width_on_resize_ = width();
    state_controller_->SetIsResizing(true);
  }
  bool previously_collapsed = target_collapse_state_.collapsed;
  const int resize_delta = state_controller_->IsTabStripRightAligned()
                               ? -resize_amount
                               : resize_amount;
  const int proposed_width = starting_width_on_resize_.value() + resize_delta;
  if (done_resizing) {
    starting_width_on_resize_ = std::nullopt;
    state_controller_->SetIsResizing(false);
  }

  tabs::VerticalTabStripState new_state;
  new_state.collapsed = proposed_width <= kCollapsedWidth;
  new_state.uncollapsed_width = target_collapse_state_.uncollapsed_width;
  if (!new_state.collapsed) {
    new_state.uncollapsed_width =
        std::clamp(proposed_width, kUncollapsedMinWidth, kUncollapsedMaxWidth);
    if (done_resizing) {
      // We only want to save the uncollapsed width to the state controller if
      // the user has lifted their mouse, otherwise dragging the resize area to
      // collapse will cause a subsequent collapse button click to only expand
      // to the minimum expanded width, and not to the starting width of the
      // drag-to-collapse operation.
      state_controller_->SetUncollapsedWidth(new_state.uncollapsed_width);
    }
  } else if (done_resizing) {
    // If we are done resizing while in collapsed state, update the target state
    // to use the uncollapsed width from the state controller.
    new_state.uncollapsed_width = state_controller_->GetUncollapsedWidth();
  }

  if (done_resizing) {
    UpdateResizeAreaVisibility();
    base::RecordAction(base::UserMetricsAction(
        new_state.collapsed ? "VerticalTabs_TabStrip_ResizeToCollapsed"
                            : "VerticalTabs_TabStrip_ResizeToUncollapsed"));
    base::UmaHistogramCounts1000(
        "Tabs.VerticalTabs.TabStripSize",
        new_state.collapsed ? kCollapsedWidth : new_state.uncollapsed_width);
  }

  target_collapse_state_ = new_state;

  if (previously_collapsed != target_collapse_state_.collapsed) {
    state_controller_->RequestCollapse(target_collapse_state_.collapsed);
  }

  // Direct manipulation must follow the pointer, including when it crosses
  // the collapse threshold or interrupts an existing button animation. Finish
  // that motion synchronously so its interpolated width cannot trail the drag.
  // Keep the controller's normal completion notifications and saved state.
  if (IsAnimatingSize()) {
    BrowserAnimationController::From(browser_view()->browser())
        ->Reset(TabStripAnimations::kVerticalTabStrip,
                target_collapse_state_.collapsed ? TabStripAnimations::kCollapse
                                                 : TabStripAnimations::kExpand);
  }
  // Reset() completes animation callbacks, which clear this flag. A drag is
  // still in progress until release, so restore the direct-resize state last.
  tab_strip_view()->SetIsAnimatingSize(!done_resizing);
  InvalidateLayout();
}

void VerticalTabStripRegionView::SetCollapsedStateUpdatedCallback(
    base::RepeatingCallback<void(bool)> callback) {
  update_state_controller_collapsed_callback_ = std::move(callback);
}

bool VerticalTabStripRegionView::IsCollapsing() {
  return BrowserAnimationController::From(browser_view()->browser())
             ->GetCurrentMotion(TabStripAnimations::kVerticalTabStrip) ==
         TabStripAnimations::kCollapse;
}

void VerticalTabStripRegionView::RequestCollapse(bool collapse) {
  target_collapse_state_.collapsed = collapse;
  if (collapse) {
    suppress_expand_on_hover_ = IsCollapseButtonHovered();
    ResetExpandOnHoverTimers();
  } else {
    suppress_expand_on_hover_ = false;
  }
  // Do not trigger the animation before tab_strip_view() is set, as the region
  // view only subscribes to animation updates once tab_strip_view() has been
  // attached. target_collapse_state_ is still set so that when
  // SetTabStripView() is eventually called, the region view and all child
  // views initialize directly in the target collapse state.
  if (!tab_strip_view()) {
    return;
  }
  const auto motion =
      collapse ? TabStripAnimations::kCollapse : TabStripAnimations::kExpand;
  BrowserAnimationController::From(browser_view()->browser())
      ->Start(TabStripAnimations::kVerticalTabStrip, motion);
}

void VerticalTabStripRegionView::SetOrganizerPanelShowPercent(double percent) {
  if (organizer_panel_show_percent_ == percent) {
    return;
  }
  organizer_panel_show_percent_ = percent;
  InvalidateLayout(/*avoid_propagate_during_layout=*/true);
}

void VerticalTabStripRegionView::UpdatePanelClips() {
  auto* const bg = background()->AsA<CustomCornersBackground>();
  if (organizer_panel_view_) {
    bg->ClipViewToBackground(organizer_panel_view_);
  }
  bg->ClipViewToBackground(content_area_view_);
}

VerticalTabStripRegionView::RegionViewFocusListener::RegionViewFocusListener(
    VerticalTabStripRegionView* region_view)
    : region_view_(region_view) {}

void VerticalTabStripRegionView::RegionViewFocusListener::OnDidChangeFocus(
    views::View* focused_before,
    views::View* focused_now) {
  if ((focused_before && region_view_->Contains(focused_before)) ||
      (focused_now && region_view_->Contains(focused_now))) {
    region_view_->UpdateExpandOnHoverState();
  }
}

VerticalTabStripRegionView::ClickEventHandler::ClickEventHandler(
    VerticalTabStripRegionView* region_view)
    : region_view_(region_view) {}

void VerticalTabStripRegionView::ClickEventHandler::OnMouseEvent(
    ui::MouseEvent* event) {
  if (event->type() == ui::EventType::kMousePressed &&
      region_view_->state_controller_->IsExpandOnHoverEnabled() &&
      !region_view_->is_expanded_on_hover()) {
    region_view_->RestartExpandOnHoverTimer(
        tabs::kVerticalTabsExpandOnHoverClickDelay.Get());
  }
}

void VerticalTabStripRegionView::AddTabStripView(
    std::unique_ptr<views::View> view) {
  view->SetProperty(
      views::kFlexBehaviorKey,
      views::FlexSpecification(views::MinimumFlexSizeRule::kScaleToMinimum,
                               views::MaximumFlexSizeRule::kPreferred));
  view->SetProperty(views::kMarginsKey,
                    gfx::Insets::TLBR(0, 0, kBottomContainerGap, 0));

  views::View* const content_parent =
      content_area_view_ ? content_area_view_.get() : this;
  std::optional<size_t> bottom_container_index =
      content_parent->GetIndexOf(bottom_button_container_);
  CHECK(bottom_container_index.has_value());
  content_parent->AddChildViewAt(std::move(view),
                                 bottom_container_index.value());

  // Pre-set the animation values to the appropriate state.
  auto* const animation_controller =
      BrowserAnimationController::From(browser_view()->browser());
  animation_controller->Reset(TabStripAnimations::kVerticalTabStrip,
                              target_collapse_state_.collapsed
                                  ? TabStripAnimations::kCollapse
                                  : TabStripAnimations::kExpand);

  on_animation_update_subscription_ = animation_controller->Subscribe(
      TabStripAnimations::kVerticalTabStrip,
      base::BindRepeating(&VerticalTabStripRegionView::OnAnimationProgressed,
                          base::Unretained(this)));

  expand_on_hover_enabled_changed_subscription_ =
      state_controller_->RegisterOnExpandOnHoverEnabledChanged(
          base::BindRepeating(
              &VerticalTabStripRegionView::OnExpandOnHoverEnabledChanged,
              base::Unretained(this)));

  OnCollapseStateChanged(state_controller_->GetCollapseState());
}

std::unique_ptr<views::View> VerticalTabStripRegionView::RemoveTabStripView(
    views::View* view) {
  if (auto* dock = browser_view()->noventra_dock_controller()) {
    dock->RestoreEmbeddedBookmarks();
  }
  on_animation_update_subscription_.reset();
  expand_on_hover_enabled_changed_subscription_.reset();
  omnibox_tab_helper_observation_.Reset();
  ResetExpandOnHoverTimers();
  is_expanded_on_hover_ = false;
  return BaseTabStripRegionView::RemoveTabStripView(view);
}

void VerticalTabStripRegionView::UpdateInteriorMargin() {
  const int padding = GetLayoutConstant(
      LayoutConstant::kVerticalTabStripUncollapsedVerticalPadding);
  int top_padding = 0;
  BrowserView* const browser = browser_view();
  if (browser && browser->IsZenModeEnabledForLayout()) {
    const float top_chrome_ratio = browser->GetTopChromeRevealRatio();
    const bool top_chrome_visible_or_pinned =
        top_chrome_ratio > 0.0f || browser->IsZenModeTopChromePinned();
    const float ratio =
        !browser->IsZenModeSideChromePinned() || !top_chrome_visible_or_pinned
            ? 1.0f
            : 1.0f - top_chrome_ratio;
    top_padding = base::ClampRound(
        GetLayoutConstant(LayoutConstant::kVerticalTabCornerRadius) * ratio);
  }

  interior_margin_ = gfx::Insets::TLBR(top_padding, 0, padding, 0);
  InvalidateLayout();
}

void VerticalTabStripRegionView::OnCollapseStateChanged(
    tabs::VerticalTabStripCollapseState state) {
  // Width presets update the controller without going through OnResize().
  // Refresh the local layout target as well as the persisted session state.
  // During a drag, OnResize owns the intermediate width until mouse release.
  const int width = state_controller_->GetUncollapsedWidth();
  if (!state_controller_->IsResizing() &&
      target_collapse_state_.uncollapsed_width != width) {
    target_collapse_state_.uncollapsed_width = width;
    InvalidateLayout();
  }

  // Apply padding immediately at the start of the animation by including
  // the collapsing state.
  bool collapsed = state != tabs::VerticalTabStripCollapseState::kExpanded;

  UpdateResizeAreaVisibility();

  resize_area_width_ = collapsed ? kCollapsedResizeAreaWidth : kResizeAreaWidth;

  if (tab_strip_view()) {
    tab_strip_view()->SetCollapsedState(collapsed);
  }

  if (state == tabs::VerticalTabStripCollapseState::kExpanded ||
      state == tabs::VerticalTabStripCollapseState::kCollapsed) {
    UpdateExpandOnHoverState();
  }
}

void VerticalTabStripRegionView::UpdateResizeAreaVisibility() {
  // Keep the native resize handle available in Noventra floating layouts.
  const bool is_fully_expanded =
      state_controller_->GetCollapseState() ==
      tabs::VerticalTabStripCollapseState::kExpanded;
  const bool can_resize_collapsed_tab_strip =
      !state_controller_->IsExpandOnHoverEnabled() ||
      resize_area_->is_resizing();

  resize_area_->SetVisible(is_fully_expanded || can_resize_collapsed_tab_strip);
}

bool VerticalTabStripRegionView::IsCollapseButtonHovered() const {
  const auto* button = browser_view()->GetViewByID(
      VIEW_ID_COLLAPSE_VERTICAL_TABS_BUTTON);
  return button && button->GetVisible() && button->IsMouseHovered();
}

gfx::Rect VerticalTabStripRegionView::GetTabStripDraggableBounds() const {
  // Tabs should be draggable from the top of the tab strip to the bottom of the
  // tab strip's max size, saving space for the bottom button container and
  // padding.
  gfx::Rect tab_strip_draggable_bounds = tab_strip_view()->GetBoundsInScreen();
  tab_strip_draggable_bounds.set_height(
      GetBoundsInScreen().bottom() -
      bottom_button_container_->GetMinimumSize().height() -
      GetLayoutConstant(
          LayoutConstant::kVerticalTabStripUncollapsedVerticalPadding) -
      tab_strip_draggable_bounds.y());
  return tab_strip_draggable_bounds;
}

void VerticalTabStripRegionView::OnExpandOnHoverEnabledChanged(
    bool /*enabled*/) {
  UpdateResizeAreaVisibility();
  UpdateExpandOnHoverState();
}

void VerticalTabStripRegionView::UpdateExpandOnHoverState(
    std::optional<bool> hovered) {
  if (!BrowserAnimationController::From(browser_view()->browser())) {
    ResetExpandOnHoverTimers();
    return;
  }

  // If not collapsed, then we shouldn't be in or entering the expand on hover
  // state.
  if (!state_controller_->ShouldDisplayVerticalTabs() ||
      !state_controller_->IsCollapsed()) {
    ResetExpandOnHoverTimers();
    is_expanded_on_hover_ = false;
    return;
  }

  if (suppress_expand_on_hover_ && !IsCollapseButtonHovered()) {
    suppress_expand_on_hover_ = false;
  }

  // If the force collapse lock is held, collapse the tab strip.
  if (force_collapse_lock_count_ > 0) {
    if (is_expanded_on_hover_) {
      AnimateExpandOnHover(/*expand=*/false);
    }
    return;
  }

  // If the current state lock is held, reset the timers and wait.
  if (keep_current_state_lock_count_ > 0) {
    ResetExpandOnHoverTimers();
    return;
  }

  // If the keep expanded lock is held, expand the tab strip.
  if (keep_expanded_lock_count_ > 0) {
    if (!is_expanded_on_hover_) {
      AnimateExpandOnHover(/*expand=*/true);
    }
    return;
  }

  // If the window becomes inactive, then we should not enter the expand on
  // hover state or exit it if already expanded. We evaluate this after the
  // locks because IsFrameActive can also return false when a WebUI bubble is
  // open.
  if (GetWidget() && !GetWidget()->ShouldPaintAsActive()) {
    if (is_expanded_on_hover_) {
      AnimateExpandOnHover(/*expand=*/false);
    }
    return;
  }

  const bool should_expand =
      !suppress_expand_on_hover_ &&
      state_controller_->IsExpandOnHoverEnabled() &&
      (hovered.value_or(IsMouseHovered()) ||
       (GetFocusManager() && Contains(GetFocusManager()->GetFocusedView())));

  if (!should_expand) {
    if (is_expanded_on_hover_ && keep_current_state_lock_count_ == 0) {
      AnimateExpandOnHover(/*expand=*/false);
    } else {
      ResetExpandOnHoverTimers();
    }
    return;
  }

  // If the region is already expanded, do nothing.
  if (is_expanded_on_hover_) {
    return;
  }

  if (!hover_card_animation_lock_) {
    hover_card_animation_lock_ =
        hover_card_controller()->GetHoverCardHideLock();
  }

  if (tabs::kVerticalTabsExpandOnHoverUseVelocityHeuristic.Get()) {
    CalculateMouseVelocityForExpandOnHover();
  } else if (expand_on_hover_timer_.IsRunning()) {
    // If the timer is already running then we are already waiting to
    // expand, so do nothing.
    return;
  } else {
    expand_on_hover_timer_.Start(
        FROM_HERE, tabs::kVerticalTabsExpandOnHoverDelay.Get(),
        base::BindOnce(&VerticalTabStripRegionView::AnimateExpandOnHover,
                       base::Unretained(this),
                       /*expand=*/true));
    if (tabs::IsExpandOnHoverClickDelayEnabled()) {
      AddPreTargetHandler(&click_handler_);
    }
  }
}

void VerticalTabStripRegionView::RestartExpandOnHoverTimer(
    const base::TimeDelta& delay) {
  if (expand_on_hover_timer_.IsRunning()) {
    expand_on_hover_timer_.Start(
        FROM_HERE, delay,
        base::BindOnce(&VerticalTabStripRegionView::AnimateExpandOnHover,
                       base::Unretained(this),
                       /*expand=*/true));
  }
}

void VerticalTabStripRegionView::OnMouseVelocityHeuristicInterval() {
  const bool should_expand =
      state_controller_->IsExpandOnHoverEnabled() &&
      (IsMouseHovered() ||
       (GetFocusManager() && Contains(GetFocusManager()->GetFocusedView())));

  if (!should_expand || is_expanded_on_hover_) {
    ResetExpandOnHoverTimers();
    return;
  }

  CalculateMouseVelocityForExpandOnHover();
}

void VerticalTabStripRegionView::CalculateMouseVelocityForExpandOnHover() {
  gfx::Point current_point = display::Screen::Get()->GetCursorScreenPoint();
  ConvertPointFromScreen(this, &current_point);

  // If this is the first mouse event within the region, initialize values.
  if (!time_at_expand_on_hover_timer_start_.has_value()) {
    point_at_expand_on_hover_timer_start_ = current_point;
    time_at_expand_on_hover_timer_start_ = base::TimeTicks::Now();
    expand_on_hover_heuristic_samples_ = 1;
    expand_on_hover_heuristic_timer_.Start(
        FROM_HERE,
        tabs::kVerticalTabsExpandOnHoverVelocityHeuristicInterval.Get(),
        base::BindRepeating(
            &VerticalTabStripRegionView::OnMouseVelocityHeuristicInterval,
            base::Unretained(this)));
    return;
  }

  // Reset the timer so it will trigger if a mouse event isn't received in the
  // specified interval.
  expand_on_hover_heuristic_timer_.Reset();

  // Increment the number of samples received.
  expand_on_hover_heuristic_samples_ += 1;

  const int dx = std::abs(current_point.x() -
                          (*point_at_expand_on_hover_timer_start_).x());
  const base::TimeDelta dt =
      base::TimeTicks::Now() - *time_at_expand_on_hover_timer_start_;

  // Wait a minimum amount of time before potentially expanding. This also
  // avoids divide by zero errors because this param is at least 0.
  if (dt <= tabs::kVerticalTabsExpandOnHoverVelocityHeuristicDelay.Get()) {
    return;
  }

  // If the mouse is close to the inside edge, wait longer till the mouse is
  // more fully inside the tab strip.
  const int distance_from_inside_edge =
      std::abs(current_point.x() - GetContentsBounds().right());
  if (dt <= tabs::kVerticalTabsExpandOnHoverVelocityHeuristicEdgeDelay.Get() &&
      distance_from_inside_edge <=
          tabs::kVerticalTabsExpandOnHoverVelocityHeuristicDistanceFromEdge
              .Get()) {
    return;
  }

  if (expand_on_hover_heuristic_samples_ >=
          tabs::kVerticalTabsExpandOnHoverVelocityHeuristicMinSamples.Get() &&
      static_cast<double>(dx) / dt.InMilliseconds() <
          tabs::kVerticalTabsExpandOnHoverVelocityHeuristicThreshold.Get()) {
    AnimateExpandOnHover(/*expand=*/true);
  }
}

void VerticalTabStripRegionView::ResetExpandOnHoverTimers() {
  hover_card_animation_lock_.reset();

  if (expand_on_hover_timer_.IsRunning()) {
    expand_on_hover_timer_.Stop();

    if (tabs::IsExpandOnHoverClickDelayEnabled()) {
      RemovePreTargetHandler(&click_handler_);
    }
  }

  if (tabs::kVerticalTabsExpandOnHoverUseVelocityHeuristic.Get()) {
    expand_on_hover_heuristic_timer_.Stop();
    time_at_expand_on_hover_timer_start_ = std::nullopt;
    point_at_expand_on_hover_timer_start_ = std::nullopt;
    expand_on_hover_heuristic_samples_ = 0;
  }
}

void VerticalTabStripRegionView::AnimateExpandOnHover(bool expand) {
  is_expanded_on_hover_ = expand;
  ResetExpandOnHoverTimers();

  if (expand) {
    expand_on_hover_start_time_ = base::TimeTicks::Now();
    base::RecordAction(
        base::UserMetricsAction("VerticalTabs_ExpandOnHover_Show"));
  } else {
    if (expand_on_hover_start_time_.has_value()) {
      base::UmaHistogramLongTimes(
          "Tabs.VerticalTabs.ExpandOnHover.ShowDuration",
          base::TimeTicks::Now() - expand_on_hover_start_time_.value());
      expand_on_hover_start_time_.reset();
    }
    base::RecordAction(
        base::UserMetricsAction("VerticalTabs_ExpandOnHover_Hide"));
  }

  BrowserAnimationController::From(browser_view()->browser())
      ->Start(TabStripAnimations::kVerticalTabStrip,
              expand ? TabStripAnimations::kExpandOnHover
                     : TabStripAnimations::kCollapseOnHover);
}

void VerticalTabStripRegionView::RegisterExpandOnHoverLock(
    VerticalTabStripExpandOnHoverLock* lock) {
  hover_locks_.insert(lock);
  ExpandOnHoverLockType lock_type = lock->lock_type();
  switch (lock_type) {
    case ExpandOnHoverLockType::kForceCollapse: {
      force_collapse_lock_count_++;
      break;
    }
    case ExpandOnHoverLockType::kKeepCurrentState: {
      keep_current_state_lock_count_++;
      break;
    }
    case ExpandOnHoverLockType::kKeepExpanded: {
      keep_expanded_lock_count_++;
      break;
    }
  }
  UpdateExpandOnHoverState();
}

void VerticalTabStripRegionView::UnregisterExpandOnHoverLock(
    VerticalTabStripExpandOnHoverLock* lock) {
  hover_locks_.erase(lock);
  ExpandOnHoverLockType lock_type = lock->lock_type();
  switch (lock_type) {
    case ExpandOnHoverLockType::kForceCollapse: {
      CHECK_GT(force_collapse_lock_count_, 0);
      force_collapse_lock_count_--;
      if (force_collapse_lock_count_ == 0) {
        UpdateExpandOnHoverState();
      }
      break;
    }
    case ExpandOnHoverLockType::kKeepCurrentState: {
      CHECK_GT(keep_current_state_lock_count_, 0);
      keep_current_state_lock_count_--;
      if (keep_current_state_lock_count_ == 0) {
        UpdateExpandOnHoverState();
      }
      break;
    }
    case ExpandOnHoverLockType::kKeepExpanded: {
      CHECK_GT(keep_expanded_lock_count_, 0);
      keep_expanded_lock_count_--;
      if (keep_expanded_lock_count_ == 0) {
        UpdateExpandOnHoverState();
      }
      break;
    }
  }
}

void VerticalTabStripRegionView::OnOmniboxPopupVisibilityChanged(bool is_open) {
  if (is_open) {
    if (!omnibox_open_lock_) {
      omnibox_open_lock_ =
          GetExpandOnHoverLock(ExpandOnHoverLockType::kForceCollapse);
      UpdateExpandOnHoverState();
    }
  } else {
    omnibox_open_lock_.reset();
  }
}

void VerticalTabStripRegionView::OnActiveTabChanged(
    const tabs::TabInterface* active_tab) {
  BaseTabStripRegionView::OnActiveTabChanged(active_tab);
  browser_view()->InvalidateLayout();
  omnibox_tab_helper_observation_.Reset();

  if (active_tab) {
    OmniboxTabHelper* const tab_helper =
        OmniboxTabHelper::FromWebContents(active_tab->GetContents());
    if (tab_helper) {
      omnibox_tab_helper_observation_.Observe(tab_helper);
    }
  }
}

BEGIN_METADATA(VerticalTabStripRegionView)
END_METADATA
