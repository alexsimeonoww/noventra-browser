#include "components/tabs/public/split_tab_data.h"
#include "chrome/browser/profiles/noventra_account_spaces.h"
#include "chrome/browser/ui/views/frame/browser_view.h"
#include "chrome/browser/ui/views/frame/multi_contents_view.h"
#include "chrome/browser/ui/views/frame/noventra_dock_view.h"
#include "chrome/browser/ui/browser_commands.h"
#include "base/task/single_thread_task_runner.h"
#include "ui/views/widget/widget.h"
// Copyright 2025 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/ui/views/tabs/common/tab_view.h"

#include <optional>
#include <string>

#include "base/functional/callback_helpers.h"
#include "base/metrics/histogram_functions.h"
#include "base/metrics/user_metrics.h"
#include "base/metrics/user_metrics_action.h"
#include "base/time/time.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/dictation/dictation_keyed_service.h"
#include "chrome/browser/glic/browser_ui/tab_underline_controller.h"
#include "chrome/browser/glic/browser_ui/tab_underline_view.h"
#include "chrome/browser/glic/public/glic_enabling.h"
#include "chrome/browser/themes/theme_properties.h"
#include "chrome/browser/ui/browser_element_identifiers.h"
#include "chrome/browser/ui/browser_window/public/browser_window_interface.h"
#include "chrome/browser/ui/layout_constants.h"
#include "chrome/browser/ui/tab_contents/core_tab_helper.h"
#include "chrome/browser/ui/tabs/alert/tab_alert_controller.h"
#include "chrome/browser/ui/tabs/tab_change_type.h"
#include "chrome/browser/ui/tabs/tab_data.h"
#include "chrome/browser/ui/tabs/tab_enums.h"
#include "chrome/browser/ui/tabs/tab_muted_utils.h"
#include "chrome/browser/ui/tabs/tab_strip_model.h"
#include "chrome/browser/ui/tabs/tab_style.h"
#include "chrome/browser/ui/tabs/vertical_tab_strip_state_controller.h"
#include "chrome/browser/ui/ui_features.h"
#include "chrome/browser/ui/views/event_utils.h"
#include "chrome/browser/ui/views/frame/base_tab_strip_region_view.h"
#include "chrome/browser/ui/views/frame/browser_frame_view.h"
#include "chrome/browser/ui/views/frame/themed_background.h"
#include "chrome/browser/ui/views/frame/vertical_tab_strip_region_view.h"
#include "chrome/browser/ui/views/helium/native_frame_materials.h"
#include "chrome/browser/ui/views/tabs/common/split_tab_view.h"
#include "chrome/browser/ui/views/tabs/common/tab_collection_node.h"
#include "chrome/browser/ui/views/tabs/common/tab_collection_z_order_manager.h"
#include "chrome/browser/ui/views/tabs/common/tab_drag_handler.h"
#include "chrome/browser/ui/views/tabs/common/tab_group_view.h"
#include "chrome/browser/ui/views/tabs/common/tab_strip_collection_controller.h"
#include "chrome/browser/ui/views/tabs/common/tab_strip_utils.h"
#include "chrome/browser/ui/views/tabs/common/tab_strip_view.h"
#include "chrome/browser/ui/views/tabs/common/tab_view_horizontal_layout.h"
#include "chrome/browser/ui/views/tabs/common/tab_view_vertical_layout.h"
#include "chrome/browser/ui/views/tabs/common/vertical_tab_style_views.h"
#include "chrome/browser/ui/views/tabs/shared/tab_strip_types.h"
#include "chrome/browser/ui/views/tabs/tab/alert_indicator_button.h"
#include "chrome/browser/ui/views/tabs/tab/glow_hover_controller.h"
#include "chrome/browser/ui/views/tabs/tab/tab_accessibility.h"
#include "chrome/browser/ui/views/tabs/tab/tab_close_button.h"
#include "chrome/browser/ui/views/tabs/tab/tab_icon.h"
#include "chrome/browser/ui/views/tabs/tab/tab_title.h"
#include "chrome/browser/ui/views/tabs/tab_style_views.h"
#include "chrome/browser/ui/window_metadata/window_metadata_controller.h"
#include "chrome/common/buildflags.h"
#include "chrome/common/pref_names.h"
#include "chrome/grit/generated_resources.h"
#include "chrome/grit/theme_resources.h"
#include "components/contextual_tasks/public/features.h"
#include "components/tab_groups/tab_group_id.h"
#include "components/tabs/public/tab_alert.h"
#include "components/tabs/public/tab_collection_types.h"
#include "components/tabs/public/tab_interface.h"
#include "content/public/browser/navigation_controller.h"
#include "third_party/skia/include/core/SkPath.h"
#include "third_party/skia/include/core/SkPathBuilder.h"
#include "third_party/skia/include/core/SkRRect.h"
#include "ui/accessibility/ax_enums.mojom-shared.h"
#include "ui/base/l10n/l10n_util.h"
#include "ui/base/metadata/metadata_impl_macros.h"
#include "ui/base/models/list_selection_model.h"
#include "ui/base/theme_provider.h"
#include "ui/compositor/clip_recorder.h"
#include "ui/compositor/layer.h"
#include "ui/events/keycodes/keyboard_codes.h"
#include "ui/events/types/event_type.h"
#include "ui/gfx/color_utils.h"
#include "ui/gfx/geometry/insets.h"
#include "ui/gfx/geometry/rect.h"
#include "ui/gfx/geometry/rounded_corners_f.h"
#include "ui/gfx/geometry/size.h"
#include "ui/gfx/geometry/skia_conversions.h"
#include "ui/views/accessibility/view_accessibility.h"
#include "ui/views/background.h"
#include "ui/views/border.h"
#include "ui/views/controls/button/button.h"
#include "ui/views/controls/focus_ring.h"
#include "ui/views/controls/highlight_path_generator.h"
#include "ui/views/controls/label.h"
#include "ui/views/layout/delegating_layout_manager.h"
#include "ui/views/layout/layout_types.h"
#include "ui/views/layout/proposed_layout.h"
#include "ui/views/view.h"
#include "ui/views/view_class_properties.h"
#include "ui/views/view_utils.h"
#include "ui/events/event_observer.h"
#include "ui/views/event_monitor.h"

namespace {
// Accessible-only tab views need not receive focused key events. Observe Escape
// for the duration of the capture, as the native tab drag controller does.
class NoventraDragKeyMonitor : public ui::EventObserver {
 public:
  NoventraDragKeyMonitor(gfx::NativeWindow context,
                        base::RepeatingCallback<void(const ui::KeyEvent&)> callback)
      : callback_(std::move(callback)) {
    monitor_ = views::EventMonitor::CreateApplicationMonitor(
        this, context, {ui::EventType::kKeyPressed});
  }
  void OnEvent(const ui::Event& event) override {
    if (event.IsKeyEvent() && event.AsKeyEvent()->key_code() == ui::VKEY_ESCAPE) {
      // Cancellation destroys this observer; retain the callback locally.
      auto callback = callback_;
      callback.Run(*event.AsKeyEvent());
    }
  }
 private:
  base::RepeatingCallback<void(const ui::KeyEvent&)> callback_;
  std::unique_ptr<views::EventMonitor> monitor_;
};

constexpr int kFocusRingInset = 0.0f;

class TabHighlightPathGenerator : public views::HighlightPathGenerator {
 public:
  explicit TabHighlightPathGenerator(TabView* tab_view) : tab_view_(tab_view) {}

  // views::HighlightPathGenerator:
  SkPath GetHighlightPath(const views::View* view) override {
    return tab_view_->GetPath();
  }

 private:
  raw_ptr<TabView> tab_view_;
};

bool IsSelectionModifierDown(const ui::MouseEvent& event) {
#if BUILDFLAG(IS_MAC)
  return event.IsCommandDown();
#else
  return event.IsControlDown();
#endif
}

TabStripUserGestureDetails GetGestureDetail(const ui::Event& event) {
  TabStripUserGestureDetails gesture_detail(
      TabStripUserGestureDetails::GestureType::kOther, event.time_stamp());
  TabStripUserGestureDetails::GestureType type =
      TabStripUserGestureDetails::GestureType::kOther;
  if (event.type() == ui::EventType::kMousePressed) {
    type = TabStripUserGestureDetails::GestureType::kMouse;
  } else if (event.type() == ui::EventType::kGestureTapDown) {
    type = TabStripUserGestureDetails::GestureType::kTouch;
  }
  gesture_detail.type = type;
  return gesture_detail;
}

std::unique_ptr<TabView::LayoutManager> CreateTabViewLayout(
    TabStripOrientation orientation) {
  if (orientation == TabStripOrientation::kVertical) {
    return std::make_unique<TabViewVerticalLayout>();
  }
  return std::make_unique<TabViewHorizontalLayout>();
}
}  // namespace

class TabStyleViewDelegateImpl : public TabStyleViewDelegate {
 public:
  explicit TabStyleViewDelegateImpl(const TabView* tab_view)
      : tab_view_(tab_view) {
    CHECK(tab_view_);
  }
  ~TabStyleViewDelegateImpl() override = default;

  const views::View* GetView() const override { return tab_view_; }

  bool IsActive() const override { return tab_view_->IsActive(); }

  bool IsSelected() const override {
    return tab_view_->GetSelectionState() ==
           TabStyle::TabSelectionState::kSelected;
  }

  bool IsHovering() const override { return tab_view_->IsMouseHovered(); }

  bool IsClosing() const override { return tab_view_->IsClosing(); }
  bool IsDragging() const override { return tab_view_->IsDragging(); }

  std::optional<tab_groups::TabGroupId> GetGroup() const override {
    const tabs::TabInterface* tab_interface = tab_view_->GetTabInterface();
    return tab_interface ? tab_interface->GetGroup() : std::nullopt;
  }

  std::optional<SkColor> GetGroupColor() const override {
    const auto* controller = tab_view_->collection_node()
                                 ? tab_view_->collection_node()->GetController()
                                 : nullptr;
    return controller ? controller->GetGroupColor(tab_view_->GetTabInterface())
                      : std::nullopt;
  }

  bool IsInFocusedGroup() const override {
    const std::optional<tab_groups::TabGroupId> group = GetGroup();
    if (!group.has_value()) {
      return false;
    }
    const auto* controller = tab_view_->collection_node()
                                 ? tab_view_->collection_node()->GetController()
                                 : nullptr;
    return controller && controller->GetFocusedGroup() == group;
  }

  bool IsGroupCollapsed() const override {
    const std::optional<tab_groups::TabGroupId> group = GetGroup();
    if (!group.has_value()) {
      return false;
    }
    const auto* controller = tab_view_->collection_node()
                                 ? tab_view_->collection_node()->GetController()
                                 : nullptr;
    return controller && controller->IsGroupCollapsed(group.value());
  }

  bool IsSplit() const override { return tab_view_->split(); }
  std::optional<split_tabs::SplitTabId> GetSplit() const override {
    const tabs::TabInterface* tab_interface = tab_view_->GetTabInterface();
    return tab_interface ? tab_interface->GetSplit() : std::nullopt;
  }

  bool IsLeftSplitTab() const override {
    if (!tab_view_->split()) {
      return false;
    }
    const views::View* parent = tab_view_->parent();
    if (!parent || !views::IsViewClass<SplitTabView>(parent)) {
      return false;
    }
    const auto& children = parent->children();
    if (children.size() < 2) {
      return true;
    }
    return tab_view_ == children[base::i18n::IsRTL() ? children.size() - 1 : 0];
  }

  bool IsRightSplitTab() const override {
    if (!tab_view_->split()) {
      return false;
    }
    const views::View* parent = tab_view_->parent();
    if (!parent || !views::IsViewClass<SplitTabView>(parent)) {
      return false;
    }
    const auto& children = parent->children();
    if (children.size() < 2) {
      return true;
    }
    return tab_view_ == children[base::i18n::IsRTL() ? 0 : children.size() - 1];
  }

  const TabStyleViewDelegate* GetAdjacentTab(bool leading) const override {
    const auto* controller = tab_view_->collection_node()
                                 ? tab_view_->collection_node()->GetController()
                                 : nullptr;
    const TabCollectionNode* adjacent_node =
        controller
            ? controller->GetAdjacentTab(tab_view_->GetTabInterface(), leading)
            : nullptr;
    const TabView* adjacent_view =
        adjacent_node ? views::AsViewClass<TabView>(adjacent_node->view())
                      : nullptr;
    return adjacent_view ? adjacent_view->tab_styling()->delegate() : nullptr;
  }

  float GetHoverAnimationValue() const override {
    return tab_view_->GetHoverAnimationValue();
  }

  float GetHoverOpacity() const override {
    return tab_view_->GetHoverOpacity();
  }

  bool IsHoverAnimationActive() const override {
    return tab_view_->IsHoverAnimationActive();
  }

  GlowHoverController* GetHoverControllerForTesting() override {
    return const_cast<TabView*>(tab_view_.get())
        ->GetHoverControllerForTesting();  // IN-TEST
  }

  BrowserFrameView* GetBrowserFrameView() const override {
    const auto* controller = tab_view_->collection_node()
                                 ? tab_view_->collection_node()->GetController()
                                 : nullptr;
    return controller ? controller->GetBrowserFrameView() : nullptr;
  }

  BrowserWindowInterface* GetBrowserWindowInterface() const override {
    tabs::TabInterface* tab_interface =
        const_cast<tabs::TabInterface*>(tab_view_->GetTabInterface());
    return tab_interface ? tab_interface->GetBrowserWindowInterface() : nullptr;
  }

  int GetTabCount() const override {
    const auto* controller = tab_view_->collection_node()
                                 ? tab_view_->collection_node()->GetController()
                                 : nullptr;
    return controller ? controller->GetTabCount() : 0;
  }

  bool IsGlassFrame() const override {
    const auto* controller = tab_view_->collection_node()
                                 ? tab_view_->collection_node()->GetController()
                                 : nullptr;
    return controller ? controller->IsGlassFrame() : false;
  }

  bool IsPinned() const override { return tab_view_->pinned_; }

  bool ShouldPaintTabBackgroundColor() const override {
    return tab_view_->should_fill_background_tab_color_;
  }

  int GetStrokeThickness() const override {
    const auto* controller = tab_view_->collection_node()
                                 ? tab_view_->collection_node()->GetController()
                                 : nullptr;
    return controller ? controller->GetStrokeThickness() : 0;
  }

 private:
  const raw_ptr<const TabView> tab_view_;
};

TabView::TabView(TabCollectionNode* collection_node)
    : HoverCardAnchorTarget(this),
      collection_node_(collection_node),
      orientation_(collection_node->orientation()),
      tab_styling_(TabStyleViews::Create(
          std::make_unique<TabStyleViewDelegateImpl>(this),
          orientation_)),
      // Title must be below the favicon in the z-order as the title should be
      // drawn under the favicon during the title animation for horizontal tabs.
      title_(AddChildView(std::make_unique<TabTitle>())),
      icon_(AddChildView(std::make_unique<TabIcon>())),
      alert_indicator_(
          AddChildView(std::make_unique<AlertIndicatorButton>(this))),
      close_button_(AddChildView(std::make_unique<TabCloseButton>(
          base::BindRepeating(&TabView::CloseButtonPressed,
                              base::Unretained(this)),
          // TODO(crbug.com/467733947): Hook up metrics logging callback.
          base::DoNothingAs<void(views::View*, const ui::MouseEvent&)>()))),
      hover_controller_(gfx::Animation::ShouldRenderRichAnimation()
                            ? std::make_unique<GlowHoverController>(
                                  this,
                                  kGlowHoverAnimationDuration)
                            : nullptr) {
  tabs::TabInterface* tab = const_cast<tabs::TabInterface*>(GetTabInterface());
  BrowserWindowInterface* browser_window = tab->GetBrowserWindowInterface();

  title_->SetFontList(title_->font_list().DeriveWithSizeDelta(
      13 - title_->font_list().GetFontSize()));

  bool should_create_underline = false;
  if (browser_window) {
    Profile* profile = browser_window->GetProfile();
    icon_->SetNoventraProfilePrefs(profile->GetPrefs());
    should_create_underline =
        (glic::GlicEnabling::IsProfileEligible(profile) ||
         contextual_tasks::IsContextualTasksUIEnabled() ||
         (dictation::DictationKeyedService::Get(profile)));
  }

  if (should_create_underline) {
    glic_tab_underline_view_ =
        AddChildView(views::Builder<glic::TabUnderlineView>(
                         glic::TabUnderlineView::Factory::Create(
                             std::make_unique<glic::TabUnderlineController>(
                                 tab->GetHandle()),
                             browser_window, tab->GetHandle()))
                         .Build());
    const bool is_horizontal = orientation_ == TabStripOrientation::kHorizontal;
    glic_tab_underline_view_->SetOrientation(
        is_horizontal ? glic::TabUnderlineView::Orientation::kHorizontal
                      : glic::TabUnderlineView::Orientation::kVertical);
    if (is_horizontal) {
      glic_tab_underline_view_->SetInsets(tab_styling_->GetContentsInsets());
    }
  }

  title_->SetProperty(views::kElementIdentifierKey, kVerticalTabTitleElementId);
  SetProperty(views::kElementIdentifierKey, kTabElementId);
  // Layout manager must be set after child views are created because the
  // vertical layout stores pointers to those children.
  SetLayoutManager(CreateTabViewLayout(orientation_));
  SetEventTargeter(std::make_unique<views::ViewTargeter>(this));

  // So we get don't get enter/exit on children and don't prematurely stop the
  // hover.
  SetNotifyEnterExitOnChild(true);
  set_context_menu_controller(this);

  // Add accessibility and focus ring
  SetFocusBehavior(FocusBehavior::ACCESSIBLE_ONLY);
  views::FocusRing::Install(this);
  auto* focus_ring = views::FocusRing::Get(this);
  focus_ring->SetHaloInset(kFocusRingInset);
  focus_ring->SetOutsetFocusRingDisabled(true);

  views::HighlightPathGenerator::Install(
      this, std::make_unique<TabHighlightPathGenerator>(this));

  GetViewAccessibility().SetRole(ax::mojom::Role::kTab);
  GetViewAccessibility().SetName(
      std::string(), ax::mojom::NameFrom::kAttributeExplicitlyEmpty);

  ax_name_changed_subscription_ =
      GetViewAccessibility().AddStringAttributeChangedCallback(
          ax::mojom::StringAttribute::kName,
          base::BindRepeating(&TabView::OnAXNameChanged,
                              base::Unretained(this)));
  node_destroyed_subscription_ = collection_node_->RegisterWillDestroyCallback(
      base::BindOnce(&TabView::ResetCollectionNode, base::Unretained(this)));
  tab_state_changed_subscription_ =
      collection_node_->RegisterTabSelectionChangedCallback(base::BindRepeating(
          &TabView::OnTabStateChanged, base::Unretained(this)));
  tab_data_observer_ = std::make_unique<tabs::TabDataObserver>(tab);
  tab_data_changed_subscription_ =
      tab_data_observer_->RegisterTabDataChangedCallback(base::BindRepeating(
          &TabView::OnTabDataChanged, base::Unretained(this)));

  CHECK(collection_node_->GetController());
  if (orientation_ == TabStripOrientation::kVertical) {
    auto* state_controller =
        collection_node_->GetController()->GetStateController();
    CHECK(state_controller);
    OnCollapseStateChanged(state_controller->GetCollapseState());
    collapsed_state_changed_subscription_ =
        state_controller->RegisterOnCollapseChanged(base::BindRepeating(
            &TabView::OnCollapseStateChanged, base::Unretained(this)));
  }
  close_button_observation_.Observe(close_button_);
}

TabView::~TabView() { ResetNoventraDrag(); }

void TabView::LayoutManager::OnInstalled(views::View* host) {
  CHECK(IsViewClass<class TabView>(host));
}

TabView& TabView::LayoutManager::TabView() {
  return static_cast<class TabView&>(*host_view());
}

const TabView& TabView::LayoutManager::TabView() const {
  return static_cast<const class TabView&>(*host_view());
}

void TabView::StepLoadingAnimation(const base::TimeDelta& elapsed_time) {
  // TODO(crbug.com/467710547): Paint favicon to a layer when tab strip isn't
  // animating or when dragging isn't in progress or in full screen mode.
  icon_->StepLoadingAnimation(elapsed_time);
}

void TabView::CreateFreezingVote(FreezingVoteReason reason) {
  auto& vote = GetFreezingVote(reason);
  if (!vote.has_value()) {
    if (const tabs::TabInterface* tab = GetTabInterface()) {
      vote.emplace(tab->GetContents());
    }
  }
}

void TabView::ReleaseFreezingVote(FreezingVoteReason reason) {
  GetFreezingVote(reason).reset();
}

bool TabView::HasFreezingVote(FreezingVoteReason reason) const {
  switch (reason) {
    case FreezingVoteReason::kCollapsedGroup:
      return collapsed_freezing_vote_.has_value();
    case FreezingVoteReason::kFocusedGroup:
      return focus_mode_freezing_vote_.has_value();
  }
  NOTREACHED();
}

bool TabView::HasFreezingVote() const {
  return collapsed_freezing_vote_.has_value() ||
         focus_mode_freezing_vote_.has_value();
}

void TabView::UpdateFocusFreezing() {
  if (!features::IsTabGroupsFocusFreezingEnabled()) {
    return;
  }
  const tabs::TabInterface* tab = GetTabInterface();
  if (!tab) {
    return;
  }
  const auto* controller =
      collection_node_ ? collection_node_->GetController() : nullptr;
  if (!controller) {
    return;
  }
  const std::optional<tab_groups::TabGroupId> focused_group =
      controller->GetFocusedGroup();
  if (focused_group.has_value() && !tab->IsPinned() &&
      tab->GetGroup() != focused_group.value()) {
    CreateFreezingVote(FreezingVoteReason::kFocusedGroup);
  } else {
    ReleaseFreezingVote(FreezingVoteReason::kFocusedGroup);
  }
}

void TabView::UpdateHovered(bool hovered) {
  if (hovered_ == hovered) {
    return;
  }

  hovered_ = hovered;
  if (hover_controller_ && !split_) {
    if (hovered_) {
      hover_controller_->SetSubtleOpacityScale(radial_highlight_opacity_);
      hover_controller_->Show(TabStyle::ShowHoverStyle::kSubtle);
    } else {
      hover_controller_->Hide(TabStyle::HideHoverStyle::kGradual);
    }
  }

  UpdateColors();
  UpdateZOrder();
  InvalidateLayout();
}

bool TabView::IsHoverAnimationActive() const {
  if (split_) {
    auto* split_view = views::AsViewClass<SplitTabView>(parent());
    // Ask the parent if its hover animation is running.
    return split_view &&
           (hovered_ || (split_view->hover_controller() &&
                         split_view->hover_controller()->ShouldDraw()));
  }

  return hovered_ || (hover_controller_ && hover_controller_->ShouldDraw());
}

std::optional<SkColor> TabView::GetBackgroundColor() {
  const TabStyle::TabSelectionState selection_state = GetSelectionState();
  const bool hover_animation_active = IsHoverAnimationActive();
  if (ShouldUseNativeFrameMaterials() &&
      selection_state == TabStyle::TabSelectionState::kInactive) {
    if (!hover_animation_active) {
      return std::nullopt;
    }
    return GetNativeFrameMaterialTabBackgroundColor(selection_state);
  }
  if (active_ || selected_ || hover_animation_active ||
      should_fill_background_tab_color_) {
    return tab_styling()->CalculateTargetColors().background_color;
  }
  return std::nullopt;
}

SkPath TabView::GetPath() const {
  return tab_styling()->GetPath(TabStyle::PathType::kHighlight, 1.0f,
                                {.render_units = TabStyle::RenderUnits::kDips});
}

void TabView::Layout(PassKey) {
  LayoutSuperclass<views::View>(this);
  // A newly attached tab has no dock ancestor during construction. Refresh
  // once when attachment or actual width changes its address presentation.
  if (noventra_address_title_ != UsesNoventraUnifiedAddress()) {
    RefreshNoventraAddressTitle();
  }
  alert_indicator_->UpdateAlertIndicatorAnimation();
  if (orientation_ == TabStripOrientation::kHorizontal) {
    icon_->ResizeDiscardIndicatorRadiusForWidth(
        width() - 2 * tab_styling()->tab_style()->GetBottomCornerRadius());
  }
}

bool TabView::OnKeyPressed(const ui::KeyEvent& event) {
  CHECK(collection_node_);

  if (event.key_code() == ui::VKEY_ESCAPE && noventra_drag_pending_) {
    noventra_drag_cancelled_ = true;
    ResetNoventraDrag();
    collection_node_->GetController()->GetDragHandler().EndDrag(EndDragReason::kCancel);
    return true;
  }
  if (event.key_code() == ui::VKEY_RETURN && CanEditNoventraAddress()) {
    chrome::FocusLocationBar(const_cast<tabs::TabInterface*>(GetTabInterface())
                                 ->GetBrowserWindowInterface());
    return true;
  }
  const views::LayoutOrientation layout_orientation =
      orientation_ == TabStripOrientation::kVertical
          ? views::LayoutOrientation::kVertical
          : views::LayoutOrientation::kHorizontal;
  if (std::optional<event_utils::ExtendSelectionDirection> direction =
          event_utils::GetExtendSelectionCommandForKeyboardEvent(
              event, layout_orientation)) {
    const bool leading =
        *direction == event_utils::ExtendSelectionDirection::kPrevious;
    const TabCollectionNode* adjacent_node =
        collection_node_->GetController()->GetAdjacentTab(GetTabInterface(),
                                                          leading);
    if (adjacent_node) {
      if (TabView* adjacent_tab_view =
              views::AsViewClass<TabView>(adjacent_node->view())) {
        if (!selected_) {
          collection_node_->GetController()->SelectTab(GetTabInterface(),
                                                       GetGestureDetail(event));
        }
        collection_node_->GetController()->ExtendSelectionTo(
            adjacent_tab_view->GetTabInterface());
        base::RecordAction(
            base::UserMetricsAction("TabMultiSelect_ExtendSelectionTo"));
        if (views::FocusManager* focus_manager = GetFocusManager()) {
          focus_manager->SetFocusedViewWithReason(
              adjacent_tab_view,
              views::FocusManager::FocusChangeReason::kFocusTraversal);
        }
      }
    }
    return true;
  }

  if (event.key_code() == ui::VKEY_RETURN && !selected_) {
    collection_node_->GetController()->SelectTab(GetTabInterface(),
                                                 GetGestureDetail(event));
    return true;
  }

  std::optional<event_utils::ReorderDirection> reorder_direction =
      event_utils::GetReorderCommandForKeyboardEvent(event, layout_orientation);
  if (!reorder_direction) {
    return false;
  }

  bool move_to_end = event.flags() & ui::EF_SHIFT_DOWN;
  switch (*reorder_direction) {
    case event_utils::ReorderDirection::kPrevious: {
      if (move_to_end) {
        collection_node_->GetController()->MoveTabFirst(GetTabInterface());
      } else {
        collection_node_->GetController()->ShiftTabPrevious(GetTabInterface());
      }
      break;
    }
    case event_utils::ReorderDirection::kNext: {
      if (move_to_end) {
        collection_node_->GetController()->MoveTabLast(GetTabInterface());
      } else {
        collection_node_->GetController()->ShiftTabNext(GetTabInterface());
      }
      break;
    }
  }

  return true;
}

bool TabView::OnKeyReleased(const ui::KeyEvent& event) {
  CHECK(collection_node_);

  if (event.key_code() == ui::VKEY_SPACE && !selected_) {
    collection_node_->GetController()->SelectTab(GetTabInterface(),
                                                 GetGestureDetail(event));
    return true;
  }
  return false;
}

bool TabView::OnMousePressed(const ui::MouseEvent& event) {
  CHECK(collection_node_);

  auto* controller = collection_node_->GetController();
  noventra_drag_cancelled_ = false;
  noventra_address_pressed_ =
      CanEditNoventraAddress() && event.IsOnlyLeftMouseButton() &&
      !event.IsShiftDown() && !IsSelectionModifierDown(event) &&
      !event.IsAltDown() &&
      ((title_->GetVisible() && title_->bounds().Contains(event.location())) ||
       ((pinned_ || width() < VerticalTabStripRegionView::kWidthWithTabTitles) &&
        HitTestPoint(event.location())));
  noventra_drag_pending_ =
      orientation_ == TabStripOrientation::kVertical &&
      event.IsOnlyLeftMouseButton() && !event.IsShiftDown() &&
      !IsSelectionModifierDown(event) && !event.IsAltDown();
  noventra_drag_active_ = false;
  noventra_press_screen_ = views::View::ConvertPointToScreen(this, event.location());
  auto* model = GetTabInterface()->GetBrowserWindowInterface()->GetTabStripModel();
  noventra_page_target_ = model->GetActiveTab()->GetWeakPtr();
  if (noventra_page_target_.get() == GetTabInterface()) {
    const int index = model->GetIndexOfTab(GetTabInterface());
    noventra_page_target_.reset();
    for (int distance = 1; distance < model->count() && !noventra_page_target_; ++distance) {
      for (int candidate : {index - distance, index + distance}) {
        if (!model->ContainsIndex(candidate)) continue;
        auto* tab = model->GetTabAtIndex(candidate);
        if (tab->IsPinned() == GetTabInterface()->IsPinned() &&
            noventra::SpaceId(tab->GetContents()) == noventra::SpaceId(GetTabInterface()->GetContents())) {
          noventra_page_target_ = tab->GetWeakPtr();
          break;
        }
      }
    }
  }
  shift_pressed_on_mouse_down_ = event.IsShiftDown();
  RecordMousePressedInTab();
  UpdateHoverCard(nullptr, TabSlotController::HoverCardUpdateType::kEvent);

  // Capture the selection model before selection changes.
  const ui::ListSelectionModel original_selection_model =
      controller->GetSelectionModel();

  if (event.IsOnlyLeftMouseButton() ||
      (event.IsOnlyRightMouseButton() && event.flags() & ui::EF_FROM_TOUCH)) {
    if (event.IsShiftDown() && IsSelectionModifierDown(event)) {
      controller->AddSelectionFromAnchorTo(GetTabInterface());
      base::RecordAction(
          base::UserMetricsAction("TabMultiSelect_AddSelectionFromAnchorTo"));
    } else if (event.IsShiftDown()) {
      controller->ExtendSelectionTo(GetTabInterface());
      base::RecordAction(
          base::UserMetricsAction("TabMultiSelect_ExtendSelectionTo"));
    } else if (IsSelectionModifierDown(event)) {
      controller->ToggleSelected(GetTabInterface());
      base::RecordAction(
          base::UserMetricsAction("TabMultiSelect_ToggleSelected"));
      if (!selected_) {
        return false;
      }
    } else if (!selected_ && !noventra_drag_pending_) {
      controller->SelectTab(GetTabInterface(), GetGestureDetail(event));
      base::RecordAction(base::UserMetricsAction("SwitchTab_Click"));
    }
    // Potentially start the drag for the mouse press.
    // Follow-up mouse-movement events will update the drag controller and
    // eventually kick off the drag-loop.
    if (!noventra_drag_pending_) {
      controller->GetDragHandler().InitializeDrag(
          *collection_node_, original_selection_model, event);
    }
  }
  return true;
}

void TabView::OnMouseReleased(const ui::MouseEvent& event) {
  CHECK(collection_node_);
  if (noventra_drag_cancelled_) {
    noventra_drag_cancelled_ = false;
    return;
  }
  const bool activate_on_release =
      !noventra_drag_pending_ || HitTestPoint(event.location());

  const bool edit_address =
      noventra_address_pressed_ && !noventra_drag_active_ &&
      event.IsOnlyLeftMouseButton() && !event.IsShiftDown() &&
      !IsSelectionModifierDown(event) && !event.IsAltDown() &&
      (title_->bounds().Contains(event.location()) ||
       ((pinned_ || width() < VerticalTabStripRegionView::kWidthWithTabTitles) &&
        HitTestPoint(event.location()))) &&
      CanEditNoventraAddress();
  noventra_address_pressed_ = false;
  if (noventra_drag_pending_ && noventra_drag_active_) {
    FinishNoventraDrag();
    return;
  }
  noventra_drag_pending_ = false;
  auto* controller = collection_node_->GetController();
  base::WeakPtr<TabView> self = weak_ptr_factory_.GetWeakPtr();
  if (event.IsOnlyMiddleMouseButton()) {
    if (HitTestPoint(event.location())) {
      controller->CloseTab(GetTabInterface(), CloseTabSource::kFromMouse);
    }
  } else if (event.IsOnlyLeftMouseButton() && activate_on_release &&
             !(event.IsShiftDown() || shift_pressed_on_mouse_down_) &&
             !IsSelectionModifierDown(event)) {
    controller->SelectTab(GetTabInterface(), GetGestureDetail(event));
  }
  // Cancel the initialized drag (noop if not started). This is considered
  // a cancel because the drag handler assumes mouse capture when the drag
  // loop starts.
  controller->GetDragHandler().EndDrag(EndDragReason::kCancel);
  if (!self) {
    return;
  }
  shift_pressed_on_mouse_down_ = false;
  if (edit_address) {
    chrome::FocusLocationBar(const_cast<tabs::TabInterface*>(GetTabInterface())
                                 ->GetBrowserWindowInterface());
  }
}

void TabView::OnMouseMoved(const ui::MouseEvent& event) {
  // Hover state is handled by the parent if it is split.
  if (split_) {
    return;
  }
  // Windows synthesizes mouse move events if the user does a touch drag.
  // Don't set the hover state for those events.
  if (event.flags() & ui::EF_FROM_TOUCH) {
    return;
  }
  // Linux enter/leave events are sometimes flaky, so we don't want to "miss"
  // an enter event and fail to hover the tab.
  UpdateHovered(true);
}

void TabView::OnMouseEntered(const ui::MouseEvent& event) {
  CHECK(collection_node_);
  UpdateHoverCard(this, TabSlotController::HoverCardUpdateType::kHover);

  // Hover state is handled by the parent if it is split.
  if (split_) {
    return;
  }
  // Windows synthesizes mouse events if the user does a touch drag.
  // Don't set the hover state for those events.
  if (event.flags() & ui::EF_FROM_TOUCH) {
    return;
  }

  UpdateHovered(true);
}

void TabView::OnMouseExited(const ui::MouseEvent& event) {
  CHECK(collection_node_);

  // Hover state is handled by the parent if it is split.
  if (split_) {
    return;
  }

  UpdateHovered(false);
}

bool TabView::OnMouseDragged(const ui::MouseEvent& event) {
  // Protect against key presses when the tab is animating out. Drag events may
  // call this function after the node has been deleted.
  if (!collection_node_) {
    return false;
  }

  auto* controller = collection_node_->GetController();
  CHECK(controller);
  if (noventra_drag_pending_) {
    const auto screen_point = views::View::ConvertPointToScreen(this, event.location());
    if (UpdateNoventraDrag(event)) return true;
    // Resetting the carried-row transform changes local coordinates. Preserve
    // the actual pointer position when handing off to the native window drag.
    ui::MouseEvent handoff(event);
    // Only handing the gesture to native tear-off needs a selected source.
    // Until then the visible destination page and selection remain unchanged.
    const auto selection = controller->GetSelectionModel();
    auto self = weak_ptr_factory_.GetWeakPtr();
    controller->SelectTab(GetTabInterface(), GetGestureDetail(event));
    if (!self || !collection_node_)
      return true;
    ui::MouseEvent press(
        ui::EventType::kMousePressed,
        views::View::ConvertPointFromScreen(this, noventra_press_screen_),
        views::View::ConvertPointFromScreen(this, noventra_press_screen_),
        event.time_stamp(), event.flags(), ui::EF_LEFT_MOUSE_BUTTON);
    controller->GetDragHandler().InitializeDrag(*collection_node_, selection,
                                                press);
    handoff.set_location(views::View::ConvertPointFromScreen(this, screen_point));
    return controller->GetDragHandler().ContinueDrag(*this, handoff);
  }
  return controller->GetDragHandler().ContinueDrag(*this, event);
}

void TabView::OnGestureEvent(ui::GestureEvent* event) {
  CHECK(collection_node_);
  UpdateHoverCard(nullptr, TabSlotController::HoverCardUpdateType::kEvent);

  auto* controller = collection_node_->GetController();
  CHECK(controller);

  const ui::ListSelectionModel original_selection_model =
      collection_node_->GetController()->GetSelectionModel();

  switch (event->type()) {
    case ui::EventType::kGestureTapDown: {
      // Handle TapDown to receive subsequent events like LongPress or Tap.
      // We don't call InitializeDrag here to allow scrolling.
      event->SetHandled();
      break;
    }

    case ui::EventType::kGestureTap: {
      // Short press release. Select the tab.
      if (!selected_) {
        controller->SelectTab(GetTabInterface(), GetGestureDetail(*event));
      }
      event->SetHandled();
      break;
    }

    case ui::EventType::kGestureLongPress: {
      // Long press detected. Initialize dragging.
      if (!selected_) {
        // Ensure the tab is selected before dragging starts to avoid crashes.
        controller->SelectTab(GetTabInterface(), GetGestureDetail(*event));
      }
      controller->GetDragHandler().InitializeDrag(
          *collection_node_, original_selection_model, *event);
      event->SetHandled();
      break;
    }

    case ui::EventType::kGestureLongTap: {
      // Show context menu on release after long press.
      gfx::Point point_in_screen = event->location();
      views::View::ConvertPointToScreen(this, &point_in_screen);
      controller->ShowTabContextMenu(collection_node_, point_in_screen,
                                     ui::mojom::MenuSourceType::kTouch);
      event->SetHandled();
      break;
    }

    default:
      break;
  }
}

void TabView::PaintChildren(const views::PaintInfo& info) {
  ui::ClipRecorder clip_recorder(info.context());
  // The paint recording scale for tabs is consistent along the x and y axis.
  const float paint_recording_scale = info.paint_recording_scale_x();

  if (const std::optional<SkPath> clip_path =
          tab_styling()->GetChildClipPath(paint_recording_scale)) {
    clip_recorder.ClipPathWithAntiAliasing(clip_path.value());
  }

  View::PaintChildren(info);
}

void TabView::OnPaint(gfx::Canvas* canvas) {
  // Split pinned tabs have a merged background that is rendered in
  // `SplitTabView`.
  if (pinned_ && split_) {
    return;
  }

  tab_styling()->PaintTab(canvas);

  views::View::OnPaint(canvas);
}

void TabView::AddedToWidget() {
  paint_as_active_subscription_ =
      GetWidget()->RegisterPaintAsActiveChangedCallback(base::BindRepeating(
          &TabView::OnFrameActiveStateChanged, base::Unretained(this)));

  // A layout change can reparent the strip while a closed tab is still
  // animating out. ResetCollectionNode already retired its model and callbacks;
  // retain the closing view's painted state without reading that missing model.
  if (!collection_node_) {
    return;
  }

  OnTabStateChanged();

  // Recompute accessible name when the structure changes.
  UpdateAccessibleName();

  // Recompute the hovered state as mouse events are not processed if a view
  // removed from the widget and added.
  if (!split_) {
    UpdateHovered(IsMouseHovered());
  }
}

void TabView::RemovedFromWidget() {
  paint_as_active_subscription_ = {};
  UpdateHovered(false);
}

void TabView::OnFocus() {
  views::View::OnFocus();

  UpdateZOrder();

  if (collection_node_ && collection_node_->GetController()) {
    collection_node_->GetController()->TabKeyboardFocusChangedTo(
        GetTabInterface());
  }

  // Update the accessible label before showing the hover card to ensure that
  // the displayed memory usage is in sync with what will be read to screen
  // readers.
  UpdateAccessibleName();
  UpdateHoverCard(this, TabSlotController::HoverCardUpdateType::kFocus);
  InvalidateLayout();
}

void TabView::OnBlur() {
  views::View::OnBlur();

  UpdateZOrder();

  if (collection_node_ && collection_node_->GetController()) {
    collection_node_->GetController()->TabKeyboardFocusChangedTo(nullptr);
  }

  if (auto* tab_strip_view = GetTabStripView(this)) {
    if (!tab_strip_view->IsFocusInTabStrip()) {
      UpdateHoverCard(nullptr, TabSlotController::HoverCardUpdateType::kFocus);
    }
  }
  InvalidateLayout();
}

gfx::Size TabView::GetMinimumSize() const {
  if (orientation_ == TabStripOrientation::kHorizontal) {
    if (pinned_) {
      return gfx::Size(tab_styling()->tab_style()->GetPinnedWidth(split_),
                       tab_styling()->tab_style()->GetStandardHeight());
    }
    const int min_width =
        active_ ? tab_styling()->tab_style()->GetMinimumActiveWidth(split_)
                : tab_styling()->tab_style()->GetMinimumInactiveWidth();
    return gfx::Size(min_width,
                     tab_styling()->tab_style()->GetStandardHeight());
  }
  return views::View::GetMinimumSize();
}

void TabView::OnBoundsChanged(const gfx::Rect& previous_bounds) {
  if (orientation_ == TabStripOrientation::kVertical) {
    SetClipPath(GetPath());
    InvalidateLayout();
    // Click-to-mute depends on whether a collapsed tab is expanded on hover.
    alert_indicator_->UpdateEnabledForMuteToggle();
  }
}

void TabView::UpdateParentLayer() {
  views::View::UpdateParentLayer();
  if (layer()) {
    UpdateLayerRoundedCorners();
  }
}

void TabView::OnViewFocused(views::View* observed_view) {
  if (observed_view == close_button_) {
    InvalidateLayout();
  }
}

void TabView::OnViewBlurred(views::View* observed_view) {
  if (observed_view == close_button_) {
    InvalidateLayout();
  }
}

void TabView::UpdateLayerRoundedCorners() {
  const SkScalar corner_radius = GetCornerRadius();
  layer()->SetRoundedCornerRadius(gfx::RoundedCornersF(corner_radius));
  layer()->SetIsFastRoundedCorner(true);
}

void TabView::OnThemeChanged() {
  views::View::OnThemeChanged();
  UpdateColors();
}

bool TabView::GetHitTestMask(SkPath* mask) const {
  *mask = GetPath();
  return true;
}

bool TabView::ShouldEnableMuteToggle(int required_width) {
  // The mute toggle interferes with the click target of the
  // collapsed tab, so we disable click-to-mute for it.
  if (collapsed_ && !IsInExpandOnHover(width())) {
    return false;
  }

  if (active_) {
    return true;
  }

  if (!alert_indicator_->GetVisible()) {
    return false;
  }

  // The alert indicator is on the leading edge, after the favicon. Measure
  // the selectable title area between it and the trailing close button (or
  // the tab edge when the close button is hidden).
  const int selectable_region_end =
      close_button_->GetVisible() ? close_button_->x() : width();
  return selectable_region_end - alert_indicator_->bounds().right() >=
         required_width;
}

void TabView::ToggleTabAudioMute() {
  // The TabAlertIndicator can call this function even after the node has been
  // deleted, so prevent calling if collection node doesnt exist.
  if (!collection_node_) {
    return;
  }

  content::WebContents* const contents = GetTabInterface()->GetContents();
  bool mute = !contents->IsAudioMuted();
  base::UmaHistogramBoolean("Media.Audio.TabAudioMuted", mute);
  SetTabAudioMuted(contents, mute, TabMutedReason::kAudioIndicator,
                   /*extension_id=*/std::string());
}

bool TabView::IsApparentlyActive() const {
  if (active_) {
    return true;
  }
  if (!ShouldUseNativeFrameMaterials() && hovered_) {
    return GetHoverOpacity() > 0.5f;
  }
  return selected_;
}

void TabView::AlertStateChanged() {
  // TODO(crbug.com/457525548): Update hover card.
  InvalidateLayout();
}

void TabView::ShowContextMenuForViewImpl(
    views::View* source,
    const gfx::Point& point,
    ui::mojom::MenuSourceType source_type) {
  if (collection_node_) {
    if (auto* controller = collection_node_->GetController()) {
      controller->ShowTabContextMenu(collection_node_, point, source_type);
    }
  }
}

bool TabView::NeedsToShowThumbnail() const {
  return !IsActive();
}

bool TabView::IsValidHoverCardTarget() const {
  return collection_node_ && !IsDragging();
}

views::BubbleBorder::Arrow TabView::GetAnchorPosition() const {
  if (orientation_ == TabStripOrientation::kHorizontal) {
    return views::BubbleBorder::Arrow::TOP_LEFT;
  }
  if (pinned_ && !collapsed_) {
    return views::BubbleBorder::Arrow::TOP_LEFT;
  }
  return views::BubbleBorder::Arrow::LEFT_TOP;
}

views::BubbleAnchor TabView::GetAnchor() {
  if (split_ && !collapsed_) {
    return views::BubbleAnchor(parent());
  }
  return views::BubbleAnchor(this);
}

void TabView::ResetCollectionNode() {
  CHECK(collection_node_);
  ResetNoventraDrag();

  // Fetch the hover card controller before we clear `collection_node_`.
  TabHoverCardController* const hover_card_controller =
      collection_node_->GetController()
          ? collection_node_->GetController()->GetHoverCardController()
          : nullptr;

  // Clear all observers and subscriptions immediately to prevent any callback
  // execution during the rest of this cleanup.
  tab_data_changed_subscription_ = base::CallbackListSubscription();
  tab_state_changed_subscription_ = base::CallbackListSubscription();
  collapsed_state_changed_subscription_ = base::CallbackListSubscription();
  node_destroyed_subscription_ = base::CallbackListSubscription();

  collection_node_ = nullptr;

  if (hover_card_controller && hover_card_controller->target_tab() == this) {
    hover_card_controller->UpdateHoverCard(
        nullptr, TabSlotController::HoverCardUpdateType::kTabRemoved);
  }

  // Reset the active/selected/hovered states so the tab animates out without a
  // background.
  active_ = false;
  selected_ = false;
  UpdateZOrder();

  // Update the callbacks for the buttons so that we don't call anything that
  // needs the node.
  close_button_->SetCallback(base::RepeatingClosure(base::DoNothing()));

  layout_manager()->OnTabClosing();
}

void TabView::UpdateAccessibleName() {
  CHECK(collection_node_);

  std::u16string name =
      tabs::GetAccessibleTabLabel(GetTabInterface(), /*is_for_tab=*/true);
  if (!name.empty()) {
    GetViewAccessibility().SetName(name);
  } else {
    GetViewAccessibility().SetName(
        std::string(), ax::mojom::NameFrom::kAttributeExplicitlyEmpty);
  }
}

void TabView::OnFrameActiveStateChanged() {
  UpdateColors();
}

void TabView::OnAXNameChanged(ax::mojom::StringAttribute attribute,
                              const std::optional<std::string>& name) {
  if (GetWidget() && active_) {
    GetWidget()->UpdateAccessibleNameForRootView();
  }
}

void TabView::OnCollapseStateChanged(
    tabs::VerticalTabStripCollapseState state) {
  collapsed_ = state == tabs::VerticalTabStripCollapseState::kCollapsed;
  alert_indicator_->UpdateEnabledForMuteToggle();
  InvalidateLayout();
}

void TabView::OnTabStateChanged() {
  CHECK(collection_node_);

  tabs::TabInterface* tab = const_cast<tabs::TabInterface*>(GetTabInterface());
  CHECK(tab);

  const TabStripModel* tab_strip_model =
      tab->GetBrowserWindowInterface()->GetTabStripModel();
  int index = tab_strip_model->GetIndexOfTab(tab);
  CHECK(index != TabStripModel::kNoTab);

  active_ = tab_strip_model->IsTabInForeground(index);
  split_ = tab->IsSplit();
  pinned_ = tab->IsPinned();

  SetSelection(tab->IsSelected());
  UpdateTabData(tab_data_observer_->tab_data());

  UpdateFocusFreezing();

  UpdateColors();
  UpdateZOrder();
  InvalidateLayout();
}

void TabView::UpdateZOrder() {
  using ZOrderLevel = TabCollectionZOrderManager::ZOrderLevel;
  ZOrderLevel target_z = ZOrderLevel::kDefault;

  if (active_) {
    target_z = ZOrderLevel::kActive;
  } else if (selected_) {
    target_z = ZOrderLevel::kSelected;
  } else if (hovered_ || HasFocus()) {
    target_z = ZOrderLevel::kHovered;
  }

  if (GetProperty(kTabZOrderKey) != target_z) {
    SetProperty(kTabZOrderKey, target_z);
    if (auto* container =
            views::AsViewClass<TabCollectionZOrderManager>(parent())) {
      container->OnChildZOrderChanged(this);
    }
  }
}

void TabView::OnTabDataChanged(TabChangeType change_type,
                               const tabs::TabData& data) {
  CHECK(collection_node_);

  // Update the accessible name when the hovered tab's memory usage changes
  // so screen readers are in sync with the hover card. Skips updating
  // other visual UI elements (title, favicon, alert buttons) because those
  // states usually do not change when memory is updated.
  if (change_type == TabChangeType::kResourceUsageOnly) {
    if (IsActive() || HasFocus()) {
      UpdateAccessibleName();
    }
    return;
  }
  if (data.should_display_favicon != tab_data_.should_display_favicon) {
    layout_manager()->OnShouldDisplayFaviconChanged();
  }
  UpdateTabData(data);
}

void TabView::SetSelection(bool selected) {
  if (selected_ == selected) {
    return;
  }

  selected_ = selected;
  GetViewAccessibility().SetIsSelected(selected_);
}

void TabView::UpdateTabData(const tabs::TabData& data) {
  tabs::TabData old_data = std::move(tab_data_);
  tab_data_ = data;

  tabs::TabInterface* tab = const_cast<tabs::TabInterface*>(GetTabInterface());

  if (tabs::ShouldUpdateAccessibleName(old_data, tab_data_)) {
    UpdateAccessibleName();
  }

  icon_->SetData(tab_data_);
  icon_->SetActiveState(tab->IsActivated());
  icon_->SetAttention(TabIcon::AttentionType::kBlockedWebContents,
                      !tab->IsActivated() && tab->IsBlocked());
  icon_->SetAttention(TabIcon::AttentionType::kTabWantsAttentionStatus,
                      tab_data_.needs_attention);
  UpdateTitle(tab_data_.title, tab_data_.should_render_loading_title);
  alert_indicator_->TransitionToAlertState(tab_data_.alert_state);
  SetHoverCardDataFrom(tab_data_);
}

void TabView::SetDataForTesting(const tabs::TabData& data) {
  OnTabDataChanged(TabChangeType::kAll, data);
}

bool TabView::CanEditNoventraAddress() const {
  if (!active_ || split_ || !tab_data_.visible_url.is_valid() ||
      orientation_ != TabStripOrientation::kVertical) {
    return false;
  }
  // Opening the native editor is independent of the resting address layout.
  // Compact sidebars hide that row while retaining tab click-to-edit.
  for (const views::View* ancestor = parent(); ancestor;
       ancestor = ancestor->parent()) {
    if (views::IsViewClass<NoventraDockView>(ancestor)) {
      return true;
    }
  }
  return false;
}

bool TabView::UsesNoventraUnifiedAddress() const {
  if (!active_ || split_ || !tab_data_.visible_url.is_valid() ||
      orientation_ != TabStripOrientation::kVertical) {
    return false;
  }
  for (const views::View* ancestor = parent(); ancestor;
       ancestor = ancestor->parent()) {
    if (ancestor->GetProperty(kNoventraDockUnifiedAddress)) {
      return true;
    }
  }
  return false;
}

void TabView::RefreshNoventraAddressTitle() {
  UpdateTitle(tab_data_.title, tab_data_.should_render_loading_title);
}

views::View* TabView::GetNoventraAddressAnchor() {
  return UsesNoventraUnifiedAddress() ? title_.get() : nullptr;
}

void TabView::UpdateTitle(std::u16string title,
                          bool should_render_loading_title) {
  noventra_address_title_ = UsesNoventraUnifiedAddress();
  if (should_render_loading_title) {
    title = icon_->GetShowingLoadingAnimation()
                ? l10n_util::GetStringUTF16(IDS_TAB_LOADING_TITLE)
                : CoreTabHelper::GetDefaultTitle();
  } else {
    title = WindowMetadataController::FormatTitleForDisplay(title);
  }
  title_->SetText(title);
}

void TabView::UpdateBorder() {
  if (orientation_ == TabStripOrientation::kHorizontal) {
    SetBorder(views::CreateEmptyBorder(tab_styling()->GetContentsInsets()));
    return;
  }

  if (pinned_) {
    if (split_) {
      // Insets for border handled by the `SplitTabView`.
      SetBorder(views::CreateEmptyBorder(gfx::Insets(GetLayoutConstant(
          LayoutConstant::kVerticalTabPinnedBorderThickness))));
    } else {
      SetBorder(views::CreateEmptyBorder(gfx::Insets(GetLayoutConstant(
          LayoutConstant::kVerticalTabPinnedBorderThickness))));
    }
  } else if (GetBorder()) {
    SetBorder(nullptr);
  }
}

void TabView::UpdateColors() {
  UpdateContrastRatioValues();
  if (auto* theme_provider = GetThemeProvider()) {
    should_fill_background_tab_color_ = theme_provider->GetDisplayProperty(
        ThemeProperties::SHOULD_FILL_BACKGROUND_TAB_COLOR);
  }
  if (ShouldUseNativeFrameMaterials()) {
    should_fill_background_tab_color_ = false;
  }
  TabStyle::TabColors colors = tab_styling()->CalculateTargetColors();
  title_->SetEnabledColor(colors.foreground_color);
  icon_->SetNoventraForegroundColor(colors.foreground_color);
  close_button_->SetColors(colors);
  alert_indicator_->OnParentTabButtonColorChanged();

  UpdateBorder();

  // TODO(crbug.com/465159185): Update focus ring colors.
  SchedulePaint();
}

void TabView::UpdateContrastRatioValues() {
  auto [hover_opacity_min, hover_opacity_max, radial_highlight_opacity, _] =
      tab_styling()->tab_style()->GetContrastRatioValues(IsFrameActive(),
                                                         GetColorProvider());
  hover_opacity_min_ = hover_opacity_min;
  hover_opacity_max_ = hover_opacity_max;
  radial_highlight_opacity_ = radial_highlight_opacity;
}

void TabView::CloseButtonPressed(const ui::Event& event) {
  CHECK(collection_node_);

  if (active_) {
    base::RecordAction(base::UserMetricsAction("CloseTab_Active"));
  } else {
    base::RecordAction(base::UserMetricsAction("CloseTab_Inactive"));
  }

  CHECK(alert_indicator_);
  if (!alert_indicator_->GetVisible()) {
    base::RecordAction(base::UserMetricsAction("CloseTab_NoAlertIndicator"));
  } else if (tab_data_.alert_state.has_value()) {
    tabs::TabAlertController::RecordCloseTabMetrics(
        tab_data_.alert_state.value());
  }

  if (split_) {
    auto* split_view = views::AsViewClass<SplitTabView>(parent());
    base::RecordAction(base::UserMetricsAction(this == split_view->children()[0]
                                                   ? "CloseTab_StartTabInSplit"
                                                   : "CloseTab_EndTabInSplit"));
  }

  // Hide the interactive close button while the tab is animating out.
  if (close_button_) {
    close_button_->SetVisible(false);
  }

  const bool from_mouse = event.type() == ui::EventType::kMouseReleased &&
                          !(event.flags() & ui::EF_FROM_TOUCH);
  collection_node_->GetController()->CloseTab(
      GetTabInterface(),
      from_mouse ? CloseTabSource::kFromMouse : CloseTabSource::kFromTouch);
}

void TabView::RecordMousePressedInTab() {
  auto* tab_strip_view = GetTabStripView(this);
  CHECK(tab_strip_view);
  tab_strip_view->RecordMousePressedInTab();
}

double TabView::GetHoverAnimationValue() const {
  if (split_) {
    if (auto* split_view = views::AsViewClass<SplitTabView>(parent())) {
      return split_view->GetHoverAnimationValue();
    }
  }
  return hover_controller_ ? hover_controller_->GetAnimationValue()
                           : (hovered_ ? 1.0 : 0.0);
}

float TabView::GetHoverOpacity() const {
  // Opacity boost varies on tab width.  The interpolation is nonlinear so
  // that most tabs will fall on the low end of the opacity range, but very
  // narrow tabs will still stand out on the high end.
  // TODO(crbug.com/457525745): Determine what the min and max widths should be.
  constexpr float kWidthForMinHoverOpacity = 216.0f;
  constexpr float kWidthForMaxHoverOpacity = 32.0f;
  const float value_in_range = static_cast<float>(width());
  const float t =
      std::clamp((kWidthForMinHoverOpacity - value_in_range) /
                     (kWidthForMinHoverOpacity - kWidthForMaxHoverOpacity),
                 0.0f, 1.0f);
  return gfx::Tween::FloatValueBetween(t * t, hover_opacity_min_,
                                       hover_opacity_max_);
}

bool TabView::IsFrameActive() const {
  return GetWidget() ? GetWidget()->ShouldPaintAsActive() : true;
}

TabStyle::TabSelectionState TabView::GetSelectionState() const {
  return active_ ? TabStyle::TabSelectionState::kActive
                 : (selected_ ? TabStyle::TabSelectionState::kSelected
                              : TabStyle::TabSelectionState::kInactive);
}

SkColor TabView::GetNativeFrameMaterialTabBackgroundColor(
    TabStyle::TabSelectionState selection_state) const {
  const SkColor color = tab_styling()->tab_style()->GetTabBackgroundColor(
      selection_state, true, IsFrameActive(), GetColorProvider());
  return color_utils::AlphaBlend(
      color, SK_ColorTRANSPARENT,
      static_cast<float>(GetHoverAnimationValue()));
}

bool TabView::ShouldUseNativeFrameMaterials() const {
  if (!collection_node_) {
    return false;
  }

  const tabs::TabInterface* tab_interface = GetTabInterface();
  return tab_interface && helium::ShouldUseNativeFrameMaterials(
                              tab_interface->GetBrowserWindowInterface());
}

bool TabView::IsDragging() const {
  if (!collection_node_ || !collection_node_->GetController()) {
    return false;
  }
  const auto& drag_handler =
      collection_node_->GetController()->GetDragHandler();
  if (drag_handler.IsViewDragging(*this)) {
    return true;
  }
  for (const views::View* v = parent(); v; v = v->parent()) {
    if (drag_handler.IsViewDragging(*v)) {
      return true;
    }
  }
  return false;
}

// static
int TabView::UncollapsedMinWidth() {
  // This is the width of a tab in a split that is in a tab group, while the
  // tab strip has enough room for titles. Narrower user-selected widths keep
  // the same close-button threshold instead of squeezing both icon targets.
  return (VerticalTabStripRegionView::kWidthWithTabTitles -
          2 * GetLayoutConstant(
                  LayoutConstant::kVerticalTabStripHorizontalPadding) -
          SplitTabView::kSplitViewGap - TabGroupView::kTabLeadingPadding) /
         2;
}

// static
int TabView::CollapsedWidth() {
  return VerticalTabStripRegionView::kCollapsedWidth -
         2 * GetLayoutConstant(
                 LayoutConstant::kVerticalTabStripHorizontalPadding);
}

bool TabView::IsInExpandOnHover(int width) const {
  return collapsed_ && width > CollapsedWidth();
}

const tabs::TabInterface* TabView::GetTabInterface() const {
  if (!collection_node_) {
    return nullptr;
  }
  return std::get<tabs::ConstDanglingUntriagedTabInterface>(
      collection_node_->GetNodeData());
}

void TabView::UpdateHoverCard(HoverCardAnchorTarget* target,
                              int hover_card_update_type) {
  if (!collection_node_ || !collection_node_->GetController()) {
    return;
  }

  if (TabHoverCardController* hover_card_controller =
          collection_node_->GetController()->GetHoverCardController()) {
    hover_card_controller->UpdateHoverCard(
        target, static_cast<TabSlotController::HoverCardUpdateType>(
                    hover_card_update_type));
  }
}

SkScalar TabView::GetCornerRadius() const {
  return SkIntToScalar(
      GetLayoutConstant(LayoutConstant::kVerticalTabCornerRadius) +
      (split_ ? GetInsets().height() : 0));
}

std::optional<performance_manager::freezing::FreezingVote>&
TabView::GetFreezingVote(FreezingVoteReason reason) {
  switch (reason) {
    case FreezingVoteReason::kCollapsedGroup:
      return collapsed_freezing_vote_;
    case FreezingVoteReason::kFocusedGroup:
      return focus_mode_freezing_vote_;
  }
  NOTREACHED();
}

TabView::LayoutManager* TabView::layout_manager() {
  return static_cast<TabView::LayoutManager*>(GetLayoutManager());
}

BEGIN_METADATA(TabView)
END_METADATA

// Keep the native mouse capture while composing tabs inside a window. Starting
// the platform window-drag loop at the sidebar edge loses the original target
// on Wayland and turns a requested split into a detached browser window.
bool TabView::UpdateNoventraDrag(const ui::MouseEvent& event) {
  auto* browser = BrowserView::GetBrowserViewForBrowser(GetTabInterface()->GetBrowserWindowInterface());
  if (!browser) return false;
  const auto point = views::View::ConvertPointToScreen(this, event.location());
  if (!browser->GetBoundsInScreen().Contains(point)) {
    ResetNoventraDrag();
    return false;  // Outside the browser, retain the native tear-off gesture.
  }
  if (!noventra_drag_active_ && (point - noventra_press_screen_).LengthSquared() < 100) return true;
  if (!noventra_drag_active_) {
    noventra_drag_active_ = true;
    RequestFocus();
    browser->SetNoventraTabDragActive(true);
    noventra_drag_reveal_ = base::ScopedClosureRunner(base::BindOnce(
        [](base::WeakPtr<BrowserView> view,
           std::unique_ptr<NoventraDragKeyMonitor> key_monitor) {
          key_monitor.reset();
          if (view) {
            view->multi_contents_view()->HideNoventraSplitDropPreview();
            view->SetNoventraTabDragActive(false);
          }
        }, browser->GetAsWeakPtr(), std::make_unique<NoventraDragKeyMonitor>(
            GetWidget()->GetNativeWindow(), base::BindRepeating(
                [](base::WeakPtr<TabView> view, const ui::KeyEvent& event) {
                  if (view && view->collection_node_ && view->noventra_drag_pending_)
                    view->OnKeyPressed(event);
                }, weak_ptr_factory_.GetWeakPtr()))));
    auto collect = [&](auto&& self, views::View* view) -> void {
      if (!view->GetVisible()) return;
      if (auto* tab = views::AsViewClass<TabView>(view)) {
        noventra_drag_rows_.push_back({tab->weak_ptr_factory_.GetWeakPtr(), tab->GetBoundsInScreen()});
        return;
      }
      for (const auto& child : view->children()) self(self, child.get());
    };
    collect(collect, browser->tab_strip_view());
  }
  UpdateNoventraDrop(point);
  return true;
}

void TabView::UpdateNoventraDrop(const gfx::Point& point) {
  if (!collection_node_) return;
  auto* browser = BrowserView::GetBrowserViewForBrowser(GetTabInterface()->GetBrowserWindowInterface());
  if (!browser) return;
  noventra_last_drag_point_ = point;
  auto* model = browser->browser()->GetTabStripModel();
  auto* source = GetTabInterface();
  const auto sidebar = browser->tab_strip_view()->GetBoundsInScreen();
  const auto page = browser->multi_contents_view()->GetBoundsInScreen();
  // The Zen sidebar overlays the contents. Hit-test it before the page beneath.
  const bool in_sidebar = sidebar.Contains(point);
  noventra_drop_target_.reset();
  noventra_drop_combine_ = false;
  noventra_drop_valid_ = in_sidebar;
  noventra_drop_stacked_ = false;
  noventra_drop_after_ = false;
  TabView* target_view = nullptr;
  gfx::Rect target_bounds;
  if (in_sidebar) {
    for (const auto& row : noventra_drag_rows_) {
      if (row.view && row.view->collection_node_ && row.bounds.Contains(point)) {
        target_view = row.view.get();
        target_bounds = row.bounds;
        break;
      }
    }
  }
  std::u16string label = source->IsSplit() ? u"Separate tab" : u"Move tab";
  bool hover_candidate = false;
  bool same_split = false;
  if (target_view) {
    auto* target = model->GetTabAtIndex(model->GetIndexOfTab(target_view->GetTabInterface()));
    if (target == source) {
      noventra_drop_valid_ = false;
    } else {
      noventra_drop_target_ = target->GetWeakPtr();
      noventra_drop_after_ = point.y() >= target_bounds.CenterPoint().y();
      same_split = source->IsSplit() && source->GetSplit() == target->GetSplit();
      hover_candidate = !same_split && point.y() > target_bounds.y() + 5 &&
                        point.y() < target_bounds.bottom() - 5;
      if (hover_candidate) {
        if (noventra_hover_target_.get() != noventra_drop_target_.get() ||
            (point - noventra_hover_point_).LengthSquared() > 16) {
          noventra_hover_target_ = noventra_drop_target_;
          noventra_hover_point_ = point;
          noventra_hover_armed_ = false;
          noventra_combine_timer_.Start(FROM_HERE, base::Milliseconds(400),
              base::BindOnce([](base::WeakPtr<TabView> view) {
                if (!view || !view->noventra_drag_active_) return;
                view->noventra_hover_armed_ = true;
                view->UpdateNoventraDrop(view->noventra_last_drag_point_);
              }, weak_ptr_factory_.GetWeakPtr()));
        }
        label = u"Move tab · hold to combine";
      }
      noventra_drop_combine_ = same_split || (hover_candidate && noventra_hover_armed_);
      if (noventra_drop_combine_) {
        label = same_split ? u"Reorder split tabs" : target->IsSplit() ? u"Add tab to split" : u"Create split view";
      }
    }
  } else if (!in_sidebar && page.Contains(point) && noventra_page_target_ && noventra_page_target_.get() != source) {
    auto* hovered =
        browser->multi_contents_view()->GetNoventraSplitDropTarget(point);
    noventra_drop_target_ = hovered && hovered != source
                                ? hovered->GetWeakPtr()
                                : noventra_page_target_;
    noventra_drop_combine_ = noventra_drop_valid_ = true;
    // The entire page accepts a split. Only the top/bottom bands choose rows.
    target_bounds = page;
    if (auto* pane =
            browser->multi_contents_view()->GetContentsContainerViewFor(
                noventra_drop_target_->GetContents());
        pane && pane->GetBoundsInScreen().Contains(point)) {
      target_bounds = pane->GetBoundsInScreen();
    }
    const int band = std::min(64, target_bounds.height() / 4);
    noventra_drop_stacked_ = point.y() < target_bounds.y() + band ||
                             point.y() > target_bounds.bottom() - band;
    noventra_drop_after_ = noventra_drop_stacked_
                               ? point.y() >= target_bounds.CenterPoint().y()
                               : point.x() >= target_bounds.CenterPoint().x();
    label = noventra_page_target_->IsSplit() ? u"Add tab to split" : u"Create split view";
  }
  if (!hover_candidate) {
    noventra_combine_timer_.Stop();
    noventra_hover_target_.reset();
    noventra_hover_armed_ = false;
  }
  if (noventra_drop_target_) {
    auto* target = noventra_drop_target_.get();
    if (noventra::SpaceId(source->GetContents()) != noventra::SpaceId(target->GetContents()) || source->IsPinned() != target->IsPinned()) {
      noventra_drop_valid_ = false;
      label = u"Tabs must be in the same space and pin section";
    } else if (noventra_drop_combine_ && target->IsSplit() && target->GetSplit() != source->GetSplit() &&
               model->GetSplitData(target->GetSplit().value())->ListTabs().size() >= 4) {
      noventra_drop_valid_ = false;
      label = u"This split already has four tabs";
    }
  }
  if (noventra_drop_combine_ && !same_split && !browser->GetProfile()->GetPrefs()->GetBoolean(prefs::kSplitViewDragAndDropEnabled)) {
    noventra_drop_valid_ = false;
    label = u"Enable split view dragging in Settings";
  }

  // Preview the insertion without mutating collections during mouse capture.
  // Hit-testing continues to use the original row positions, not translated ones.
  gfx::Rect source_bounds;
  for (const auto& row : noventra_drag_rows_) if (row.view.get() == this) source_bounds = row.bounds;
  const bool reorder = in_sidebar && noventra_drop_valid_ && (!noventra_drop_combine_ || same_split);
  for (const auto& row : noventra_drag_rows_) {
    if (!row.view || !row.view->collection_node_) continue;
    gfx::Transform transform;
    if (reorder) {
      if (row.view.get() == this) {
        transform.Translate(0, point.y() - noventra_press_screen_.y());
      } else if (row.view->GetTabInterface()->IsPinned() == source->IsPinned()) {
        if (point.y() > source_bounds.CenterPoint().y() && row.bounds.CenterPoint().y() > source_bounds.CenterPoint().y() &&
            row.bounds.CenterPoint().y() <= point.y()) transform.Translate(0, -source_bounds.height());
        if (point.y() < source_bounds.CenterPoint().y() && row.bounds.CenterPoint().y() < source_bounds.CenterPoint().y() &&
            row.bounds.CenterPoint().y() >= point.y()) transform.Translate(0, source_bounds.height());
      }
    }
    row.view->SetTransform(transform);
  }
  const auto destination =
      noventra_drop_valid_ && noventra_drop_combine_
          ? browser->multi_contents_view()->GetNoventraSplitDropBounds(
                source, noventra_drop_target_.get(), noventra_drop_after_,
                noventra_drop_stacked_)
          : gfx::Rect();
  if (noventra_drop_valid_ && noventra_drop_combine_) {
    auto layout = split_tabs::SplitTabLayout::kSideBySide;
    if (noventra_drop_target_ && noventra_drop_target_->GetSplit()) {
      layout = model->GetSplitData(*noventra_drop_target_->GetSplit())
                   ->visual_data()
                   ->split_layout();
    }
    if (noventra_drop_stacked_) {
      layout = split_tabs::SplitTabLayout::kStacked;
    }
    if (in_sidebar && same_split) {
      label = u"Reorder split tabs";
    } else if (layout == split_tabs::SplitTabLayout::kGrid) {
      label = u"Place in split";
    } else {
      label = layout == split_tabs::SplitTabLayout::kStacked
                  ? (noventra_drop_after_ ? u"Split below" : u"Split above")
                  : (noventra_drop_after_ ? u"Split right" : u"Split left");
    }
  }
  browser->multi_contents_view()->ShowNoventraSplitDropPreview(
      destination, point, label + u" · " + tab_data_.title);
}

void TabView::ResetNoventraDrag() {
  // A closing tab may already be detached from its BrowserWindowInterface.
  // The active drag's closure owns the weak browser reference for cleanup.
  noventra_address_pressed_ = false;
  noventra_drag_pending_ = noventra_drag_active_ = false;
  noventra_combine_timer_.Stop();
  noventra_hover_target_.reset();
  noventra_hover_armed_ = false;
  for (const auto& row : noventra_drag_rows_) if (row.view) row.view->SetTransform(gfx::Transform());
  noventra_drag_rows_.clear();
  noventra_drag_reveal_.RunAndReset();
}

void TabView::FinishNoventraDrag() {
  auto* browser = BrowserView::GetBrowserViewForBrowser(GetTabInterface()->GetBrowserWindowInterface());
  auto* model = browser->browser()->GetTabStripModel();
  auto source = model->GetTabAtIndex(model->GetIndexOfTab(GetTabInterface()))->GetWeakPtr();
  auto target = noventra_drop_target_;
  const bool combine = noventra_drop_combine_;
  const bool valid = noventra_drop_valid_;
  const bool stacked = noventra_drop_stacked_;
  const bool after = noventra_drop_after_;
  const bool had_target = !!target || target.WasInvalidated();
  ResetNoventraDrag();
  // Model operations can destroy this tab view while it is handling the event.
  // Complete the capture/drag cleanup first, then resolve weak tab identities.
  const auto weak_browser = browser->GetAsWeakPtr();
  base::SingleThreadTaskRunner::GetCurrentDefault()->PostTask(FROM_HERE,
      base::BindOnce([](base::WeakPtr<BrowserView> view,
                       base::WeakPtr<tabs::TabInterface> source,
                       base::WeakPtr<tabs::TabInterface> target, bool combine, bool stacked, bool valid, bool after, bool had_target) {
        if (!valid || !view || !source) return;
        auto* model = view->browser()->GetTabStripModel();
        if (!model->ContainsIndex(model->GetIndexOfTab(source.get()))) return;
        if (had_target && (!target || !model->ContainsIndex(model->GetIndexOfTab(target.get())))) return;
        if (target && (noventra::SpaceId(source->GetContents()) != noventra::SpaceId(target->GetContents()) ||
                       source->IsPinned() != target->IsPinned())) return;
        if (combine) {
          const bool same_split = target && source->IsSplit() && source->GetSplit() == target->GetSplit();
          if (!same_split && !view->GetProfile()->GetPrefs()->GetBoolean(prefs::kSplitViewDragAndDropEnabled)) return;
          if (target &&
              model->JoinNoventraSplit(model->GetIndexOfTab(source.get()),
                                       model->GetIndexOfTab(target.get()))) {
            if (!view || !source || !target)
              return;
            model = view->browser()->GetTabStripModel();
            const int source_index = model->GetIndexOfTab(source.get());
            if (!model->ContainsIndex(source_index) ||
                !model->ContainsIndex(model->GetIndexOfTab(target.get())))
              return;
            int destination =
                model->GetIndexOfTab(target.get()) + (after ? 1 : 0);
            if (source_index < destination)
              --destination;
            if (model->ContainsIndex(destination) &&
                destination != source_index)
              model->MoveWebContentsAt(source_index, destination, true);
            if (view && source && stacked && source->GetSplit())
              view->browser()->GetTabStripModel()->UpdateSplitLayout(
                  *source->GetSplit(), split_tabs::SplitTabLayout::kStacked);
          }
        } else {
          model->DetachNoventraSplitTab(model->GetIndexOfTab(source.get()));
          const int source_index = model->GetIndexOfTab(source.get());
          int insertion = model->count();
          if (target && model->ContainsIndex(model->GetIndexOfTab(target.get()))) {
            int start = model->GetIndexOfTab(target.get());
            int end = start + 1;
            if (target->GetSplit()) {
              const auto members = model->GetSplitData(*target->GetSplit())->ListTabs();
              start = model->GetIndexOfTab(members.front());
              end = model->GetIndexOfTab(members.back()) + 1;
            }
            insertion = after ? end : start;
          }
          const int destination = insertion - (source_index < insertion ? 1 : 0);
          if (model->ContainsIndex(destination)) {
            model->MoveWebContentsAt(source_index, destination, true);
            model->ActivateTabAt(model->GetIndexOfTab(source.get()));
          }
        }
      }, weak_browser, source, target, combine, stacked, valid, after, had_target));
  collection_node_->GetController()->GetDragHandler().EndDrag(EndDragReason::kCancel);
}

void TabView::OnMouseCaptureLost() {
  noventra_drag_cancelled_ = noventra_drag_pending_;
  ResetNoventraDrag();
  views::View::OnMouseCaptureLost();
}
