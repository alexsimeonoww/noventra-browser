// Copyright 2013 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/ui/views/toolbar/toolbar_view.h"
#include "chrome/browser/ui/noventra/layout_model.h"

#include <algorithm>
#include <memory>
#include <utility>

#include "base/auto_reset.h"
#include "base/command_line.h"
#include "base/containers/fixed_flat_map.h"
#include "base/feature_list.h"
#include "base/functional/bind.h"
#include "base/functional/callback.h"
#include "base/i18n/number_formatting.h"
#include "base/i18n/rtl.h"
#include "base/metrics/histogram_functions.h"
#include "base/metrics/user_metrics.h"
#include "base/metrics/user_metrics_action.h"
#include "base/notimplemented.h"
#include "base/strings/utf_string_conversions.h"
#include "base/trace_event/trace_event.h"
#include "build/build_config.h"
#include "chrome/app/chrome_command_ids.h"
#include "chrome/browser/actor/ui/actor_ui_metrics.h"
#include "chrome/browser/command_updater.h"
#include "chrome/browser/glic/browser_ui/activity_list_bubble/glic_activity_list_bubble.h"
#include "chrome/browser/glic/browser_ui/activity_list_bubble/glic_activity_list_bubble_controller.h"
#include "chrome/browser/glic/browser_ui/glic_actor_nudge_controller.h"
#include "chrome/browser/glic/browser_ui/glic_nudge_controller.h"
#include "chrome/browser/glic/browser_ui/glic_split_button_controller.h"
#include "chrome/browser/glic/public/features.h"
#include "chrome/browser/glic/public/glic_enabling.h"
#include "chrome/browser/glic/public/glic_invoke_options.h"
#include "chrome/browser/glic/public/glic_keyed_service.h"
#include "chrome/browser/glic/public/glic_keyed_service_factory.h"
#include "chrome/browser/glic/public/service/glic_activity_manager.h"
#include "chrome/browser/glic/public/service/glic_instance_coordinator.h"
#include "chrome/browser/media/router/media_router_feature.h"
#include "chrome/browser/performance_manager/public/user_tuning/user_tuning_utils.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/profiles/profiles_state.h"
#include "chrome/browser/themes/theme_properties.h"
#include "chrome/browser/ui/actions/chrome_action_id.h"
#include "chrome/browser/ui/ai_overlay_dialog/ai_overlay_dialog_controller.h"
#include "chrome/browser/ui/browser_actions.h"
#include "chrome/browser/ui/browser_command_controller.h"
#include "chrome/browser/ui/browser_commands.h"
#include "chrome/browser/ui/chrome_pages.h"
#include "chrome/browser/themes/theme_service.h"
#include "chrome/browser/themes/theme_service_factory.h"
#include "chrome/browser/ui/browser_content_setting_bubble_model_delegate.h"
#include "chrome/browser/ui/browser_element_identifiers.h"
#include "chrome/browser/ui/browser_tabstrip.h"
#include "chrome/browser/ui/browser_window.h"
#include "chrome/browser/ui/browser_window/public/browser_window_features.h"
#include "chrome/browser/ui/browser_window/public/browser_window_interface.h"
#include "chrome/browser/ui/color/chrome_color_id.h"
#include "chrome/browser/ui/global_error/global_error.h"
#include "chrome/browser/ui/global_error/global_error_service.h"
#include "chrome/browser/ui/global_error/global_error_service_factory.h"
#include "chrome/browser/ui/global_media_controls/media_toolbar_button_controller.h"
#include "chrome/browser/ui/helium/helium_layout_state_controller.h"
#include "chrome/browser/ui/intent_picker_tab_helper.h"
#include "chrome/browser/ui/layout_constants.h"
#include "chrome/browser/ui/omnibox/omnibox_view.h"
#include "chrome/browser/ui/page_action/page_action_properties_provider.h"
#include "chrome/browser/ui/side_panel/side_panel_entry_id.h"
#include "chrome/browser/ui/tabs/public/tab_features.h"
#include "chrome/browser/ui/tabs/tab_strip_model.h"
#include "chrome/browser/ui/tabs/tab_strip_prefs.h"
#include "chrome/browser/ui/tabs/vertical_tab_strip_state_controller.h"
#include "chrome/browser/ui/toolbar/app_menu_model.h"
#include "chrome/browser/ui/toolbar/chrome_labs/chrome_labs_prefs.h"
#include "chrome/browser/ui/toolbar/chrome_labs/chrome_labs_utils.h"
#include "chrome/browser/ui/ui_features.h"
#include "chrome/browser/ui/view_ids.h"
#include "chrome/browser/ui/views/bookmarks/bookmark_bubble_view.h"
#include "chrome/browser/ui/views/contextual_tasks/contextual_tasks_button.h"
#include "chrome/browser/ui/views/contextual_tasks/contextual_tasks_close_tab_button.h"
#include "chrome/browser/ui/views/contextual_tasks/contextual_tasks_ephemeral_button_controller.h"
#include "chrome/browser/ui/views/extensions/extension_popup.h"
#include "chrome/browser/ui/views/extensions/extensions_container_views.h"
#include "chrome/browser/ui/views/extensions/extensions_toolbar_button.h"
#include "chrome/browser/ui/views/extensions/extensions_toolbar_coordinator.h"
#include "chrome/browser/ui/views/extensions/extensions_toolbar_desktop.h"
#include "chrome/browser/ui/views/frame/browser_view.h"
#include "chrome/browser/ui/views/frame/custom_corners_background.h"
#include "chrome/browser/ui/views/frame/horizontal_tab_strip_region_view.h"
#include "chrome/browser/ui/views/frame/noventra_dock_view.h"
#include "chrome/browser/ui/views/frame/vertical_tab_strip_region_view.h"
#include "chrome/browser/ui/views/frame/top_container_view.h"
#include "chrome/browser/ui/views/glic/glic_button_interface.h"
#include "chrome/browser/ui/views/global_media_controls/media_toolbar_button.h"
#include "chrome/browser/ui/views/global_media_controls/media_toolbar_button_contextual_menu.h"
#include "chrome/browser/ui/views/global_media_controls/media_toolbar_button_view.h"
#include "chrome/browser/ui/views/intent_picker_bubble_view.h"
#include "chrome/browser/ui/views/location_bar/webui_location_bar.h"
#include "chrome/browser/ui/views/page_action/page_action_container_view.h"
#include "chrome/browser/ui/views/page_action/page_action_view.h"
#include "chrome/browser/ui/views/page_action/page_action_view_interface.h"
#include "chrome/browser/ui/views/performance_controls/battery_saver_button.h"
#include "chrome/browser/ui/views/performance_controls/performance_intervention_button.h"
#include "chrome/browser/ui/views/profiles/avatar_toolbar_button.h"
#include "chrome/browser/ui/views/side_panel/side_panel.h"
#include "chrome/browser/ui/views/tabs/glic/glic_and_actor_buttons_container.h"
#include "chrome/browser/ui/views/tabs/tab_strip.h"
#include "chrome/browser/ui/views/tabs/tab_strip_controller.h"
#include "chrome/browser/ui/views/toolbar/app_menu.h"
#include "chrome/browser/ui/views/toolbar/app_menu_control.h"
#include "chrome/browser/ui/views/toolbar/avatar_toolbar_button_interface.h"
#include "chrome/browser/ui/views/toolbar/back_forward_button.h"
#include "chrome/browser/ui/views/toolbar/browser_app_menu_button.h"
#include "chrome/browser/ui/views/toolbar/chrome_labs/chrome_labs_coordinator.h"
#include "chrome/browser/ui/views/toolbar/home_button.h"
#include "chrome/browser/ui/views/toolbar/overflow_menu.h"
#include "chrome/browser/ui/views/toolbar/pinned_toolbar_actions_container.h"
#include "chrome/browser/ui/views/toolbar/reload_button.h"
#include "chrome/browser/ui/views/toolbar/split_tabs_button.h"
#include "chrome/browser/ui/views/toolbar/toolbar_button.h"
#include "chrome/browser/ui/views/toolbar/toolbar_controller.h"
#include "chrome/browser/ui/views/toolbar/toolbar_divider.h"
#include "chrome/browser/ui/views/toolbar/toolbar_glic_actor_task_icon.h"
#include "chrome/browser/ui/views/toolbar/toolbar_glic_button.h"
#include "chrome/browser/ui/views/toolbar/toolbar_icon_container_view.h"
#include "chrome/browser/ui/views/toolbar/webui_back_forward_control.h"
#include "chrome/browser/ui/views/toolbar/webui_toolbar_web_view.h"
#include "chrome/browser/ui/views/zoom/zoom_view_controller.h"
#include "chrome/browser/ui/waap/initial_webui_window_metrics_manager.h"
#include "chrome/browser/ui/web_applications/app_browser_controller.h"
#include "chrome/browser/ui/window_feature_controller/window_feature_controller.h"
#include "chrome/browser/web_applications/link_capturing_features.h"
#include "chrome/common/chrome_features.h"
#include "chrome/common/pref_names.h"
#include "chrome/common/webui_url_constants.h"
#include "chrome/grit/branded_strings.h"
#include "chrome/grit/generated_resources.h"
#include "chrome/grit/theme_resources.h"
#include "components/autofill/core/common/autofill_payments_features.h"
#include "components/contextual_tasks/public/features.h"
#include "components/feature_engagement/public/feature_constants.h"
#include "components/omnibox/browser/location_bar_model.h"
#include "components/prefs/pref_service.h"
#include "components/safe_browsing/core/common/features.h"
#include "components/send_tab_to_self/features.h"
#include "components/signin/public/base/signin_buildflags.h"
#include "components/strings/grit/components_strings.h"
#include "components/vector_icons/vector_icons.h"
#include "content/public/browser/render_view_host.h"
#include "content/public/browser/web_contents.h"
#include "third_party/skia/include/core/SkPath.h"
#include "third_party/skia/include/core/SkPathBuilder.h"
#include "ui/accessibility/ax_node_data.h"
#include "ui/actions/actions.h"
#include "ui/base/interaction/element_identifier.h"
#include "ui/base/l10n/l10n_util.h"
#include "ui/base/metadata/metadata_impl_macros.h"
#include "ui/menus/simple_menu_model.h"
#include "ui/base/menu_source_utils.h"
#include "ui/base/theme_provider.h"
#include "ui/base/ui_base_features.h"
#include "ui/base/window_open_disposition.h"
#include "ui/base/window_open_disposition_utils.h"
#include "ui/color/color_id.h"
#include "ui/color/color_provider.h"
#include "ui/compositor/layer.h"
#include "ui/gfx/canvas.h"
#include "ui/gfx/geometry/insets.h"
#include "ui/gfx/image/canvas_image_source.h"
#include "ui/gfx/paint_vector_icon.h"
#include "ui/gfx/scoped_canvas.h"
#include "ui/gfx/text_utils.h"
#include "ui/views/accessibility/view_accessibility.h"
#include "ui/views/actions/action_view_controller.h"
#include "ui/views/background.h"
#include "ui/views/border.h"
#include "ui/views/cascading_property.h"
#include "ui/views/controls/button/label_button.h"
#include "ui/views/controls/label.h"
#include "ui/views/controls/separator.h"
#include "ui/views/layout/fill_layout.h"
#include "ui/views/layout/flex_layout.h"
#include "ui/views/layout/flex_layout_view.h"
#include "ui/views/layout/proposed_layout.h"
#include "ui/views/mouse_watcher.h"
#include "ui/views/mouse_watcher_view_host.h"
#include "ui/views/vector_icons.h"
#include "ui/views/view.h"
#include "ui/views/view_class_properties.h"
#include "ui/views/view_tracker.h"
#include "ui/views/view_utils.h"
#include "ui/views/widget/tooltip_manager.h"
#include "ui/views/widget/widget.h"

#if defined(USE_AURA)
#include <array>

#include "ui/aura/window_occlusion_tracker.h"
#endif

using base::UserMetricsAction;
using content::WebContents;

DEFINE_UI_CLASS_PROPERTY_KEY(bool, kActionItemUnderlineIndicatorKey, false)

// Owned with the toolbar; trackers become null if optional controls disappear.
using NoventraToolbarOrder = std::vector<std::unique_ptr<views::ViewTracker>>;
DEFINE_UI_CLASS_PROPERTY_TYPE(NoventraToolbarOrder*)
DEFINE_OWNED_UI_CLASS_PROPERTY_KEY(NoventraToolbarOrder, kNoventraToolbarOrderKey)

namespace {

constexpr int kToolbarFlexOrderOffset = 1000;
constexpr int kLocationBarFlexOrder = kToolbarFlexOrderOffset + 1;
constexpr int kToolbarActionsFlexOrder = kToolbarFlexOrderOffset + 2;
constexpr int kExtensionsFlexOrder = kToolbarFlexOrderOffset + 3;
constexpr int kTabStripFlexOrder = kToolbarFlexOrderOffset + 4;

constexpr int kDynamicNewTabButtonIconSize = 16;

class DynamicNewTabToolbarButton : public ToolbarButton {
 public:
  explicit DynamicNewTabToolbarButton(PressedCallback callback)
      : ToolbarButton(std::move(callback)) {}
  ~DynamicNewTabToolbarButton() override = default;

 protected:
  int GetIconSize() const override { return kDynamicNewTabButtonIconSize; }

  void OnBoundsChanged(const gfx::Rect& previous_bounds) override {
    ToolbarButton::OnBoundsChanged(previous_bounds);
    UpdatePresentation();
  }

  void ViewHierarchyChanged(
      const views::ViewHierarchyChangedDetails& details) override {
    ToolbarButton::ViewHierarchyChanged(details);
    UpdatePresentation();
  }

 private:
  void UpdatePresentation() {
    const bool sidebar = parent() &&
                         (views::IsViewClass<NoventraDockView>(parent()) ||
                          parent()->GetProperty(kNoventraDockKind) == 6) &&
                         !GetProperty(kNoventraDockHorizontal);
    const auto title = l10n_util::GetStringUTF16(IDS_ACCNAME_NEWTAB);
    // This row can show its short label well before tab titles expand. Include
    // the label's trailing padding even while it is hidden, so resizing does
    // not alternate between the labeled and icon-only measurements.
    const int labeled_width = gfx::GetStringWidth(title, label()->font_list()) +
                              kDynamicNewTabButtonIconSize + 7 +
                              GetInsets().width() + GetRoundedCornerRadius() / 2;
    const std::u16string text =
        sidebar && width() >= labeled_width ? title : std::u16string();
    if (GetText() != text) {
      SetText(text);
      SetImageLabelSpacing(7);
    }
    SetHorizontalAlignment(sidebar && !text.empty() ? gfx::ALIGN_LEFT
                                                   : gfx::ALIGN_CENTER);
  }

  int GetRoundedCornerRadius() const override {
    return parent() &&
                   (views::IsViewClass<NoventraDockView>(parent()) ||
                    parent()->GetProperty(kNoventraDockKind) == 6) &&
                   !GetProperty(kNoventraDockHorizontal)
               ? GetLayoutConstant(LayoutConstant::kVerticalTabCornerRadius)
               : ToolbarButton::GetRoundedCornerRadius();
  }
};

// Gets the display mode for a given browser.
ToolbarView::DisplayMode GetDisplayMode(BrowserWindowInterface* browser) {
  // Checked in this order because even tabbed PWAs use the CUSTOM_TAB
  // display mode.
  if (web_app::AppBrowserController::IsWebApp(browser)) {
    return ToolbarView::DisplayMode::kCustomTab;
  }

  if (WindowFeatureController::From(browser)->SupportsWindowFeature(
          WindowFeatureController::WindowFeature::kFeatureTabStrip)) {
    return ToolbarView::DisplayMode::kNormal;
  }

  return ToolbarView::DisplayMode::kLocation;
}

auto& GetViewCommandMap() {
  static constexpr auto kViewCommandMap = base::MakeFixedFlatMap<int, int>(
      {{VIEW_ID_BACK_BUTTON, IDC_BACK},
       {VIEW_ID_FORWARD_BUTTON, IDC_FORWARD},
       {VIEW_ID_HOME_BUTTON, IDC_HOME},
       {VIEW_ID_RELOAD_BUTTON, IDC_RELOAD},
       {VIEW_ID_AVATAR_BUTTON, IDC_SHOW_AVATAR_MENU},
       {VIEW_ID_DYNAMIC_NEW_TAB_BUTTON, IDC_NEW_TAB},
       {VIEW_ID_COLLAPSE_VERTICAL_TABS_BUTTON, IDC_TOGGLE_VERTICAL_TABS}});
  return kViewCommandMap;
}

constexpr int kBrowserAppMenuRefreshCollapsedMargin = 2;
constexpr int kLargeSpaceBetweenButtons = 6;
constexpr int kInsideBorderAroundGlicButtons = 2;
constexpr int kGlicButtonMargin = 5;

void SetRefreshMargins(views::View* button, bool expanded) {
  button->SetProperty(
      views::kMarginsKey,
      gfx::Insets::VH(
          0, expanded ? GetLayoutConstant(
                            LayoutConstant::kToolbarButtonRefreshExpandedMargin)
                      : kBrowserAppMenuRefreshCollapsedMargin));
}

constexpr int kCenteredLocationBarMaxWidth = 700;

struct CenteredLocationBarStep {
  int min_toolbar_width;
  int toolbar_width_reduction_percent;
};

constexpr CenteredLocationBarStep kCenteredLocationBarSteps[] = {
    {1000, 12},
    {800, 10},
    {600, 5},
};

int CenteredLocationBarWidthReduction(int toolbar_width) {
  for (const auto& step : kCenteredLocationBarSteps) {
    if (toolbar_width >= step.min_toolbar_width) {
      return toolbar_width * step.toolbar_width_reduction_percent / 100;
    }
  }
  return 0;
}

class ResponsiveToolbarLayout : public views::FlexLayout {
 public:
  explicit ResponsiveToolbarLayout(base::RepeatingCallback<bool()> is_compact)
      : is_compact_(std::move(is_compact)) {}

  ~ResponsiveToolbarLayout() override = default;

  views::FlexSpecification ProtectedCompactFlex(
      const views::FlexSpecification& responsive,
      views::MaximumFlexSizeRule maximum =
          views::MaximumFlexSizeRule::kPreferred) const {
    const views::FlexSpecification protected_spec(
        views::MinimumFlexSizeRule::kPreferred, maximum);
    const views::FlexRule mode_rule = base::BindRepeating(
        &ResponsiveToolbarLayout::GetSizeForMode, base::Unretained(this),
        responsive.rule(), protected_spec.rule());
    return views::FlexSpecification(mode_rule)
        .WithOrder(responsive.order())
        .WithWeight(responsive.weight())
        .WithAlignment(responsive.alignment());
  }

 protected:
  views::ProposedLayout CalculateProposedLayout(
      const views::SizeBounds& bounds) const override {
    base::AutoReset reset_mode(&flex_mode_, FlexMode::kResponsive);
    if (is_compact_.Run() && bounds.width().is_bounded()) {
      // Measure the controls with compact protection and no space for tabs.
      flex_mode_ = FlexMode::kProtectControls;
      const auto minimum = FlexLayout::CalculateProposedLayout(
          views::SizeBounds(0, bounds.height()));
      flex_mode_ = bounds.width().value() < minimum.host_size.width()
                       ? FlexMode::kResponsive
                       : FlexMode::kProtectControls;
    }
    return FlexLayout::CalculateProposedLayout(bounds);
  }

 private:
  enum class FlexMode {
    // Use each control's normal shrinking and overflow behavior.
    kResponsive,
    // Keep controls from shrinking or overflowing while there is room.
    kProtectControls,
  };

  gfx::Size GetSizeForMode(const views::FlexRule& responsive_rule,
                           const views::FlexRule& protected_rule,
                           const views::View* view,
                           const views::SizeBounds& bounds) const {
    if (flex_mode_ == FlexMode::kResponsive) {
      return responsive_rule.Run(view, bounds);
    }
    return protected_rule.Run(view, bounds);
  }

  base::RepeatingCallback<bool()> is_compact_;
  mutable FlexMode flex_mode_ = FlexMode::kResponsive;
};

// Optionally centers the location bar after the toolbar's flex layout.
class CenteredLocationBarLayout : public ResponsiveToolbarLayout {
 public:
  using GetCenteredBoundsCallback =
      base::RepeatingCallback<gfx::Rect(gfx::Rect, int)>;

  CenteredLocationBarLayout(views::View* location_bar,
                            GetCenteredBoundsCallback callback,
                            base::RepeatingCallback<bool()> is_compact)
      : ResponsiveToolbarLayout(std::move(is_compact)),
        location_bar_(location_bar),
        callback_(std::move(callback)) {}

  ~CenteredLocationBarLayout() override = default;

 protected:
  void LayoutImpl() override {
    views::ProposedLayout layout = GetProposedLayout(host_view()->size());
    views::ChildLayout* const location_bar_layout =
        layout.GetLayoutFor(location_bar_);
    if (location_bar_layout && location_bar_layout->visible) {
      location_bar_layout->bounds =
          callback_.Run(location_bar_layout->bounds, layout.host_size.width());
    }
    ApplyLayout(layout);
  }

 private:
  const raw_ptr<views::View> location_bar_;
  GetCenteredBoundsCallback callback_;
};

}  // namespace

////////////////////////////////////////////////////////////////////////////////
// ToolbarView, public:

DEFINE_CLASS_ELEMENT_IDENTIFIER_VALUE(ToolbarView, kToolbarElementId);

ToolbarView::ToolbarView(BrowserWindowInterface* browser,
                         BrowserView* browser_view)
    : AnimationDelegateViews(this),
      browser_(browser),
      browser_view_(browser_view),
      app_menu_icon_controller_(browser->GetProfile(), this),
      display_mode_(GetDisplayMode(browser)) {
  // WebApp type-browsers set their own ToolbarButtonProvider.
  if (!web_app::AppBrowserController::IsWebApp(browser)) {
    scoped_unowned_user_data_.emplace(browser_->GetUnownedUserDataHost(),
                                      *this);
  }

  SetID(VIEW_ID_TOOLBAR);
  SetProperty(views::kElementIdentifierKey, kToolbarElementId);

  GetViewAccessibility().SetRole(ax::mojom::Role::kToolbar);

  if (display_mode_ == DisplayMode::kNormal) {
    for (const auto& view_and_command : GetViewCommandMap()) {
      chrome::AddCommandObserver(browser_, view_and_command.second, this);
    }
  }
  views::SetCascadingColorProviderColor(this, views::kCascadingBackgroundColor,
                                        kColorToolbar);

  mouse_watcher_ = std::make_unique<views::MouseWatcher>(
      std::make_unique<views::MouseWatcherViewHost>(this, gfx::Insets()), this);
}

ToolbarView::~ToolbarView() {
  if (glic_split_button_controller_) {
    glic_split_button_controller_->SetVerticalTabsDelegate(nullptr);
  }

  if (display_mode_ != DisplayMode::kNormal) {
    return;
  }

  if (overflow_button_) {
    overflow_button_->set_toolbar_controller(nullptr);
  }

  for (const auto& view_and_command : GetViewCommandMap()) {
    chrome::RemoveCommandObserver(browser_, view_and_command.second, this);
  }
}

// Forwards the early teardown request to both the embedded and detached WebUI
// toolbar web views to stop renderer script execution before IPC disconnection.
void ToolbarView::DestroyWebUIToolbarWebContents() {
  if (toolbar_webview_) {
    toolbar_webview_->DestroyWebContents();
  }
  if (detached_toolbar_webview_) {
    detached_toolbar_webview_->DestroyWebContents();
  }
}

void ToolbarView::Init() {
#if defined(USE_AURA)
  // Avoid generating too many occlusion tracking calculation events before this
  // function returns. The occlusion status will be computed only once once this
  // function returns.
  // See crbug.com/40171404#comment3
  aura::WindowOcclusionTracker::ScopedPause pause_occlusion;
#endif

  std::unique_ptr<LocationBarView> location_bar_view;
  std::unique_ptr<WebUILocationBar> webui_location_bar;
  if (features::IsWebUILocationBarEnabled() &&
      /* TODO(http://crbug.com/470042732): Figure out where we fit in other
       * modes. When doing this, we have to be careful of floating DevTools ---
       * that secretly has a hidden toolbar in location mode.*/
      display_mode_ == DisplayMode::kNormal) {
    webui_location_bar = std::make_unique<WebUILocationBar>(browser_, this);
  } else {
    location_bar_view = std::make_unique<LocationBarView>(
        browser_, browser_->GetProfile(),
        chrome::BrowserCommandController::From(browser_), this,
        display_mode_ != DisplayMode::kNormal);
  }

  // Make sure the toolbar shows by default.
  size_animation_.Reset(1);

  if (display_mode_ != DisplayMode::kNormal) {
    CHECK(location_bar_view)
        << "Alternate location bar impls need to handle this.";
    location_bar_view_ = AddChildView(std::move(location_bar_view));
    location_bar_ = location_bar_view_;
    location_bar_view_->Init();
  }

  if (display_mode_ == DisplayMode::kNormal) {
    SetBackground(std::make_unique<CustomCornersBackground>(
        *this, *browser_view_,
        /*primary_color=*/CustomCornersBackground::ToolbarTheme(),
        /*corner_color=*/CustomCornersBackground::FrameTheme()));
  } else if (display_mode_ == DisplayMode::kCustomTab) {
    custom_tab_bar_ =
        AddChildView(std::make_unique<CustomTabBarView>(browser_view_, this));
    SetLayoutManager(std::make_unique<views::FillLayout>());
    initialized_ = true;
    return;
  } else if (display_mode_ == DisplayMode::kLocation) {
    // Add the pinned toolbar actions container so that downloads can be shown
    // in popups.
    pinned_toolbar_actions_container_ = AddChildView(
        std::make_unique<PinnedToolbarActionsContainer>(browser_view_, this));
    pinned_toolbar_actions_ = pinned_toolbar_actions_container_;
    SetBackground(views::CreateSolidBackground(kColorLocationBarBackground));
    SetLayoutManager(std::make_unique<views::FlexLayout>())
        ->SetOrientation(views::LayoutOrientation::kHorizontal)
        .SetCrossAxisAlignment(views::LayoutAlignment::kCenter)
        .SetDefault(views::kFlexBehaviorKey,
                    views::FlexSpecification(
                        views::LayoutOrientation::kHorizontal,
                        views::MinimumFlexSizeRule::kPreferredSnapToZero))
        .SetFlexAllocationOrder(views::FlexAllocationOrder::kReverse);
    if (location_bar_view_) {
      location_bar_view_->SetProperty(
          views::kFlexBehaviorKey,
          views::FlexSpecification(views::LayoutOrientation::kHorizontal,
                                   views::MinimumFlexSizeRule::kScaleToZero,
                                   views::MaximumFlexSizeRule::kUnbounded));
    }
    initialized_ = true;
    return;
  }

  const auto callback = [](BrowserWindowInterface* browser, int command,
                           const ui::Event& event) {
    chrome::ExecuteCommandWithDisposition(
        browser, command, ui::DispositionFromEventFlags(event.flags()));
  };

  PrefService* const prefs = browser_->GetProfile()->GetPrefs();

  std::unique_ptr<ExtensionsToolbarDesktop> extensions_container;

  // Do not create the extensions or browser actions container if it is a guest
  // profile (only regular and incognito profiles host extensions).
  if (!browser_->GetProfile()->IsGuestSession() &&
      !features::IsWebUIExtensionsContainerEnabled()) {
    extensions_container = std::make_unique<ExtensionsToolbarDesktop>(browser_);
  }

  std::unique_ptr<MediaToolbarButtonView> media_button;
#if BUILDFLAG(IS_WIN) || BUILDFLAG(IS_MAC) || BUILDFLAG(IS_LINUX)
  if (!features::IsWebUIMediaButtonEnabled()) {
    media_button = std::make_unique<MediaToolbarButtonView>(
        browser_view_, std::make_unique<MediaToolbarButtonContextualMenu>(
                           browser_->GetProfile()));
  }
#endif

  // Always add children in order from left to right, for accessibility.

  vertical_tabs_collapse_button_ = AddChildView(std::make_unique<ToolbarButton>(
      base::BindRepeating(&ToolbarView::VerticalTabsCollapseButtonPressed,
                          base::Unretained(this))));
  vertical_tabs_collapse_button_->SetID(VIEW_ID_COLLAPSE_VERTICAL_TABS_BUTTON);
  vertical_tabs_collapse_button_->SetVisible(false);
  vertical_tabs_collapse_button_->SetProperty(
      views::kElementIdentifierKey,
      kToolbarVerticalTabsCollapseButtonElementId);

  if (!features::IsWebUIBackForwardButtonEnabled()) {
    back_ = AddChildView(std::make_unique<BackForwardButton>(
        BackForwardButton::Direction::kBack,
        base::BindRepeating(callback, browser_, IDC_BACK), browser_));
    forward_ = AddChildView(std::make_unique<BackForwardButton>(
        BackForwardButton::Direction::kForward,
        base::BindRepeating(callback, browser_, IDC_FORWARD), browser_));
  }

  if (base::FeatureList::IsEnabled(
          features::kWebUIToolbarProcessOverheadExperiment)) {
    detached_toolbar_webview_ = std::make_unique<WebUIToolbarWebView>(
        browser_, chrome::BrowserCommandController::From(browser_),
        /*location_bar=*/nullptr);
  } else if (features::IsWebUIToolbarEnabled()) {
    toolbar_webview_ = AddChildView(std::make_unique<WebUIToolbarWebView>(
        browser_, chrome::BrowserCommandController::From(browser_),
        std::move(webui_location_bar)));
  }

  if (!features::IsWebUIReloadButtonEnabled() ||
      base::FeatureList::IsEnabled(
          features::kWebUIToolbarProcessOverheadExperiment)) {
    reload_ = AddChildView(std::make_unique<ReloadButton>(
        browser_->GetProfile(),
        chrome::BrowserCommandController::From(browser_),
        InitialWebUIWindowMetricsManager::From(browser_)));
  }

  if (!features::IsWebUIHomeButtonEnabled()) {
    home_ = AddChildView(std::make_unique<HomeButton>(
        browser_, base::BindRepeating(callback, browser_, IDC_HOME)));
  }

  if (!features::IsWebUISplitTabsButtonEnabled()) {
    split_tabs_ =
        AddChildView(std::make_unique<SplitTabsToolbarButton>(browser_));
  }

  if (contextual_tasks::IsContextualTasksUIEnabled() &&
      contextual_tasks::kShowEntryPoint.Get() ==
          contextual_tasks::EntryPointOption::kToolbarEphemeralBranded) {
    auto button = std::make_unique<ContextualTasksButton>(browser_);
    auto* vts_controller =
        tabs::VerticalTabStripStateController::From(browser_);
    if ((!vts_controller || !vts_controller->ShouldDisplayVerticalTabs()) &&
        !(contextual_tasks::kEnableCircularEphemeralButtonNextToBatterySaver
              .Get() &&
          button->IsSidePanelRightAligned())) {
      button->SetProperty(views::kMarginsKey, gfx::Insets());
    }
    contextual_tasks_button_ = AddChildViewAt(std::move(button), 0);
  }

  if (location_bar_view) {
    auto search_button = std::make_unique<ToolbarButton>(
        base::BindRepeating(callback, browser_, IDC_FOCUS_LOCATION));
    search_button->SetTooltipText(
        l10n_util::GetStringUTF16(IDS_ACCNAME_LOCATION));
    search_button->GetViewAccessibility().SetName(
        l10n_util::GetStringUTF16(IDS_ACCNAME_LOCATION));
    search_button->SetVectorIcon(vector_icons::kSearchIcon);
    const int size = GetLayoutConstant(LayoutConstant::kToolbarButtonHeight);
    search_button->SetPreferredSize(gfx::Size(size, size));
    search_button->SetHorizontalAlignment(gfx::ALIGN_CENTER);
    search_button->SetVisible(false);
    search_button->SetProperty(
        views::kFlexBehaviorKey,
        views::FlexSpecification(views::MinimumFlexSizeRule::kPreferred,
                                 views::MaximumFlexSizeRule::kPreferred));
    noventra_search_button_ = AddChildView(std::move(search_button));
    location_bar_view_ = AddChildView(std::move(location_bar_view));
    location_bar_ = location_bar_view_;
    auto tools_button = std::make_unique<ToolbarButton>(base::BindRepeating(
        [](BrowserWindowInterface* browser) {
          chrome::ShowSettingsSubPage(browser, "appearance");
        }, base::Unretained(browser_)));
    tools_button->SetTooltipText(u"Customize Noventra");
    tools_button->GetViewAccessibility().SetName(u"Customize Noventra");
    tools_button->SetVectorIcon(vector_icons::kTuneIcon);
    tools_button->SetPreferredSize(gfx::Size(size, size));
    tools_button->SetHorizontalAlignment(gfx::ALIGN_CENTER);
    tools_button->SetProperty(
        views::kFlexBehaviorKey,
        views::FlexSpecification(views::MinimumFlexSizeRule::kPreferred,
                                 views::MaximumFlexSizeRule::kPreferred));
    noventra_tools_button_ = AddChildView(std::move(tools_button));
    auto bookmarks_button = std::make_unique<ToolbarButton>(
        base::BindRepeating(callback, browser_, IDC_SHOW_BOOKMARK_MANAGER));
    bookmarks_button->SetTooltipText(u"Bookmarks");
    bookmarks_button->GetViewAccessibility().SetName(u"Bookmarks");
    bookmarks_button->SetVectorIcon(kBookmarksSidePanelRefreshOldIcon);
    bookmarks_button->SetPreferredSize(gfx::Size(size, size));
    bookmarks_button->SetProperty(views::kFlexBehaviorKey,
        views::FlexSpecification(views::MinimumFlexSizeRule::kPreferred,
                                 views::MaximumFlexSizeRule::kPreferred));
    noventra_bookmarks_button_ = AddChildView(std::move(bookmarks_button));
  } else {
    location_bar_ = toolbar_webview_->GetLocationBar();
  }

  if (auto* vertical_tab_strip_state_controller =
          tabs::VerticalTabStripStateController::From(
              browser_view_->browser())) {
    vertical_tab_mode_subscription_ =
        vertical_tab_strip_state_controller->RegisterOnModeChanged(
            base::BindRepeating(&ToolbarView::OnVerticalTabStripModeChanged,
                                base::Unretained(this)));
    vertical_tab_collapse_subscription_ =
        vertical_tab_strip_state_controller->RegisterOnCollapseChanged(
            base::BindRepeating(&ToolbarView::OnVerticalTabStripCollapseChanged,
                                base::Unretained(this)));
    should_display_vertical_tabs_ =
        vertical_tab_strip_state_controller->ShouldDisplayVerticalTabs();
  }

  bool is_glic_left_of_profile =
      features::kGlicToolbarButtonLocationParam.Get() ==
          features::GlicToolbarButtonLocation::kLeftOfProfileChip ||
      features::kGlicToolbarButtonLocationParam.Get() ==
          features::GlicToolbarButtonLocation::kLeftOfProfileChipWithBackground;
  if (glic::GlicEnabling::IsProfileEligible(browser_view_->GetProfile()) &&
      !is_glic_left_of_profile) {
    if (features::IsWebUIGlicButtonEnabled() && toolbar_webview_) {
      glic_button_ = toolbar_webview_->GetGlicControl();
    } else {
      InitGlicContainer();

      glic_button_view_ = AddChildView(CreateGlicButton());
      glic_button_ = glic_button_view_;
      std::unique_ptr<ToolbarDivider> glic_button_divider =
          std::make_unique<ToolbarDivider>();
      glic_button_divider_ = AddChildView(std::move(glic_button_divider));
      glic_button_divider_->SetProperty(
          views::kMarginsKey,
          gfx::Insets::VH(
              0, GetLayoutConstant(LayoutConstant::kToolbarDividerSpacing)));
    }
  }

  std::unique_ptr<ToolbarButton> dynamic_new_tab_button =
      std::make_unique<DynamicNewTabToolbarButton>(base::BindRepeating(
          &ToolbarView::NewTabButtonPressed, base::Unretained(this)));
  const int dynamic_new_tab_button_size =
      GetLayoutConstant(LayoutConstant::kToolbarButtonHeight);
  dynamic_new_tab_button->SetPreferredSize(
      gfx::Size(dynamic_new_tab_button_size, dynamic_new_tab_button_size));
  dynamic_new_tab_button_ = AddChildView(std::move(dynamic_new_tab_button));
  dynamic_new_tab_button_->SetID(VIEW_ID_DYNAMIC_NEW_TAB_BUTTON);
  dynamic_new_tab_button_->SetTooltipText(
      l10n_util::GetStringUTF16(IDS_TOOLTIP_NEW_TAB));
  dynamic_new_tab_button_->GetViewAccessibility().SetName(
      l10n_util::GetStringUTF16(IDS_ACCNAME_NEWTAB));
  dynamic_new_tab_button_->SetVectorIcon(vector_icons::kAddWeight500CustomIcon);
  dynamic_new_tab_button_->SetHorizontalAlignment(gfx::ALIGN_CENTER);
  dynamic_new_tab_button_->SetVisible(false);
  dynamic_new_tab_button_->SetProperty(views::kElementIdentifierKey,
                                       kToolbarDynamicNewTabButtonElementId);

  if (extensions_container) {
    extensions_container_ = AddChildView(std::move(extensions_container));
    extensions_toolbar_coordinator_ =
        std::make_unique<ExtensionsToolbarCoordinator>(extensions_container_);
  }

  if (!features::IsWebUIPinnedToolbarActionsEnabled()) {
    pinned_toolbar_actions_container_ = AddChildView(
        std::make_unique<PinnedToolbarActionsContainer>(browser_view_, this));
    pinned_toolbar_actions_ = pinned_toolbar_actions_container_;
  } else {
    pinned_toolbar_actions_ = toolbar_webview_->GetPinnedToolbarActions();
  }

  if (IsChromeLabsEnabled()) {
    UpdateChromeLabsNewBadgePrefs(browser_->GetProfile());

    const bool should_show_chrome_labs_ui =
        ShouldShowChromeLabsUI(browser_->GetProfile());
    if (should_show_chrome_labs_ui) {
      show_chrome_labs_button_.Init(
          chrome_labs_prefs::kBrowserLabsEnabledEnterprisePolicy, prefs,
          base::BindRepeating(&ToolbarView::OnChromeLabsPrefChanged,
                              base::Unretained(this)));
      CHECK(!features::IsWebUIPinnedToolbarActionsEnabled())
          << "WebUIPinnedToolbarActions does not support ChromeLabs.";
      // Set the visibility for the button based on initial enterprise policy
      // value. Only call OnChromeLabsPrefChanged if there is a change from
      // the initial value.
      pinned_toolbar_actions_container_->GetActionItemFor(kActionShowChromeLabs)
          ->SetVisible(show_chrome_labs_button_.GetValue() &&
                       should_show_chrome_labs_ui);
    }
  }

  // Only show the Battery Saver button when it is not controlled by the OS. On
  // ChromeOS the battery icon in the shelf shows the same information.
  if (!performance_manager::user_tuning::IsBatterySaverModeManagedByOS() &&
      !features::IsWebUIBatterySaverButtonEnabled()) {
    battery_saver_button_ =
        AddChildView(std::make_unique<BatterySaverButton>(browser_));
  }

  if (!features::IsWebUIPerformanceInterventionButtonEnabled()) {
    performance_intervention_button_ =
        AddChildView(std::make_unique<PerformanceInterventionButton>(browser_));
  }

  if (features::IsWebUIMediaButtonEnabled()) {
    media_button_ = toolbar_webview_->GetMediaToolbarButton();
  } else if (media_button) {
    media_button_ = AddChildView(std::move(media_button));
  }

  if (glic::GlicEnabling::IsProfileEligible(browser_view_->GetProfile())) {
    if (base::FeatureList::IsEnabled(features::kAiOverlayDialog) &&
        ttc::AiOverlayDialogController::From(browser_)) {
      actions::ActionItem* action_item =
          actions::ActionManager::Get().FindAction(
              kActionShowAiOverlayDialog,
              BrowserActions::From(browser_)->root_action_item());
      if (action_item) {
        action_item->SetVisible(true);
        action_item->SetEnabled(true);
        PinnedToolbarActionsModel::Get(browser_->GetProfile())
            ->UpdatePinnedState(kActionShowAiOverlayDialog, true);
      }
    }
  }

  if (is_glic_left_of_profile &&
      glic::GlicEnabling::IsProfileEligible(browser_view_->GetProfile())) {
    if (features::IsWebUIGlicButtonEnabled() && toolbar_webview_) {
      glic_button_ = toolbar_webview_->GetGlicControl();
    } else {
      InitGlicContainer();

      glic_button_view_ = AddChildView(CreateGlicButton());
      glic_button_ = glic_button_view_;
      // The left margin is needed to ensure proper spacing before the
      // separator. The right margin is needed for spacing between the glic and
      // actor icons. The space between glic and profile should also be 5 but
      // that is handled by the profile margins.
      glic_button_view_->SetProperty(
          views::kMarginsKey, gfx::Insets()
                                  .set_left(kGlicButtonMargin)
                                  .set_right(kInsideBorderAroundGlicButtons));
    }
    UpdateGlicButtonVisibility();
  }

  if (!features::IsWebUIAvatarButtonEnabled()) {
    avatar_ = AddChildView(std::make_unique<AvatarToolbarButton>(browser_));
    bool show_avatar_toolbar_button =
        AvatarToolbarButtonInterface::CanShowForProfile(browser_->GetProfile());

    const std::string sab_value = base::CommandLine::ForCurrentProcess()->
                                  GetSwitchValueASCII("show-avatar-button");
    if (sab_value == "always")
      show_avatar_toolbar_button = true;
    else if (sab_value == "incognito-and-guest")
      show_avatar_toolbar_button = browser_->GetProfile()->IsIncognitoProfile() ||
                                   browser_->GetProfile()->IsGuestSession();
    else if (sab_value == "never")
      show_avatar_toolbar_button = false;

    avatar_->SetVisible(show_avatar_toolbar_button);
  }

  // Only manage the overflow button when there are at least some views
  // controls. If there aren't any, the WebUIToolbarWebView will handle all
  // layout and overflow itself, in Javascript.
  if (!features::IsWebUIToolbarFullyEnabled()) {
    overflow_button_ = AddChildView(std::make_unique<OverflowButton>());
    overflow_button_->SetVisible(false);
  }

  // WebUI app menu button handles these internally, so no need to set these
  // properties here, and the control is added as part of the WebUI toolbar.
  if (!features::IsWebUIAppMenuButtonEnabled()) {
    auto app_menu_button = std::make_unique<BrowserAppMenuButton>(this);
    app_menu_button->SetFlipCanvasOnPaintForRTLUI(true);
    app_menu_button->GetViewAccessibility().SetName(
        l10n_util::GetStringUTF16(IDS_ACCNAME_APP));
    app_menu_button->SetTooltipText(
        l10n_util::GetStringUTF16(IDS_APPMENU_TOOLTIP));
    app_menu_button->SetID(VIEW_ID_APP_MENU);
    app_menu_button_ = AddChildView(std::move(app_menu_button));
  }

  if (base::FeatureList::IsEnabled(contextual_tasks::kContextualTasks) &&
      contextual_tasks::GetExpandButtonOption() ==
          contextual_tasks::ExpandButtonOption::kToolbarCloseButton) {
    AddChildView(std::make_unique<ContextualTasksCloseTabButton>(browser_));
  }

  if (contextual_tasks_button_) {
    if (auto* const controller =
            ContextualTasksEphemeralButtonController::From(browser_)) {
      contextual_tasks_button_position_subscription_ =
          controller->RegisterShouldUpdateButtonPosition(
              base::BindRepeating(&ToolbarView::PositionContextualTasksButton,
                                  base::Unretained(this)));
    }
    PositionContextualTasksButton();
  }

  LoadImages();

  // Set the button icon based on the system state. Do this after
  // |app_menu_button_| has been added as a bubble may be shown that needs
  // the widget (widget found by way of app_menu_button_->GetWidget()).
  app_menu_icon_controller_.UpdateDelegate();

  if (location_bar_view_) {
    location_bar_view_->Init();
  } else {
    toolbar_webview_->GetLocationBar()->Init(toolbar_webview_.get());
  }

  show_forward_button_.Init(
      prefs::kShowForwardButton, prefs,
      base::BindRepeating(&ToolbarView::OnShowForwardButtonChanged,
                          base::Unretained(this)));

  SetForwardButtonVisibility(show_forward_button_.GetValue());

  show_home_button_.Init(
      prefs::kShowHomeButton, prefs,
      base::BindRepeating(&ToolbarView::OnShowHomeButtonChanged,
                          base::Unretained(this)));

  if (home_) {
    home_->SetVisible(show_home_button_.GetValue());
  }

  if (glic::GlicEnabling::IsProfileEligible(browser_view_->GetProfile())) {
    UpdateGlicButtonVisibility();
  }

  show_back_button_.Init(
      prefs::kShowBackButton, prefs,
      base::BindRepeating(&ToolbarView::OnShowBackButtonChanged,
                          base::Unretained(this)));

  back_->SetVisible(show_back_button_.GetValue());

  show_reload_button_.Init(
      prefs::kShowReloadButton, prefs,
      base::BindRepeating(&ToolbarView::OnShowReloadButtonChanged,
                          base::Unretained(this)));

  reload_->SetVisible(show_reload_button_.GetValue());

  show_vertical_tabs_collapse_button_.Init(
      prefs::kShowVerticalTabsCollapseButton, prefs,
      base::BindRepeating(&ToolbarView::UpdateToolbarLayout,
                          base::Unretained(this)));

  show_dynamic_new_tab_button_.Init(
      prefs::kShowDynamicNewTabButton, prefs,
      base::BindRepeating(&ToolbarView::UpdateToolbarLayout,
                          base::Unretained(this)));

  show_avatar_button_.Init(
      prefs::kShowAvatarButton, prefs,
      base::BindRepeating(&ToolbarView::OnShowAvatarButtonChanged,
                          base::Unretained(this)));

  // Avatar is always shown in incognito and guest profiles,
  // but in regular profiles it's managed by kShowAvatarButton pref.
  if (browser_->GetProfile()->IsRegularProfile() &&
      !base::CommandLine::ForCurrentProcess()->HasSwitch(
          "show-avatar-button")) {
    avatar_->SetVisible(show_avatar_button_.GetValue());
  }

  if (extensions_container_) {
    show_extensions_button_.Init(
        prefs::kShowExtensionsButton, prefs,
        base::BindRepeating(&ToolbarView::OnShowExtensionsButtonChanged,
                            base::Unretained(this)));
    extensions_container_->SetVisible(show_extensions_button_.GetValue());
  }

  if (app_menu_button_) {
    show_menu_button_.Init(
        prefs::kShowMenuButton, prefs,
        base::BindRepeating(&ToolbarView::OnShowMenuButtonChanged,
                            base::Unretained(this)));

    app_menu_button_->SetVisible(show_menu_button_.GetValue());
  }

  if (media_button_) {
    show_media_button_.Init(
        prefs::kShowMediaButton, prefs,
        base::BindRepeating(&ToolbarView::OnShowMediaButtonChanged,
                            base::Unretained(this)));

    OnShowMediaButtonChanged();
  }

  location_bar_is_centered_.Init(
      prefs::kHeliumCenteredLocationBar, prefs,
      base::BindRepeating(&ToolbarView::OnLocationBarCenteredChanged,
                          base::Unretained(this)));

  InitLayout();

  auto flex_preferred =
      views::FlexSpecification(views::MinimumFlexSizeRule::kPreferred,
                               views::MaximumFlexSizeRule::kPreferred);

  for (auto* button : std::array<views::Button*, 7>{
           vertical_tabs_collapse_button_, back_, forward_, reload_, home_,
           dynamic_new_tab_button_, avatar_}) {
    if (button) {
      button->set_tag(GetViewCommandMap().at(button->GetID()));
      if (button == back_ || button == reload_) {
        button->SetProperty(views::kFlexBehaviorKey, flex_preferred);
      }
    }
  }

  if (auto* glic_split_button_controller =
          glic::GlicSplitButtonController::From(browser_)) {
    glic_split_button_controller_ = glic_split_button_controller->GetWeakPtr();
    glic_split_button_controller_->SetVerticalTabsDelegate(this);
  }

  // Track rather than retain raw pointers: optional controls can be removed.
  // Restore this canonical order when leaving the inline-address layout.
  auto original_order = std::make_unique<NoventraToolbarOrder>();
  for (views::View* child : children()) {
    original_order->push_back(std::make_unique<views::ViewTracker>(child));
  }
  SetProperty(kNoventraToolbarOrderKey, std::move(original_order));
  initialized_ = true;
  noventra_sidebar_controls_.Init(
      "noventra.browser.sidebar_controls", prefs,
      base::BindRepeating([](ToolbarView* view) {
        view->UpdateNoventraTabOrder();
        view->UpdateToolbarLayout();
        view->browser_view_->InvalidateLayout();
      }, base::Unretained(this)));
  noventra_address_on_demand_.Init(
      "noventra.browser.address_on_demand", prefs,
      base::BindRepeating([](ToolbarView* view) {
        view->UpdateNoventraTabOrder();
        view->UpdateToolbarLayout();
      }, base::Unretained(this)));
  noventra_show_navigation_buttons_.Init(
      "noventra.browser.show_navigation_buttons", prefs,
      base::BindRepeating(&ToolbarView::UpdateToolbarLayout,
                          base::Unretained(this)));
  noventra_location_observation_.Observe(location_bar_);
  UpdateToolbarLayout();
  UpdateNoventraTabOrder();
}

void ToolbarView::InitGlicContainer() {
  if (base::FeatureList::IsEnabled(features::kGlicActorUi) &&
      features::kGlicActorUiTaskIcon.Get()) {
    glic_actor_button_container_ =
        AddChildView(CreateGlicActorButtonContainer());
    glic_actor_task_icon_ =
        glic_actor_button_container_->AddChildView(CreateGlicActorTaskIcon());
    glic_actor_button_container_->SetVisible(false);
    glic_actor_task_icon_->SetVisible(false);
  }
}

void ToolbarView::OnVerticalTabStripModeChanged(
    tabs::VerticalTabStripStateController* controller) {
  should_display_vertical_tabs_ = controller->ShouldDisplayVerticalTabs();
  UpdateGlicButtonVisibility();
  UpdateGlicActorVisibility();
  UpdateToolbarLayout();
  // Invalidate the layout cache so responsive buttons (forward, home, split,
  // etc) re-evaluate their visibility against the final toolbar width, clearing
  // out any zeroed out state from intermediate layout passes.
  InvalidateLayout();
}

void ToolbarView::OnVerticalTabStripCollapseChanged(
    tabs::VerticalTabStripCollapseState) {
  UpdateNoventraTabOrder();
  UpdateToolbarLayout();
  browser_view_->InvalidateLayout();
}

std::unique_ptr<GlicAndActorButtonsContainer>
ToolbarView::CreateGlicActorButtonContainer() {
  auto glic_actor_button_container =
      std::make_unique<GlicAndActorButtonsContainer>();

  // Should be hidden until a task starts.
  glic_actor_button_container->SetVisible(false);

  return glic_actor_button_container;
}

std::unique_ptr<glic::ToolbarGlicActorTaskIcon>
ToolbarView::CreateGlicActorTaskIcon() {
  std::unique_ptr<glic::ToolbarGlicActorTaskIcon> glic_actor_task_icon =
      std::make_unique<glic::ToolbarGlicActorTaskIcon>(
          browser_view_->browser(),
          base::BindRepeating(&ToolbarView::OnGlicActorTaskIconClicked,
                              base::Unretained(this)));

  // Add a MenuButtonController in order to keep the task icon pressed while the
  // bubble is visible.
  glic_actor_task_icon->SetButtonController(
      std::make_unique<views::MenuButtonController>(
          glic_actor_task_icon.get(),
          base::BindRepeating(&ToolbarView::OnGlicActorTaskIconClicked,
                              base::Unretained(this)),
          std::make_unique<views::Button::DefaultButtonControllerDelegate>(
              glic_actor_task_icon.get())));

  glic_actor_task_icon->SetProperty(views::kCrossAxisAlignmentKey,
                                    views::LayoutAlignment::kCenter);

  if (base::FeatureList::IsEnabled(features::kToolbarGlicButtonResizing)) {
    glic_actor_task_icon->SetProperty(
        views::kFlexBehaviorKey,
        views::FlexSpecification(
            views::LayoutOrientation::kHorizontal,
            views::MinimumFlexSizeRule::kPreferredSnapToMinimum,
            views::MaximumFlexSizeRule::kPreferred));
  }

  return glic_actor_task_icon;
}

void ToolbarView::OnGlicActorTaskIconClicked() {
  Profile* const profile = browser_view_->GetProfile();
  auto* activity_manager = glic::GlicActivityManager::Get(profile);
  if (!activity_manager) {
    return;
  }

  ActorTaskListBubbleController* controller =
      ActorTaskListBubbleController::From(browser_view_->browser());
  // Only show the bubble if the button is not currently pressed. Clicking on
  // the pressed button should dismiss the nudge.
  if (!glic_actor_task_icon_->GetIsPressed()) {
    controller->ShowBubble(glic_actor_task_icon_);
  }

  auto current_task_nudge_state =
      activity_manager->GetCurrentActorTaskNudgeState();
  actor::ui::LogGlobalTaskIndicatorClick(current_task_nudge_state);
}

std::unique_ptr<glic::ToolbarGlicButton> ToolbarView::CreateGlicButton() {
  glic::GlicKeyedService* service =
      glic::GlicKeyedService::Get(browser_view_->GetProfile());
  std::u16string tooltip_text = l10n_util::GetStringUTF16(
      service->instance_coordinator().IsAnyPanelShowing()
          ? IDS_GLIC_TAB_STRIP_BUTTON_TOOLTIP_CLOSE
          : IDS_GLIC_TAB_STRIP_BUTTON_TOOLTIP);
  std::unique_ptr<glic::ToolbarGlicButton> glic_button =
      std::make_unique<glic::ToolbarGlicButton>(
          browser_view_->browser(),
          base::BindRepeating(&ToolbarView::OnGlicButtonAnimationEnded,
                              base::Unretained(this)),
          tooltip_text,
          base::BindRepeating(&ToolbarView::OnGlicButtonClicked,
                              base::Unretained(this)));

  glic_button->SetProperty(views::kCrossAxisAlignmentKey,
                           views::LayoutAlignment::kCenter);

  return glic_button;
}

void ToolbarView::OnGlicButtonClicked() {
  CHECK(glic_button_view_);
  glic::GlicSplitButtonController::From(browser_view_->browser())
      ->OnGlicButtonClicked();

  ExecuteHideToolbarNudge(glic_button_view_);
  // Reset state manually since there wont be a mouse up event as the
  // animation moves the button out of the way.
  glic_button_view_->SetState(views::Button::ButtonState::STATE_NORMAL);
}

void ToolbarView::OnGlicButtonDismissed() {
  CHECK(glic_button_view_);
  CHECK(glic_split_button_controller_);
  glic::GlicNudgeController* nudge_controller =
      glic_split_button_controller_->nudge_controller();
  CHECK(nudge_controller);
  nudge_controller->OnNudgeActivity(glic::GlicNudgeActivity::kNudgeDismissed);

  // Force hide the button when pressed, bypassing locked expansion mode.
  ExecuteHideToolbarNudge(glic_button_view_);
}

void ToolbarView::OnGlicButtonAnimationEnded() {
  // TODO(crbug.com/484389669): ToolbarGlicButton animations
  return;
}

void ToolbarView::ShowToolbarNudge(glic::GlicButtonInterface* button) {
  if (IsMouseHovered()) {
    SetLockedExpansionMode(ExpansionMode::kWillShow, button);
    return;
  }
  if (locked_expansion_mode_ == ExpansionMode::kNone) {
    ExecuteShowToolbarNudge(button);
  }
}

void ToolbarView::HideToolbarNudge(glic::GlicButtonInterface* button) {
  if (this->IsMouseHovered()) {
    SetLockedExpansionMode(ExpansionMode::kWillHide, button);
    return;
  }
  if (locked_expansion_mode_ == ExpansionMode::kNone) {
    ExecuteHideToolbarNudge(button);
  }
}

bool ToolbarView::GetIsShowingGlicNudge() {
  return glic_button_ && glic_button_->GetIsShowingNudge();
}

bool ToolbarView::GetIsShowingGlicActorTaskIconNudge() {
  return glic_actor_task_icon_ && glic_actor_task_icon_->GetIsShowingNudge();
}

void ToolbarView::OnTriggerGlicNudgeUI(glic::NudgeParams params) {
  if (GetIsShowingGlicActorTaskIconNudge()) {
    return;
  }

  if (!glic_button_) {
    return;
  }
  if (!params.label.empty()) {
    glic_button_->SetNudgeLabel(std::move(params.label));
    if (glic_button_view_) {
      ShowToolbarNudge(glic_button_view_);
    } else {
      glic_button_->SetIsShowingNudge(true);
    }
  }
}

void ToolbarView::OnHideGlicNudgeUI() {
  if (glic_button_view_) {
    HideToolbarNudge(glic_button_view_);
  } else if (glic_button_) {
    glic_button_->SetIsShowingNudge(false);
  }
}

void ToolbarView::SetGlicActorNudgeLabel(const std::u16string& nudge_label) {
  if (glic_actor_task_icon_) {
    glic_actor_task_icon_->ShowNudgeLabel(nudge_label);
  }
}

void ToolbarView::TriggerGlicActorNudge(const std::u16string& nudge_label) {
  if (!glic_button_view_ || !glic_actor_task_icon_) {
    return;
  }
  if (GetIsShowingGlicNudge()) {
    // If the glic button is showing, start the hide animation in parallel to
    // the show actor nudge animation.
    HideToolbarNudge(glic_button_view_);
    OnGlicButtonAnimationEnded();
  }
  ShowGlicActorNudge(nudge_label);
}

void ToolbarView::ShowGlicActorNudge(const std::u16string nudge_label) {
  if (!glic_button_view_ || !glic_actor_task_icon_) {
    return;
  }
  // Start animation for minimizing the glic button.
  glic_button_view_->Collapse();
  ShowGlicActorTaskIcon();
  glic_actor_task_icon_->ShowNudgeLabel(nudge_label);
  ShowToolbarNudge(glic_actor_task_icon_);
}

void ToolbarView::ShowGlicActorTaskIcon() {
  if (!glic_button_view_ || !glic_actor_task_icon_ ||
      !glic_actor_button_container_) {
    return;
  }
  // If the nudge is showing (ex: previous state was CheckTasks), hide the
  // nudge.
  if (glic_actor_task_icon_->GetIsShowingNudge()) {
    HideToolbarNudge(glic_actor_task_icon_);
    return;
  }
  glic_button_view_ =
      glic_actor_button_container_->InsertGlicButton(glic_button_view_.get());
  glic_button_ = glic_button_view_;
  SetGlicActorShowState(true);
  SetGlicShowState(true);
  glic_button_view_->Collapse();
  glic_button_view_->SetSplitButtonCornerStyling();
  UpdateGlicActorButtonContainerBorders();

  if (glic_actor_task_icon_->GetAnimationMode() ==
      glic::AnimationMode::kEntry) {
    // TODO(crbug.com/484389669): Create animation session to being animation of
    // nudge.
    glic_actor_task_icon_->SetAnimationMode(glic::AnimationMode::kNudge);
    glic_actor_task_icon_->SetWidthFactor(1.0);
  }
}

void ToolbarView::HideGlicActorTaskIcon() {
  if (!glic_actor_task_icon_) {
    return;
  }

  // If it's already hidden, do nothing.
  if (!glic_actor_task_icon_->GetVisible() &&
      !glic_actor_task_icon_->GetIsShowingNudge()) {
    return;
  }
  glic_actor_task_icon_->SetIsShowingNudge(false);

  // TODO(crbug.com/484389669): Toolbar glic actor animations
  if (glic_actor_task_icon_->GetAnimationMode() ==
      glic::AnimationMode::kNudge) {
    // TODO(crbug.com/484389669): Create animation session to being animation of
    // nudge.
    glic_actor_task_icon_->SetAnimationMode(glic::AnimationMode::kEntry);
    glic_actor_task_icon_->SetWidthFactor(0.0);
  }

  FinalizeHideGlicActorTaskIcon();
}

void ToolbarView::SetGlicActorNudgePressedState(bool pressed) {
  if (glic_actor_task_icon_) {
    glic_actor_task_icon_->SetPressedState(pressed);
  }
}

void ToolbarView::ShowActorTaskListBubble() {
  if (!glic_actor_task_icon_) {
    return;
  }
  if (!actor_task_list_bubble_) {
    Profile* profile = browser_->GetProfile();
    auto* manager = glic::GlicActivityManager::Get(profile);
    auto* controller = ActorTaskListBubbleController::From(browser_);
    if (manager && controller) {
      actor_task_list_bubble_ = std::make_unique<ActorTaskListBubble>(
          profile, browser_, manager->actor_task_list_bubble_rows(),
          base::BindRepeating(&ActorTaskListBubbleController::OnTaskRowClicked,
                              base::Unretained(controller)));
    }
  }
  if (actor_task_list_bubble_) {
    actor_task_list_bubble_->Show(glic_actor_task_icon_);
  }
}

void ToolbarView::CloseActorTaskListBubble() {
  if (actor_task_list_bubble_) {
    actor_task_list_bubble_->Close();
  }
}

bool ToolbarView::IsActorTaskListBubbleShowing() {
  return actor_task_list_bubble_ && actor_task_list_bubble_->IsShowing();
}

void ToolbarView::FinalizeHideGlicActorTaskIcon() {
  if (!glic_button_view_ || !glic_actor_task_icon_ ||
      !glic_actor_button_container_) {
    return;
  }
  // Reset Nudge State
  if (glic_actor_task_icon_->GetIsShowingNudge()) {
    // TODO(crbug.com/484389669): Glic actor nudge animation
    glic_actor_task_icon_->SetIsShowingNudge(false);
  }
  glic_actor_task_icon_->SetVisible(false);
  glic_actor_task_icon_->SetTaskIconToDefault();

  size_t insertion_index = GetIndexOf(avatar_.get()).value();
  if (glic_button_divider_) {
    insertion_index = GetIndexOf(glic_button_divider_).value();
  }
  glic_button_view_ = AddChildViewAt(glic_button_view_.get(), insertion_index);
  glic_button_ = glic_button_view_;
  glic_actor_button_container_->SetVisible(false);
  glic_button_view_->Expand();
  glic_button_view_->ResetSplitButtonCornerStyling();
  // Reset the animation mode for the next time the icon is shown.
  glic_actor_task_icon_->SetAnimationMode(glic::AnimationMode::kEntry);
  UpdateGlicActorButtonContainerBorders();
}

void ToolbarView::UpdateGlicActorButtonContainerBorders() {
  if (!glic_button_view_) {
    return;
  }

  // Force a background repaint.
  if (glic_actor_task_icon_) {
    glic_actor_task_icon_->RefreshBackground();
  }
  glic_button_view_->RefreshBackground();
}

void ToolbarView::ExecuteShowToolbarNudge(glic::GlicButtonInterface* button) {
  // TODO(crbug.com/): Fix cases where we can't show modal ui during animation
  // session.
  button->SetIsShowingNudge(true);

  // Only change the margins between the GlicButton and nudges that are NOT
  // coming from the GlicActorTaskIcon.
  if (glic_button_view_ && glic_button_view_->GetVisible() &&
      button != glic_button_view_ && button != glic_actor_task_icon_) {
    const int space_between_buttons = kLargeSpaceBetweenButtons;
    gfx::Insets margin;
    margin.set_right(space_between_buttons);
    button->GetPropertyHandler()->SetProperty(views::kMarginsKey, margin);
  } else {
    // Reset the margins.
    button->GetPropertyHandler()->SetProperty(views::kMarginsKey,
                                              gfx::Insets());
  }
}

void ToolbarView::ExecuteHideToolbarNudge(glic::GlicButtonInterface* button) {
  if (!button->GetVisible()) {
    return;
  }

  // Since the glic button is still visible in it's hidden state we need to have
  // a special case to query if it's in its Hide state.
  if (button == glic_button_view_ && button->GetWidthFactor() == 0.0) {
    return;
  }

  button->SetIsShowingNudge(false);
}

void ToolbarView::UpdateGlicActorVisibility() {
  if (!glic_actor_task_icon_) {
    return;
  }

  bool is_glic_actor_visible =
      should_show_glic_actor_ &&
      (should_display_vertical_tabs_ ||
       base::FeatureList::IsEnabled(features::kGlicHorizontalTabToolbarButton));

  glic_actor_task_icon_->SetVisible(is_glic_actor_visible);
  if (glic_button_) {
    bool is_glic_left_of_profile =
        base::FeatureList::IsEnabled(features::kGlicToolbarButtonLocation) &&
        features::kGlicToolbarButtonLocationParam.Get() ==
            features::GlicToolbarButtonLocation::kLeftOfProfileChip;
    glic_button_->UpdateStyle(is_glic_left_of_profile &&
                              !is_glic_actor_visible);
  }
}

void ToolbarView::UpdateGlicButtonVisibility() {
  if (!glic_button_) {
    return;
  }

  bool is_glic_visible =
      should_show_glic_button_ &&
      (should_display_vertical_tabs_ ||
       base::FeatureList::IsEnabled(features::kGlicHorizontalTabToolbarButton));

  glic_button_->SetVisible(is_glic_visible);
  if (glic_button_divider_) {
    glic_button_divider_->SetVisible(is_glic_visible);
  }

  if (glic_actor_button_container_) {
    // glic_actor_button_container_ should only be visible at the same time as
    // glic_button_.
    glic_actor_button_container_->SetVisible(is_glic_visible);
  }
  bool is_glic_left_of_profile =
      base::FeatureList::IsEnabled(features::kGlicToolbarButtonLocation) &&
      features::kGlicToolbarButtonLocationParam.Get() ==
          features::GlicToolbarButtonLocation::kLeftOfProfileChip;
  bool is_task_icon_visible =
      glic_actor_task_icon_ && glic_actor_task_icon_->GetVisible();
  glic_button_->UpdateStyle(is_glic_left_of_profile && !is_task_icon_visible);
}

void ToolbarView::SetGlicActorShowState(bool show) {
  should_show_glic_actor_ = show;
  UpdateGlicActorVisibility();
}

void ToolbarView::SetGlicShowState(bool show) {
  should_show_glic_button_ = show;
  UpdateGlicButtonVisibility();
}

void ToolbarView::SetGlicPanelIsOpen(bool open) {
  if (!glic_button_) {
    return;
  }

  glic_button_->SetGlicPanelIsOpen(open);
}

void ToolbarView::MouseMovedOutOfHost() {
  SetLockedExpansionMode(ExpansionMode::kNone, /*button=*/nullptr);
}

void ToolbarView::SetLockedExpansionMode(ExpansionMode mode,
                                         glic::GlicButtonInterface* button) {
  if (mode == ExpansionMode::kNone) {
    if (locked_expansion_mode_ == ExpansionMode::kWillShow) {
      ExecuteShowToolbarNudge(locked_expansion_button_);
    } else if (locked_expansion_mode_ == ExpansionMode::kWillHide) {
      ExecuteHideToolbarNudge(locked_expansion_button_);
    }
    locked_expansion_button_ = nullptr;
  } else {
    locked_expansion_button_ = button;
    mouse_watcher_->Start(GetWidget()->GetNativeWindow());
  }
  locked_expansion_mode_ = mode;
}

void ToolbarView::AnimationEnded(const gfx::Animation* animation) {
  if (animation->GetCurrentValue() == 0) {
    SetToolbarVisibility(false);
  }
  BrowserWindow::FromBrowser(browser())->ToolbarSizeChanged(
      /*is_animating=*/false);
}

void ToolbarView::AnimationProgressed(const gfx::Animation* animation) {
  BrowserWindow::FromBrowser(browser())->ToolbarSizeChanged(
      /*is_animating=*/true);
}

void ToolbarView::Update(WebContents* tab) {
  if (location_bar_) {
    location_bar_->Update(tab);
  }

  if (custom_tab_bar_) {
    custom_tab_bar_->UpdateContents();
  }

  if (extensions_container_) {
    extensions_container_->UpdateAllIcons();
  }

  if (pinned_toolbar_actions_container_) {
    pinned_toolbar_actions_container_->UpdateAllIcons();
  }

  if (ReloadControl* reload_control = GetReloadButton(); reload_control) {
    reload_control->SetDevToolsStatus(
        chrome::IsDebuggerAttachedToCurrentTab(browser_));
  }
}

bool ToolbarView::UpdateSecurityState() {
  if ((location_bar_ && location_bar_->HasSecurityStateChanged()) ||
      (custom_tab_bar_ && custom_tab_bar_->HasSecurityStateChanged())) {
    Update(nullptr);
    return true;
  }

  return false;
}

void ToolbarView::SetToolbarVisibility(bool visible) {
  SetVisible(visible);
  views::View* bar = display_mode_ == DisplayMode::kCustomTab
                         ? static_cast<views::View*>(custom_tab_bar_)
                         : static_cast<views::View*>(location_bar_view_);
  CHECK(bar) << "Alternate location bar impls need to handle this.";
  bar->SetVisible(visible);
}

void ToolbarView::UpdateCustomTabBarVisibility(bool visible, bool animate) {
  DCHECK_EQ(display_mode_, DisplayMode::kCustomTab);

  if (!animate) {
    size_animation_.Reset(visible ? 1.0 : 0.0);
    SetToolbarVisibility(visible);
    BrowserWindow::FromBrowser(browser())->ToolbarSizeChanged(
        /*is_animating=*/false);
    return;
  }

  if (visible) {
    SetToolbarVisibility(true);
    size_animation_.Show();
  } else {
    size_animation_.Hide();
  }
}

void ToolbarView::ResetTabState(WebContents* tab) {
  if (location_bar_) {
    location_bar_->ResetTabState(tab);
  }
}

void ToolbarView::SetPaneFocusAndFocusAppMenu() {
  AppMenuControl* app_menu_control = GetAppMenuControl();
  if (app_menu_control) {
    app_menu_control->Focus(GetAsAccessiblePaneView());
  }
}

bool ToolbarView::GetAppMenuFocused() const {
  const AppMenuControl* app_menu_control = GetAppMenuControl();
  return app_menu_control && app_menu_control->HasFocus();
}

bool ToolbarView::IsRectInWindowCaption(const gfx::Rect& rect) const {
  // Clickable elements are child views of the toolbar. If the toolbar is not
  // visible, then the rect is in the window caption.
  if (!GetVisible()) {
    return true;
  }

  const auto get_target_rect = [&](views::View* target) {
    gfx::RectF rect_in_target_coords_f(rect);
    views::View::ConvertRectToTarget(this, target, &rect_in_target_coords_f);
    return gfx::ToEnclosingRect(rect_in_target_coords_f);
  };

  // Check the tab strip region first if compact layout is enabled.
  if (IsCompactLayout() && tab_strip_region_view_) {
    const gfx::Rect tab_strip_rect = get_target_rect(tab_strip_region_view_);
    if (tab_strip_region_view_->GetLocalBounds().Intersects(tab_strip_rect)) {
      // Check if the rect is actually on the tab strip region view.
      if (tab_strip_region_view_->HitTestRect(tab_strip_rect)) {
        if (tab_strip_region_view_->IsPositionInWindowCaption(
                tab_strip_rect.origin())) {
          return true;
        }

        return false;
      }
    }
  }

  // Check each child view to see if the rect intersects with
  // any clickable elements. If it does, check if the click is actually on that
  // element. False if on a clickable element, true if not on a clickable element.
  for (views::View* const child : children()) {
    if (!child->GetVisible()) {
      continue;
    }

    const gfx::Rect target_rect = get_target_rect(child);
    if (child->GetLocalBounds().Intersects(target_rect)) {
      // Check if the rect is actually on the child.
      if (child->HitTestRect(target_rect)) {
        return false;
      }
    }
  }

  // The rect is not on any clickable element, so it's in the window caption.
  return true;
}

void ToolbarView::ShowIntentPickerBubble(
    std::vector<apps::IntentPickerAppInfo> app_info,
    bool show_stay_in_chrome,
    bool show_remember_selection,
    apps::IntentPickerBubbleType bubble_type,
    const std::optional<url::Origin>& initiating_origin,
    IntentPickerResponse callback) {
  std::optional<ui::ElementIdentifier> higlighted_element;
  if (GetPageActionViewInterface(kActionShowIntentPicker)) {
    higlighted_element = kIntentPickerPageActionElementId;
  } else {
    return;
  }

  IntentPickerBubbleView::ShowBubble(
      GetBubbleAnchor(std::nullopt), higlighted_element, bubble_type,
      GetWebContents(), std::move(app_info), show_stay_in_chrome,
      show_remember_selection, initiating_origin, std::move(callback));
}

void ToolbarView::ShowBookmarkBubble(const GURL& url, bool already_bookmarked) {
  page_actions::PageActionViewInterface* bookmark_star_icon = nullptr;
  if (!features::IsWebUILocationBarEnabled()) {
    bookmark_star_icon = GetPageActionViewInterface(kActionBookmarkThisTab);
    CHECK(bookmark_star_icon);
  }
  BookmarkBubbleView::ShowBubble(GetBubbleAnchor(std::nullopt),
                                 GetWebContents(), bookmark_star_icon, browser_,
                                 url, already_bookmarked);
}

bool ToolbarView::IsPositionInWindowCaption(
    const gfx::Point& test_point) const {
  return IsRectInWindowCaption(gfx::Rect(test_point, gfx::Size(1, 1)));
}

void ToolbarView::RecordHitTestMetrics(bool is_caption_area) {
  const bool is_mouse_down = GetWidget() && GetWidget()->IsMouseButtonDown();

  if (!is_mouse_down) {
    was_mouse_down_ = false;
  } else if (!was_mouse_down_) {
    was_mouse_down_ = true;
    if (browser_view_->ShouldDrawVerticalTabStrip()) {
      if (is_caption_area) {
        base::RecordAction(
            base::UserMetricsAction("VerticalTabs.Toolbar.CaptionAreaPressed"));
      } else {
        base::RecordAction(base::UserMetricsAction(
            "VerticalTabs.Toolbar.InteractiveAreaPressed"));
      }
    }
  }
}

views::Button* ToolbarView::GetChromeLabsButton() const {
  ChromeLabsCoordinator* coordinator = ChromeLabsCoordinator::From(browser_);
  return coordinator ? coordinator->GetChromeLabsButton() : nullptr;
}

views::Button* ToolbarView::tab_search_button() {
  if (!tab_search_button_ && pinned_toolbar_actions_container_) {
    tab_search_button_ =
        pinned_toolbar_actions_container_->CreatePermanentButtonFor(
            kActionTabSearch);
  }
  return tab_search_button_;
}

ExtensionsToolbarButton* ToolbarView::GetExtensionsButton() const {
  return extensions_container_->GetExtensionsButton();
}

views::LabelButton* ToolbarView::GetGlicButton() {
  return glic_button_view_;
}

////////////////////////////////////////////////////////////////////////////////
// ToolbarView, LocationBarView::Delegate implementation:

WebContents* ToolbarView::GetWebContents() {
  return browser_->GetTabStripModel()->GetActiveWebContents();
}

LocationBarModel* ToolbarView::GetLocationBarModel() {
  return LocationBarModel::From(browser_.get());
}

const LocationBarModel* ToolbarView::GetLocationBarModel() const {
  return LocationBarModel::From(browser_.get());
}

ContentSettingBubbleModelDelegate*
ToolbarView::GetContentSettingBubbleModelDelegate() {
  return BrowserContentSettingBubbleModelDelegate::From(browser_);
}

////////////////////////////////////////////////////////////////////////////////
// ToolbarView, CommandObserver implementation:

void ToolbarView::EnabledStateChangedForCommand(int id, bool enabled) {
  DCHECK(display_mode_ == DisplayMode::kNormal);

  if ((id == IDC_BACK || id == IDC_FORWARD) &&
      features::IsWebUIBackForwardButtonEnabled()) {
    toolbar_webview_->SetBackForwardEnabled(id, enabled);
    return;
  }

  const std::array<views::Button*, 5> kButtons{back_, forward_, reload_, home_,
                                               avatar_};
  auto it = std::ranges::find_if(
      kButtons, [id](views::Button* b) { return b && b->tag() == id; });
  if (it != kButtons.end()) {
    (*it)->SetEnabled(enabled);
  }
}

////////////////////////////////////////////////////////////////////////////////
// ToolbarView, ui::AcceleratorProvider implementation:

bool ToolbarView::GetAcceleratorForCommandId(
    int command_id,
    ui::Accelerator* accelerator) const {
  return GetWidget()->GetAccelerator(command_id, accelerator);
}

////////////////////////////////////////////////////////////////////////////////
// ToolbarView, views::View overrides:

gfx::Size ToolbarView::CalculatePreferredSize(
    const views::SizeBounds& available_size) const {
  if (noventra_docking_) {
    bool has_controls = false;
    for (views::View* child : children())
      if (child != toolbar_divider_ && child != vertical_tabs_collapse_button_ &&
          child->GetVisible() && !child->GetPreferredSize().IsEmpty()) has_controls = true;
    if (!has_controls) return gfx::Size();
  }
  gfx::Size size;
  switch (display_mode_) {
    case DisplayMode::kCustomTab:
      size = custom_tab_bar_->GetPreferredSize();
      break;
    case DisplayMode::kLocation:
      size = location_bar_->PreferredSize();
      break;
    case DisplayMode::kNormal:
      size = AccessiblePaneView::CalculatePreferredSize(available_size);
      // Because there are odd cases where something causes one of the views in
      // the toolbar to report an unreasonable height (see crbug.com/41471763),
      // we cap the height at the size of known child views (location bar and
      // back button) plus margins.
      // TODO(crbug.com/40663413): Figure out why the height reports incorrectly
      // on some installations.
      if (layout_manager_ && location_bar_->IsVisible()) {
        const int max_height = std::max(location_bar_->PreferredSize().height(),
                                        GetBackForwardButtonSize().height()) +
                               layout_manager_->interior_margin().height();
        size.SetToMin({size.width(), max_height});
      }
  }
  if (display_mode_ == DisplayMode::kNormal && IsCompactLayout() &&
      browser_->GetProfile()->GetPrefs()->GetBoolean(
          "noventra.browser.address_on_demand")) {
    size.set_height(std::max(size.height(), 52));
  }
  if (UsesNoventraSidebar()) {
    size.set_height(UsesNoventraSidebarControls() ? 84 : 44);
  }
  size.set_height(size.height() * size_animation_.GetCurrentValue());
  return size;
}

gfx::Size ToolbarView::GetMinimumSize() const {
  gfx::Size size;
  switch (display_mode_) {
    case DisplayMode::kCustomTab:
      size = custom_tab_bar_->GetMinimumSize();
      break;
    case DisplayMode::kLocation:
      size = location_bar_->MinimumSize();
      break;
    case DisplayMode::kNormal:
      size = AccessiblePaneView::GetMinimumSize();
      // Because there are odd cases where something causes one of the views in
      // the toolbar to report an unreasonable height (see crbug.com/41471763),
      // we cap the height at the size of known child views (location bar and
      // back button) plus margins.
      // TODO(crbug.com/40663413): Figure out why the height reports incorrectly
      // on some installations.
      if (layout_manager_ && location_bar_->IsVisible()) {
        const int max_height =
            std::max(location_bar_->MinimumSize().height(),
                     GetBackForwardButtonSize(/*minimum_size=*/true).height()) +
            layout_manager_->interior_margin().height();
        size.SetToMin({size.width(), max_height});
      }
      // Overflow button must be part of minimum size calculation.
      if (overflow_button_ &&
          browser_->GetType() == BrowserWindowInterface::Type::TYPE_NORMAL &&
          !overflow_button_->GetVisible()) {
        const int default_margin =
            GetLayoutConstant(LayoutConstant::kToolbarIconDefaultMargin);
        size.Enlarge(
            default_margin + overflow_button_->GetMinimumSize().width(), 0);
      }
  }
  if (display_mode_ == DisplayMode::kNormal && IsCompactLayout() &&
      browser_->GetProfile()->GetPrefs()->GetBoolean(
          "noventra.browser.address_on_demand")) {
    size.set_height(std::max(size.height(), 52));
  }
  if (UsesNoventraSidebar()) {
    size.set_height(UsesNoventraSidebarControls() ? 84 : 44);
  }
  size.set_height(size.height() * size_animation_.GetCurrentValue());
  return size;
}

void ToolbarView::Layout(PassKey) {
  // If we have not been initialized yet just do nothing.
  if (!initialized_) {
    return;
  }

  if (noventra_docking_) {
    LayoutSuperclass<AccessiblePaneView>(this);
    LayoutNoventraInlineAddress();
    return;
  }
  if (display_mode_ == DisplayMode::kCustomTab) {
    custom_tab_bar_->SetBounds(0, 0, width(),
                               custom_tab_bar_->GetPreferredSize().height());
    CHECK(location_bar_view_)
        << "Alternate location bar impls need to handle this.";
    location_bar_view_->SetVisible(false);
    return;
  }

  if (display_mode_ == DisplayMode::kNormal) {
    LayoutCommon();
  }

  if (toolbar_controller_) {
    // Need to determine whether the overflow button should be visible, and only
    // update it if the visibility changes.
    const bool was_overflow_button_visible =
        toolbar_controller_->overflow_button()->GetVisible();
    const bool show_overflow_button =
        toolbar_controller_->ShouldShowOverflowButton(size());
    if (was_overflow_button_visible != show_overflow_button) {
      views::ManualLayoutUtil(layout_manager_)
          .SetViewHidden(toolbar_controller_->overflow_button(),
                         !show_overflow_button);
      base::RecordAction(base::UserMetricsAction(
          show_overflow_button ? "ResponsiveToolbar.OverflowButtonShown"
                               : "ResponsiveToolbar.OverflowButtonHidden"));
    }
  }

  // Call super implementation to ensure layout manager and child layouts
  // happen.
  LayoutSuperclass<AccessiblePaneView>(this);
  LayoutNoventraInlineAddress();
}

void ToolbarView::OnThemeChanged() {
  views::AccessiblePaneView::OnThemeChanged();
  if (!initialized_) {
    return;
  }

  if (display_mode_ == DisplayMode::kNormal) {
    LoadImages();
  }

  SchedulePaint();
}

void ToolbarView::NewTabButtonPressed(const ui::Event& event) {
  chrome::NewTab(browser_, NewTabTypes::kNewTabButton);
}

void ToolbarView::VerticalTabsCollapseButtonPressed(const ui::Event& event) {
  auto* controller =
      tabs::VerticalTabStripStateController::From(browser_view_->browser());
  if (!controller) {
    return;
  }

  controller->RequestCollapse(!controller->IsCollapsed());
}

bool ToolbarView::AcceleratorPressed(const ui::Accelerator& accelerator) {
  const views::View* focused_view = focus_manager()->GetFocusedView();
  if (focused_view && (focused_view->GetID() == VIEW_ID_OMNIBOX)) {
    return false;  // Let the omnibox handle all accelerator events.
  }
  return AccessiblePaneView::AcceleratorPressed(accelerator);
}

void ToolbarView::ChildPreferredSizeChanged(views::View* child) {
  InvalidateLayout();
  if (size() != GetPreferredSize()) {
    PreferredSizeChanged();
  }
}

void ToolbarView::ChildVisibilityChanged(views::View* child) {
  if (child == home_) {
    if (!home_->GetVisible() && show_home_button_.GetValue()) {
      base::UmaHistogramBoolean("Toolbar.Overflow.HomeButton", true);
    }
  }
  if (child == glic_button_view_ || child == glic_actor_button_container_ ||
      child == avatar_) {
    PositionContextualTasksButton();
  }
}

////////////////////////////////////////////////////////////////////////////////
// ToolbarView, private:

// Override this so that when the user presses F6 to rotate toolbar panes,
// the location bar gets focus, not the first control in the toolbar - and
// also so that it selects all content in the location bar.
views::View* ToolbarView::GetDefaultFocusableChild() {
  if (noventra_docking_ && location_bar_view_->parent() != this)
    return AccessiblePaneView::GetDefaultFocusableChild();
  return location_bar_view_;
}

void ToolbarView::InitLayout() {
  const int default_margin =
      GetLayoutConstant(LayoutConstant::kToolbarIconDefaultMargin);

  layout_manager_ =
      SetLayoutManager(std::make_unique<CenteredLocationBarLayout>(
          location_bar_view_,
          base::BindRepeating(&ToolbarView::GetCenteredLocationBarBounds,
                              base::Unretained(this)),
          base::BindRepeating(&ToolbarView::IsCompactLayout,
                              base::Unretained(this))));

  layout_manager_->SetOrientation(views::LayoutOrientation::kHorizontal)
      .SetCrossAxisAlignment(views::LayoutAlignment::kCenter)
      .SetCollapseMargins(true)
      .SetDefault(views::kMarginsKey, gfx::Insets::VH(0, default_margin));

  UpdateToolbarLayout();

  if (glic_button_view_ &&
      base::FeatureList::IsEnabled(features::kToolbarGlicButtonResizing)) {
    glic_button_view_->SetProperty(
        views::kFlexBehaviorKey,
        views::FlexSpecification(
            views::LayoutOrientation::kHorizontal,
            views::MinimumFlexSizeRule::kPreferredSnapToMinimum,
            views::MaximumFlexSizeRule::kPreferred));
  }

  if (app_menu_button_ &&
      base::FeatureList::IsEnabled(features::kToolbarAppMenuLabelResizing)) {
    app_menu_button_->SetProperty(
        views::kFlexBehaviorKey,
        views::FlexSpecification(views::LayoutOrientation::kHorizontal,
                                 views::MinimumFlexSizeRule::kScaleToMinimum,
                                 views::MaximumFlexSizeRule::kPreferred));
  }

  if (avatar_ &&
      base::FeatureList::IsEnabled(features::kToolbarProfileChipResizing)) {
    // Flex order for the profile avatar button is determined by the
    // `toolbar_controller`.
    avatar_->SetProperty(
        views::kFlexBehaviorKey,
        views::FlexSpecification(
            views::LayoutOrientation::kHorizontal,
            views::MinimumFlexSizeRule::kScaleToMinimumSnapToZero,
            views::MaximumFlexSizeRule::kPreferred));
  }

  if (features::IsWebUIToolbarEnabled() && toolbar_webview_) {
    // Strip default C++ outer margins from `toolbar_webview_` since WebUI CSS
    // handles internal toolbar spacing.
    toolbar_webview_->SetProperty(views::kMarginsKey, gfx::Insets());
  }

  // Preserve Chromium's responsive rules, but protect compact controls until
  // the controls themselves no longer fit, independently of the tab strip.
  auto* responsive_layout =
      static_cast<ResponsiveToolbarLayout*>(layout_manager_.get());
  if (pinned_toolbar_actions_container_) {
    pinned_toolbar_actions_container_->SetProperty(
        views::kFlexBehaviorKey,
        responsive_layout->ProtectedCompactFlex(
            views::FlexSpecification(
                pinned_toolbar_actions_container_->GetAnimatingLayoutManager()
                    ->GetDefaultFlexRule())
                .WithOrder(kToolbarActionsFlexOrder)));
  }

  if (extensions_container_) {
    extensions_container_->SetProperty(
        views::kFlexBehaviorKey,
        responsive_layout->ProtectedCompactFlex(
            views::FlexSpecification(
                views::MinimumFlexSizeRule::kScaleToMinimumSnapToZero,
                views::MaximumFlexSizeRule::kPreferred)
                .WithOrder(kExtensionsFlexOrder)));
  }

  const auto overflow_order = ToolbarController::GetDefaultOverflowOrder();
  for (views::View* child : children()) {
    if (child == toolbar_webview_ ||
        !std::ranges::contains(
            overflow_order, child->GetProperty(views::kElementIdentifierKey))) {
      continue;
    }
    auto responsive = views::FlexSpecification(
        views::MinimumFlexSizeRule::kPreferredSnapToZero,
        views::MaximumFlexSizeRule::kPreferred);
    if (const auto* flex = child->GetProperty(views::kFlexBehaviorKey)) {
      responsive = *flex;
    }
    child->SetProperty(views::kFlexBehaviorKey,
                       responsive_layout->ProtectedCompactFlex(responsive));
  }
  if (overflow_button_) {
    overflow_button_->SetProperty(
        views::kFlexBehaviorKey,
        responsive_layout->ProtectedCompactFlex(views::FlexSpecification()));
  }

  // If there are any non-WebUI controls, needs to set up the ToolbarController
  // to handle overflow. Otherwise, the toolbar controller is not needed.
  if (!features::IsWebUIToolbarFullyEnabled()) {
    // Order 1 is reserved for the location bar if
    // kOmniboxResizingPrioritization is enabled.
    constexpr int kToolbarFlexOrderStart = 2;

    // TODO(crbug.com/40929989): Ignore containers till issue addressed.
    toolbar_controller_ = std::make_unique<ToolbarController>(
        OverflowMenu::GetDefaultResponsiveElements(browser_), overflow_order,
        kToolbarFlexOrderStart, this, toolbar_webview_.get(), overflow_button_,
        pinned_toolbar_actions_,
        PinnedToolbarActionsModel::Get(browser_view_->GetProfile()));
    overflow_button_->set_toolbar_controller(toolbar_controller_.get());
  }

  if (toolbar_webview_) {
    if (toolbar_controller_) {
      toolbar_webview_->SetProperty(
          views::kFlexBehaviorKey,
          toolbar_webview_->GetFlexSpecification(
              toolbar_controller_->webui_toolbar_button_flex_order(),
              kLocationBarFlexOrder));
    } else {
      // If `toolbar_controller_` is nullptr, then the WebUI toolbar is managing
      // all controls, so don't need to worry about integrating with
      // ToolbarController's overflow logic, so instead use a more basic
      // resizing rule. Eventually we'll be the only View in this case, anyways,
      // and may want to get rid of the FlexLayout entirely.
      toolbar_webview_->SetProperty(
          views::kFlexBehaviorKey,
          views::FlexSpecification(views::LayoutOrientation::kHorizontal,
                                   views::MinimumFlexSizeRule::kScaleToMinimum,
                                   views::MaximumFlexSizeRule::kUnbounded));
    }
  }

  LayoutCommon();
}

void ToolbarView::LayoutCommon() {
  DCHECK(display_mode_ == DisplayMode::kNormal);

  gfx::Insets interior_margin =
      GetLayoutInsets(LayoutInset::TOOLBAR_INTERIOR_MARGIN);

  const bool is_trailing_contextual_tasks_visible =
      IsTrailingContextualTasksButtonVisible();
  const bool is_leading_contextual_tasks_visible =
      IsLeadingContextualTasksButtonVisible();

  // Only zero out the interior margin if the contextual tasks button
  // is actually visible and not in vertical tabs mode (where the button does
  // not sit flush at the window edge). When the button is hidden, we must
  // retain the default interior margin so that the Back button (or App Menu
  // button) is not incorrectly shifted to the toolbar's edge. Layout is in
  // logical / RTL-relative DIPs where `left()` is the leading edge.
  if (!should_display_vertical_tabs_) {
    if (is_leading_contextual_tasks_visible) {
      interior_margin.set_left(0);
    }
    if (is_trailing_contextual_tasks_visible) {
      interior_margin.set_right(0);
    }
  }

  if (app_menu_button_) {
    const bool expanded = app_menu_button_->IsLabelPresentAndVisible();
    if (expanded && !is_trailing_contextual_tasks_visible) {
      // The interior margin in an expanded state should be more than in a
      // collapsed state.
      interior_margin.set_right(interior_margin.right() + 1);
    }
    SetRefreshMargins(app_menu_button_, expanded);
  }

  // The margins of the `avatar_` uses the same constants as the
  // `app_menu_button_`.
  if (avatar_) {
    SetRefreshMargins(avatar_, avatar_->IsLabelPresentAndVisible());
  }

  // Record whether there is leading interior margin available before zeroing it
  // out for WebUI toolbar below. If a leading button (such as the ephemeral
  // contextual tasks button) sits flush at the container edge, the leading
  // interior margin is zero and subsequent buttons (like the Back button) must
  // not extend their hit target for Fitts' law.
  const int leading_interior_margin = interior_margin.left();

  const bool is_rtl = base::i18n::IsRTL();

  // Zero out leading or trailing interior margins for WebUI toolbar so that
  // `toolbar_webview_` spans the full width of the container edge-to-edge.
  // WebUI handles standard toolbar indentation internally via CSS.
  // Note: Because `gfx::Insets` operate in physical screen coordinates, we
  // check `base::i18n::IsRTL()` to invert which physical edge corresponds to
  // the leading Back/Forward button versus the trailing App Menu button.
  // TODO(crbug.com/538651978): Once the entire toolbar migrates to 100%
  // WebUI, simplify these individual feature checks to a single global check.
  if (features::IsWebUIBackForwardButtonEnabled()) {
    if (is_rtl) {
      interior_margin.set_right(0);
    } else {
      interior_margin.set_left(0);
    }
  }
  if (features::IsWebUIAppMenuButtonEnabled()) {
    if (is_rtl) {
      interior_margin.set_left(0);
    } else {
      interior_margin.set_right(0);
    }
  }
  layout_manager_->SetInteriorMargin(interior_margin);

  // Extend buttons to the window edge if we're either in a maximized or
  // fullscreen window. This makes the buttons easier to hit, see Fitts' law.
  // Note on layout synchronization: In Chromium UI Views, window maximize,
  // restore, or fullscreen transitions automatically invalidate container
  // geometry and invoke `ToolbarView::Layout()`. This layout pass guarantees
  // `SetIsMaximizedOrFullscreen` remains reliably synchronized with window
  // bounds without requiring supplemental widget observation hooks.
  // Reparented dock cells own their padding. Applying the old toolbar's
  // Fitts-law margin would move Back's icon away from its hover centre.
  const bool is_maximized_or_fullscreen =
      !noventra_docking_ && browser_->GetWindow() &&
      (browser_->GetWindow()->IsMaximized() ||
       browser_->GetWindow()->IsFullscreen());

  const bool collapse_button_on_trailing_edge =
      vertical_tabs_collapse_button_ &&
      vertical_tabs_collapse_button_->GetVisible() && IsVerticalLayout() &&
      IsVerticalTabsRightAligned();
  const bool collapse_button_on_leading_edge =
      vertical_tabs_collapse_button_ &&
      vertical_tabs_collapse_button_->GetVisible() &&
      !collapse_button_on_trailing_edge;

  if (vertical_tabs_collapse_button_) {
    const gfx::Insets button_margin =
        GetLayoutInsets(LayoutInset::TOOLBAR_INTERIOR_MARGIN);
    vertical_tabs_collapse_button_->SetLeadingMargin(
        is_maximized_or_fullscreen && collapse_button_on_leading_edge
            ? button_margin.left()
            : 0);
    vertical_tabs_collapse_button_->SetTrailingMargin(
        is_maximized_or_fullscreen && collapse_button_on_trailing_edge
            ? button_margin.right()
            : 0);
  }

  const int margin =
      is_maximized_or_fullscreen && !collapse_button_on_leading_edge
          ? leading_interior_margin
          : 0;

  if (features::IsWebUIBackForwardButtonEnabled()) {
    if (toolbar_webview_) {
      // `SetIsMaximizedOrFullscreen` signals the WebUI back/forward buttons
      // whether to apply Fitts' law edge padding. When a leading button
      // (e.g. contextual tasks) sits flush against the window edge, the
      // available margin is 0, so Fitts' law padding must be suppressed even
      // if the window itself is maximized or fullscreen.
      const bool should_apply_webui_fitts_law = margin > 0;
      toolbar_webview_->SetIsMaximizedOrFullscreen(
          should_apply_webui_fitts_law);
    }
  } else {
    if (back_) {
      back_->SetLeadingMargin(margin);
    }
  }

  GetAppMenuControl()->SetIsMaximizedOrFullscreen(
      ShouldAppMenuApplyFittsLaw(is_maximized_or_fullscreen) &&
      !collapse_button_on_trailing_edge);

  // Cast button visibility is controlled externally.
}

void ToolbarView::PositionContextualTasksButton() {
  if (!contextual_tasks_button_) {
    return;
  }
  auto* button =
      static_cast<ContextualTasksButton*>(contextual_tasks_button_.get());
  if (contextual_tasks::kEnableCircularEphemeralButtonNextToBatterySaver
          .Get() &&
      button->IsSidePanelRightAligned()) {
    const bool is_glic_left_of_profile =
        features::kGlicToolbarButtonLocationParam.Get() ==
            features::GlicToolbarButtonLocation::kLeftOfProfileChip ||
        features::kGlicToolbarButtonLocationParam.Get() ==
            features::GlicToolbarButtonLocation::
                kLeftOfProfileChipWithBackground;
    views::View* anchor = nullptr;
    if (glic_button_view_ && glic_button_view_->GetVisible() &&
        is_glic_left_of_profile) {
      anchor = (glic_button_view_->parent() == this)
                   ? static_cast<views::View*>(glic_button_view_)
                   : static_cast<views::View*>(glic_button_view_->parent());
    } else if (avatar_) {
      anchor = avatar_;
    } else {
      anchor = app_menu_button_;
    }
    if (anchor) {
      std::optional<size_t> anchor_index = GetIndexOf(anchor);
      if (anchor_index.has_value()) {
        const size_t current_index =
            GetIndexOf(contextual_tasks_button_).value();
        const size_t target_index =
            current_index < *anchor_index ? *anchor_index - 1 : *anchor_index;
        ReorderChildView(contextual_tasks_button_, target_index);
        return;
      }
    }
  }
  const size_t target_index = button->IsTrailing() ? children().size() : 0;
  ReorderChildView(contextual_tasks_button_, target_index);
}

bool ToolbarView::IsLeadingContextualTasksButtonVisible() const {
  return contextual_tasks_button_ && contextual_tasks_button_->GetVisible() &&
         !children().empty() && children().front() == contextual_tasks_button_;
}

bool ToolbarView::IsTrailingContextualTasksButtonVisible() const {
  return contextual_tasks_button_ && contextual_tasks_button_->GetVisible() &&
         !children().empty() && children().back() == contextual_tasks_button_;
}

bool ToolbarView::ShouldAppMenuApplyFittsLaw(
    bool is_maximized_or_fullscreen) const {
  if (!is_maximized_or_fullscreen) {
    return false;
  }

  // `SetIsMaximizedOrFullscreen()` informs the app menu control to extend its
  // hit target to the window edge per Fitts' law when maximized or fullscreen.
  // When the contextual tasks button is placed at the trailing edge, the app
  // menu button is no longer flush with the window border, so Fitts' law edge
  // padding must be suppressed to avoid inserting an unwanted gap between the
  // two buttons.
  //
  // Note: The leading edge (Back button) handles this symmetrically: when the
  // contextual tasks button is visible on the leading edge, the leading
  // interior margin is zeroed out, which naturally zeroes out the Back
  // button's leading margin (see `leading_interior_margin`).
  return !IsTrailingContextualTasksButtonVisible();
}

void ToolbarView::AttachTabStripRegionView(TabStripRegionView* view) {
  if (!view) {
    return;
  }

  tab_strip_region_view_ = view;

  if (views::View* parent = tab_strip_region_view_->parent()) {
    std::unique_ptr<views::View> owned_view =
        parent->RemoveChildViewT(tab_strip_region_view_);
    auto location_bar_index =
        location_bar_view_ ? GetIndexOf(location_bar_view_) : std::nullopt;
    const size_t insertion_index =
        location_bar_index ? *location_bar_index + 1 : children().size();
    AddChildViewAt(std::move(owned_view), insertion_index);
  }

  // The view may not be visible when attached (vertical -> compact),
  // so ensure it is.
  tab_strip_region_view_->SetVisible(true);
  UpdateNoventraTabOrder();
  tab_strip_region_view_->SetPaintToLayer();
  tab_strip_region_view_->layer()->SetFillsBoundsOpaquely(false);
  tab_strip_region_view_->layer()->SetMasksToBounds(true);
  tab_strip_region_view_->SetProperty(
      views::kFlexBehaviorKey,
      views::FlexSpecification(views::LayoutOrientation::kHorizontal,
                               views::MinimumFlexSizeRule::kScaleToZero,
                               views::MaximumFlexSizeRule::kUnbounded)
          .WithOrder(kTabStripFlexOrder));

  InvalidateLayout();
}

// Reordering views is only safe outside view-tree focus/removal notifications.
// Run this on attachment and preference changes, never on omnibox focus changes.
void ToolbarView::SetNoventraDockingActive() {
  noventra_docking_ = true;
  // These are legacy layout affordances, not dock controls. A stale overflow
  // button otherwise leaves an empty top toolbar after activating a layout.
  for (views::View* view : std::array<views::View*, 4>{overflow_button_,
           vertical_tabs_collapse_button_, toolbar_divider_, noventra_search_button_})
    if (view) {
      if (layout_manager_) views::ManualLayoutUtil(layout_manager_).SetViewHidden(view, true);
      view->SetVisible(false);
    }
  InvalidateLayout();
}

void ToolbarView::SetNoventraMediaVisibility(bool visible) {
  if (media_button_) media_button_->GetController()->SetCanShowToolbarButton(visible);
}

std::unique_ptr<views::View> ToolbarView::ReleaseNoventraControl(std::string_view id, views::View** original_parent) {
  if (auto action = noventra::NativeDockActionId(id)) {
    if (original_parent) *original_parent = pinned_toolbar_actions_container_;
    return pinned_toolbar_actions_container_->ReleaseNoventraAction(*action);
  }
  views::View* view = nullptr;
  if (id == "back") view = back_;
  else if (id == "forward") view = forward_;
  else if (id == "reload") view = reload_;
  else if (id == "home") view = home_;
  else if (id == "address") view = location_bar_view_;
  else if (id == "menu") view = app_menu_button_;
  else if (id == "new-tab") view = dynamic_new_tab_button_;
  else if (id == "saved-sites") view = noventra_bookmarks_button_;
  else if (id == "customize") view = noventra_tools_button_;
  else if (id == "extensions") view = extensions_container_;
  else if (id == "additional-actions") view = pinned_toolbar_actions_container_;
  else if (id == "profile") view = avatar_;
  else if (id == "media" && media_button_) view = media_button_->GetBubbleAnchor().GetIfView();
  if (!view || view->parent() != this) return nullptr;
  if (original_parent) *original_parent = this;
  return RemoveChildViewT(view);
}

void ToolbarView::UpdateNoventraTabOrder() {
  if (noventra_docking_) return;
  if (const auto* original_order = GetProperty(kNoventraToolbarOrderKey)) {
    size_t index = 0;
    for (const auto& tracker : *original_order) {
      if (auto* child = tracker->view(); child && child->parent() == this) {
        ReorderChildView(child, index++);
      }
    }
  }
  if (UsesNoventraSidebar() && location_bar_view_) {
    ReorderChildView(location_bar_view_, children().size() - 1);
  }
  if (tab_strip_region_view_ && tab_strip_region_view_->parent() == this &&
      IsCompactLayout()) {
    if (browser_->GetProfile()->GetPrefs()->GetBoolean(
          "noventra.browser.address_on_demand")) {
      const bool has_collapse = vertical_tabs_collapse_button_ &&
          vertical_tabs_collapse_button_->parent() == this;
      // Right-aligned vertical tabs leave this button at the trailing edge.
      // Normalize both controls here before generic layout observes the order.
      if (has_collapse) {
        ReorderChildView(vertical_tabs_collapse_button_, 0);
      }
      ReorderChildView(tab_strip_region_view_, has_collapse ? 1 : 0);
      // Search puts extensions, navigation and bookmarks after the tab run.
      // Reorder only at safe layout-mode/initialization boundaries.
      for (views::View* view : std::array<views::View*, 7>{
               extensions_container_, back_, forward_, reload_,
               noventra_bookmarks_button_, noventra_tools_button_,
               app_menu_button_}) {
        if (view && view->parent() == this) {
          ReorderChildView(view, children().size() - 1);
        }
      }
      // Paint the inline editor above the active tab; it occupies no flex slot.
      if (location_bar_view_) {
        ReorderChildView(location_bar_view_, children().size() - 1);
      }
      tab_strip_region_view_->SetProperty(views::kCrossAxisAlignmentKey,
                                          views::LayoutAlignment::kCenter);
    } else if (location_bar_view_) {
      // Restore the ordinary compact order when a persistent field is chosen.
      ReorderChildView(tab_strip_region_view_, children().size() - 1);
      ReorderChildView(tab_strip_region_view_,
                       GetIndexOf(location_bar_view_).value() + 1);
    }
  }
}

void ToolbarView::OnLocationBarFocusChanged() {
  if (initialized_) {
    UpdateToolbarLayout();
    if (UsesNoventraSidebar()) {
      browser_view_->InvalidateLayout();
    }
    if (UsesNoventraInlineAddress()) {
      if (auto* strip = tab_strip_region_view_->GetTabStripView()) {
        strip->InvalidateLayout();
      }
      tab_strip_region_view_->InvalidateLayout();
    }
  }
}

bool ToolbarView::UsesNoventraSidebarControls() const {
  if (noventra_docking_) return false;
  return initialized_ && display_mode_ == DisplayMode::kNormal &&
      IsVerticalLayout() && location_bar_view_ &&
      !tabs::VerticalTabStripStateController::From(browser_)->IsCollapsed() &&
      noventra_sidebar_controls_.GetValue();
}

bool ToolbarView::UsesNoventraSidebar() const {
  if (noventra_docking_) return false;
  if (UsesNoventraSidebarControls()) {
    return true;
  }
  return initialized_ && display_mode_ == DisplayMode::kNormal &&
      IsVerticalLayout() && location_bar_view_ &&
      !tabs::VerticalTabStripStateController::From(browser_)->IsCollapsed() &&
      !browser_view_->IsZenModeEnabledForLayout() &&
      browser_->GetProfile()->GetPrefs()->GetBoolean(
          "noventra.browser.address_on_demand");
}

bool ToolbarView::IsNoventraSidebarAddressVisible() const {
  return UsesNoventraSidebar() && !UsesNoventraSidebarControls() &&
      !location_bar_view_->IsNoventraAddressCollapsed();
}

bool ToolbarView::UsesNoventraInlineAddress() const {
  return initialized_ && display_mode_ == DisplayMode::kNormal &&
      IsCompactLayout() && location_bar_view_ && tab_strip_region_view_ &&
      tab_strip_region_view_->parent() == this &&
      browser_->GetProfile()->GetPrefs()->GetBoolean(
          "noventra.browser.address_on_demand");
}

void ToolbarView::LayoutNoventraInlineAddress() {
  if (noventra_docking_) return;
  if (UsesNoventraSidebarControls()) {
    // Use the existing native controls and omnibox, with its security chips,
    // permission anchors and completion popup, in two compact sidebar rows.
    for (views::View* child : children()) {
      if (child != location_bar_view_ && child->GetVisible()) {
        auto bounds = child->bounds();
        bounds.set_y(4);
        bounds.set_height(std::min(34, bounds.height()));
        child->SetBoundsRect(bounds);
      }
    }
    location_bar_view_->SetBounds(8, 42, std::max(0, width() - 16),
        GetLayoutConstant(LayoutConstant::kLocationBarHeight));
    return;
  }
  if (UsesNoventraSidebar()) {
    if (location_bar_view_->IsNoventraAddressCollapsed()) {
      location_bar_view_->SetBounds(0, 0, 0, 0);
    } else {
      // The native editor overlays the header only while editing or showing
      // a contextual security/permission indicator. Never replace it with a
      // separate text field: security state and autocomplete stay native.
      const int field_height = GetLayoutConstant(LayoutConstant::kLocationBarHeight);
      location_bar_view_->SetBounds(8, (height() - field_height) / 2,
                                   std::max(0, width() - 16), field_height);
    }
    return;
  }
  if (!UsesNoventraInlineAddress()) {
    return;
  }
  // The tab renderer uses a fixed strip height. Center that strip in Search's
  // taller toolbar so tab titles and the navigation icons share a baseline.
  gfx::Rect strip_bounds = tab_strip_region_view_->bounds();
  const int strip_height = GetLayoutConstant(LayoutConstant::kTabStripHeight);
  strip_bounds.set_height(strip_height);
  strip_bounds.set_y((height() - strip_height) / 2);
  tab_strip_region_view_->SetBoundsRect(strip_bounds);

  // Keep the actual omnibox focusable for Ctrl+L. Resting tabs need no separate
  // address capsule. Contextual security/permission indicators expand it too.
  if (location_bar_view_->IsNoventraAddressCollapsed()) {
    location_bar_view_->SetBounds(0, 0, 0, 0);
    return;
  }
  const auto* tab = browser_->GetActiveTabInterface();
  auto* anchor = tab ? tab_strip_region_view_->GetTabAnchorView(tab->GetHandle())
                     : nullptr;
  gfx::Rect bounds = anchor
      ? views::View::ConvertRectToTarget(anchor, this, anchor->GetLocalBounds())
      : gfx::Rect(8, 0, 340, height());
  const int right = tab_strip_region_view_->bounds().right() - 4;
  const int field_width = std::min(340, std::max(0, right - 8));
  bounds.set_x(std::clamp(bounds.x() + 4, 8,
                          std::max(8, right - field_width)));
  bounds.set_width(field_width);
  const int field_height = GetLayoutConstant(LayoutConstant::kLocationBarHeight);
  bounds.set_y((height() - field_height) / 2);
  bounds.set_height(field_height);
  location_bar_view_->SetBoundsRect(bounds);
}

void ToolbarView::UpdateToolbarLayout() {
  if (noventra_docking_) return;
  if (!location_bar_) {
    return;
  }

  const bool is_compact_layout = IsCompactLayout();
  const bool inline_address = UsesNoventraInlineAddress();
  if (location_bar_view_) {
    location_bar_view_->SetProperty(views::kViewIgnoredByLayoutKey, inline_address || UsesNoventraSidebar());
  }
  if (noventra_bookmarks_button_) {
    noventra_bookmarks_button_->SetVisible(inline_address);
  }
  if (initialized_ && display_mode_ == DisplayMode::kNormal) {
    const bool show_navigation = (!is_compact_layout && !IsVerticalLayout()) ||
        noventra_show_navigation_buttons_.GetValue();
    if (back_) {
      back_->SetVisible(show_navigation && show_back_button_.GetValue());
    }
    if (reload_) {
      reload_->SetVisible(show_navigation && show_reload_button_.GetValue());
    }
    SetForwardButtonVisibility(show_navigation && show_forward_button_.GetValue());
    if (noventra_search_button_) {
      noventra_search_button_->SetVisible(UsesNoventraSidebar() &&
                                          !UsesNoventraSidebarControls());
    }
  }

  UpdateVerticalTabsCollapseButton();
  UpdateDynamicNewTabButton();

  if (vertical_tabs_collapse_button_) {
    const size_t target_index =
        IsVerticalLayout() && IsVerticalTabsRightAligned()
            ? GetIndexOf(app_menu_button_).value() + 1
            : 0;
    if (GetIndexOf(vertical_tabs_collapse_button_).value() != target_index) {
      ReorderChildView(vertical_tabs_collapse_button_, target_index);
    }
  }

  const int location_bar_margin =
      GetLayoutConstant(LayoutConstant::kLocationBarMargin);
  if (location_bar_view_) {
    if (is_compact_layout) {
      // Let the location bar recompute width when permission/security
      // decorations change, not just when toolbar focus or prefs change.
      location_bar_view_->SetPreferredSize(std::nullopt);
      location_bar_view_->SetProperty(
          views::kMarginsKey, gfx::Insets::TLBR(0, location_bar_margin, 0, 0));
    } else {
      location_bar_view_->SetPreferredSize(std::nullopt);
      location_bar_view_->SetProperty(views::kMarginsKey,
                                      gfx::Insets::VH(0, location_bar_margin));
    }
    if (display_mode_ == DisplayMode::kNormal) {
      const auto maximum = is_compact_layout
                               ? views::MaximumFlexSizeRule::kPreferred
                               : views::MaximumFlexSizeRule::kUnbounded;
      location_bar_view_->SetProperty(
          views::kFlexBehaviorKey,
          static_cast<ResponsiveToolbarLayout*>(layout_manager_.get())
              ->ProtectedCompactFlex(
                  views::FlexSpecification(
                      views::MinimumFlexSizeRule::kScaleToMinimum, maximum)
                      .WithOrder(kLocationBarFlexOrder),
                  maximum));
    }
  }

  InvalidateLayout();
}

void ToolbarView::AbortToolbarContainerAnimationsForZenReveal() {
  if (extensions_container_) {
    extensions_container_->GetAnimatingLayoutManager()->ResetLayout();
  }
  if (pinned_toolbar_actions_container_) {
    pinned_toolbar_actions_container_->GetAnimatingLayoutManager()
        ->ResetLayout();
  }
}

bool ToolbarView::IsCompactLayout() const {
  auto* controller =
      HeliumLayoutStateController::From(browser_view_->browser());
  return controller && controller->IsCompactLayout();
}

bool ToolbarView::IsDynamicLayout() const {
  auto* controller =
      HeliumLayoutStateController::From(browser_view_->browser());
  return controller && controller->IsDynamicLayout();
}

bool ToolbarView::IsVerticalLayout() const {
  auto* controller =
      HeliumLayoutStateController::From(browser_view_->browser());
  return controller && controller->IsVerticalLayout();
}

bool ToolbarView::IsVerticalTabsRightAligned() const {
  auto* controller =
      tabs::VerticalTabStripStateController::From(browser_view_->browser());
  return controller && controller->IsTabStripRightAligned();
}

bool ToolbarView::ShouldShowToolbarNewTabButton() const {
  // TODO: Show the toolbar new tab button in Compact layout
  // once we improve it.
  return IsDynamicLayout();
}

void ToolbarView::UpdateVerticalTabsCollapseButton() {
  if (!vertical_tabs_collapse_button_) {
    return;
  }

  auto* controller =
      tabs::VerticalTabStripStateController::From(browser_view_->browser());
  const bool zen_hide = browser_view_->IsZenModeEnabledForLayout() &&
                        !browser_view_->IsZenModeSideChromePinned();
  const bool should_show = !noventra_docking_ && controller && IsVerticalLayout() &&
                           show_vertical_tabs_collapse_button_.GetValue() &&
                           !zen_hide && !UsesNoventraSidebar();
  // Let overflow layout decide whether the eligible button fits.
  if (should_show != layout_manager_->CanBeVisible(
          vertical_tabs_collapse_button_)) {
    vertical_tabs_collapse_button_->SetVisible(should_show);
  }

  if (!should_show) {
    return;
  }

  // ToolbarButton mirrors the icon in RTL.
  const gfx::VectorIcon& icon = controller->IsTabStripRightAligned()
                                    ? views::kSidebarRightIcon
                                    : views::kSidebarLeftIcon;
  const int text_id = controller->IsCollapsed() ? IDS_EXPAND_VERTICAL_TABS
                                                : IDS_COLLAPSE_VERTICAL_TABS;
  const std::u16string text = l10n_util::GetStringUTF16(text_id);

  vertical_tabs_collapse_button_->SetVectorIcon(icon);
  vertical_tabs_collapse_button_->SetTooltipText(text);
  vertical_tabs_collapse_button_->GetViewAccessibility().SetName(text);
}

void ToolbarView::UpdateDynamicNewTabButton() {
  if (noventra_docking_) return;
  if (!dynamic_new_tab_button_) {
    return;
  }

  dynamic_new_tab_button_->SetVisible(ShouldShowToolbarNewTabButton() &&
                                      show_dynamic_new_tab_button_.GetValue());
}

// AppMenuIconController::Delegate:
void ToolbarView::UpdateTypeAndSeverity(
    AppMenuIconController::TypeAndSeverity type_and_severity) {
  AppMenuControl* app_menu_control = GetAppMenuControl();
  if (app_menu_control) {
    app_menu_control->SetTypeAndSeverity(type_and_severity);
  }
  auto* action_item = actions::ActionManager::Get().FindAction(
      kActionUpgradeDialog, BrowserActions::From(browser_)->root_action_item());
  CHECK(action_item);
  action_item->SetVisible(
      type_and_severity.type ==
      AppMenuIconController::IconType::kUpgradeNotification);
  action_item->SetText(AppMenuModel::GetUpgradeDialogTitleText());

  auto* global_error_action_item = actions::ActionManager::Get().FindAction(
      kActionGlobalError, BrowserActions::From(browser_)->root_action_item());
  CHECK(global_error_action_item);
  if (GlobalError* error =
          GlobalErrorServiceFactory::GetForProfile(browser_->GetProfile())
              ->GetHighestSeverityGlobalErrorWithAppMenuItem()) {
    global_error_action_item->SetVisible(true);
    global_error_action_item->SetText(error->MenuItemLabel());
    global_error_action_item->SetImage(error->MenuItemIcon());
  } else {
    global_error_action_item->SetVisible(false);
  }
}

ExtensionsContainerViews* ToolbarView::GetExtensionsContainerViews() {
  if (features::IsWebUIExtensionsContainerEnabled()) {
    return toolbar_webview_->extensions_container_views();
  }
  return extensions_container_;
}

PinnedToolbarActions* ToolbarView::GetPinnedToolbarActions() {
  return pinned_toolbar_actions_;
}

gfx::Size ToolbarView::GetToolbarButtonSize() const {
  // Since DisplayMode::kLocation is for a slimline toolbar showing only compact
  // location bar used for popups, toolbar buttons (ie downloads) must be
  // smaller to accommodate the smaller size.
  const int size =
      display_mode_ == DisplayMode::kLocation
          ? location_bar_->PreferredSize().height()
          : GetLayoutConstant(LayoutConstant::kToolbarButtonHeight);
  return gfx::Size(size, size);
}

views::BubbleAnchor ToolbarView::GetDefaultExtensionDialogAnchor() {
  ExtensionsContainerViews* extensions_container =
      GetExtensionsContainerViews();
  if (extensions_container && extensions_container->IsVisible()) {
    return extensions_container->GetExtensionsButtonAnchor();
  }
  if (overflow_button_ && overflow_button_->IsDrawn()) {
    return views::BubbleAnchor(overflow_button_);
  }
  auto* control = GetAppMenuControl();
  return control ? control->GetAnchor() : views::BubbleAnchor();
}

page_actions::PageActionViewInterface* ToolbarView::GetPageActionViewInterface(
    actions::ActionId action_id) {
  if (features::IsWebUILocationBarEnabled()) {
    if (toolbar_webview_ && !location_bar_view_) {
      return toolbar_webview_->GetLocationBar()->GetPageActionViewInterface(
          action_id);
    }
    return nullptr;
  }
  page_actions::PageActionPropertiesProvider provider;
  if (!provider.Contains(action_id)) {
    return nullptr;
  }
  return location_bar_view()->page_action_container()->GetPageActionView(
      action_id);
}

AppMenuControl* ToolbarView::GetAppMenuControl() {
  if (features::IsWebUIAppMenuButtonEnabled() && toolbar_webview_) {
    return toolbar_webview_->GetAppMenuControl();
  }
  return app_menu_button_;
}

const AppMenuControl* ToolbarView::GetAppMenuControl() const {
  if (features::IsWebUIAppMenuButtonEnabled() && toolbar_webview_) {
    return toolbar_webview_->GetAppMenuControl();
  }
  return app_menu_button_;
}

gfx::Rect ToolbarView::GetFindBarBoundingBox(int contents_bottom) {
  return gfx::Rect();
}

void ToolbarView::FocusToolbar() {
  SetPaneFocus(nullptr);
  if (toolbar_webview_) {
    toolbar_webview_->AdjustForToolbarFocus();
  }
}

views::AccessiblePaneView* ToolbarView::GetAsAccessiblePaneView() {
  return this;
}

views::BubbleAnchor ToolbarView::GetBubbleAnchor(
    std::optional<actions::ActionId> action_id) {
  // If a pinned toolbar actions button exists for the action_id, return that.
  if (pinned_toolbar_actions_ && action_id.has_value() &&
      pinned_toolbar_actions_->IsActionPinnedOrPoppedOut(action_id.value())) {
    return pinned_toolbar_actions_->GetBubbleAnchor(action_id.value());
  }

  // Otherwise attempt to use the location bar.
  auto anchor = location_bar_view_
                    ? views::BubbleAnchor(location_bar_view_)
                    : views::BubbleAnchor(location_bar_->GetAnchorOrNull());
  bool anchor_not_drawn;
  if (views::View* view = anchor.GetIfView()) {
    anchor_not_drawn = !view->IsDrawn();
  } else {
    anchor_not_drawn =
        (features::IsWebUILocationBarEnabled() ||
         features::IsWebUIPinnedToolbarActionsEnabled()) &&
        (anchor.IsNull() || (toolbar_webview_ && !toolbar_webview_->IsDrawn()));
  }
  // In app windows the location bar view may exist but not be drawn. Avoid
  // anchoring bubbles to a non-drawn view (e.g. on Ozone/Wayland) and always
  // return a valid view anchor by falling back to the contents view.
  if (anchor_not_drawn && browser_view_) {
    auto* top_container = browser_view_->top_container();
    CHECK(top_container);
    return views::BubbleAnchor(top_container);
  }
  return anchor;
}

views::BubbleAnchor ToolbarView::GetPageActionBubbleAnchor(
    actions::ActionId action_id) {
  page_actions::PageActionViewInterface* view =
      GetPageActionViewInterface(action_id);
  if (view) {
    return view->GetBubbleAnchor();
  }
  return views::BubbleAnchor();
}

void ToolbarView::ZoomChangedForActiveTab(bool can_show_bubble) {
  auto* zoom_view_controller = browser_->GetActiveTabInterface()
                                   ->GetTabFeatures()
                                   ->zoom_view_controller();
  CHECK(zoom_view_controller);
  zoom_view_controller->UpdatePageActionIconAndBubbleVisibility(
      /*prefer_to_show_bubble=*/can_show_bubble, /*from_user_gesture=*/false);
}

AvatarToolbarButtonInterface* ToolbarView::GetAvatarToolbarButtonInterface() {
  if (features::IsWebUIAvatarButtonEnabled()) {
    return toolbar_webview_
               ? toolbar_webview_->GetAvatarToolbarButtonInterface()
               : nullptr;
  }
  return avatar_;
}

ToolbarButton* ToolbarView::GetBackButton() {
  return back_;
}

ReloadControl* ToolbarView::GetReloadButton() {
  if (features::IsWebUIReloadButtonEnabled()) {
    if (toolbar_webview_) {
      return toolbar_webview_->GetReloadControl();
    } else {
      return nullptr;
    }
  }
  return reload_;
}

ToolbarButton* ToolbarView::GetDownloadButton() {
  return pinned_toolbar_actions_container_
             ? pinned_toolbar_actions_container_->GetButtonFor(
                   kActionShowDownloads)
             : nullptr;
}

WebUIToolbarWebView* ToolbarView::GetWebUIToolbarViewForTesting() {
  return toolbar_webview_;
}

std::optional<BrowserRootView::DropIndex> ToolbarView::GetDropIndex(
    const ui::DropTargetEvent& event) {
  return BrowserRootView::DropIndex{
      .index = browser_->GetTabStripModel()->active_index(),
      .relative_to_index =
          BrowserRootView::DropIndex::RelativeToIndex::kReplaceIndex};
}

BrowserRootView::DropTarget* ToolbarView::GetDropTarget(
    gfx::Point loc_in_local_coords) {
  return HitTestPoint(loc_in_local_coords) ? this : nullptr;
}

views::View* ToolbarView::GetViewForDrop() {
  return this;
}

void ToolbarView::OnChromeLabsPrefChanged() {
  actions::ActionItem* chrome_labs_action =
      pinned_toolbar_actions_container_->GetActionItemFor(
          kActionShowChromeLabs);
  chrome_labs_action->SetVisible(
      show_chrome_labs_button_.GetValue() &&
      ShouldShowChromeLabsUI(browser_->GetProfile()));
  GetViewAccessibility().AnnounceText(l10n_util::GetStringUTF16(
      chrome_labs_action->GetVisible()
          ? IDS_ACCESSIBLE_TEXT_CHROMELABS_BUTTON_ADDED_BY_ENTERPRISE_POLICY
          : IDS_ACCESSIBLE_TEXT_CHROMELABS_BUTTON_REMOVED_BY_ENTERPRISE_POLICY));
}

void ToolbarView::LoadImages() {
  DCHECK_EQ(display_mode_, DisplayMode::kNormal);

  if (extensions_container_) {
    extensions_container_->UpdateAllIcons();
  }
}

void ToolbarView::OnShowForwardButtonChanged() {
  UpdateToolbarLayout();
}

void ToolbarView::OnShowHomeButtonChanged() {
  if (noventra_docking_) return;
  if (home_) {
    home_->SetVisible(show_home_button_.GetValue());
  }
}

void ToolbarView::OnShowBackButtonChanged() {
  UpdateToolbarLayout();
}

void ToolbarView::OnShowReloadButtonChanged() {
  UpdateToolbarLayout();
}

void ToolbarView::OnShowAvatarButtonChanged() {
  if (noventra_docking_) return;
  avatar_->SetVisible(show_avatar_button_.GetValue());
}

void ToolbarView::OnShowExtensionsButtonChanged() {
  if (noventra_docking_) return;
  extensions_container_->SetVisible(show_extensions_button_.GetValue());
}

void ToolbarView::OnShowMenuButtonChanged() {
  if (noventra_docking_) return;
  app_menu_button_->SetVisible(show_menu_button_.GetValue());
  InvalidateLayout();
}

void ToolbarView::OnShowMediaButtonChanged() {
  if (noventra_docking_) return;
  media_button_->GetController()->SetCanShowToolbarButton(
      show_media_button_.GetValue());
  InvalidateLayout();
}

void ToolbarView::OnLocationBarCenteredChanged() {
  InvalidateLayout();
}

void ToolbarView::UpdateLocationBarCentering() {
  if (noventra_docking_) return;
  if (display_mode_ != DisplayMode::kNormal || !layout_manager_ ||
      !location_bar_is_centered_.GetValue()) {
    return;
  }

  // Use the original FlexLayout slot to avoid shrinking on repeated updates.
  const auto layout = layout_manager_->GetProposedLayout(size());
  const auto* location_bar_layout = layout.GetLayoutFor(location_bar_view_);
  if (location_bar_layout && location_bar_layout->visible) {
    // Recenter using the parent's final bounds without relaying out the toolbar.
    location_bar_view_->SetBoundsRect(GetCenteredLocationBarBounds(
        location_bar_layout->bounds, layout.host_size.width()));
  }
}

gfx::Rect ToolbarView::GetCenteredLocationBarBounds(gfx::Rect bounds,
                                                    int toolbar_width) const {
  if (IsCompactLayout() || display_mode_ != DisplayMode::kNormal ||
      !location_bar_is_centered_.GetValue()) {
    return bounds;
  }

  const int width_reduction = CenteredLocationBarWidthReduction(toolbar_width);
  if (width_reduction == 0) {
    return bounds;
  }

  const int centered_width =
      std::clamp(bounds.width() - width_reduction,
                 location_bar_view_->GetMinimumSize().width(),
                 kCenteredLocationBarMaxWidth);

  // Center against the full width of the toolbar's parent. If that position
  // does not fit in the slot assigned by layout, fall back to centering within
  // the slot instead.
  int centered_x = bounds.x() + (bounds.width() - centered_width) / 2;
  if (parent() && centered_width <= bounds.width()) {
    const int ideal_centered_x = (parent()->width() - centered_width) / 2 - x();
    const int min_centered_x = bounds.x();
    const int max_centered_x = bounds.right() - centered_width;
    if (ideal_centered_x >= min_centered_x &&
        ideal_centered_x <= max_centered_x) {
      centered_x = ideal_centered_x;
    }
  }

  bounds.set_x(centered_x);
  bounds.set_width(centered_width);
  return bounds;
}

void ToolbarView::OnTouchUiChanged() {
  if (display_mode_ == DisplayMode::kNormal) {
    // Update the internal margins for touch layout.
    // TODO(dfried): I think we can do better than this by making the touch UI
    // code cleaner.
    const int default_margin =
        GetLayoutConstant(LayoutConstant::kToolbarElementPadding);
    const int location_bar_margin =
        GetLayoutConstant(LayoutConstant::kLocationBarMargin);
    layout_manager_->SetDefault(views::kMarginsKey,
                                gfx::Insets::VH(0, default_margin));
    if (location_bar_view_) {
      location_bar_view_->SetProperty(views::kMarginsKey,
                                      gfx::Insets::VH(0, location_bar_margin));
    }

    LoadImages();
    PreferredSizeChanged();
  }
}

void ToolbarView::SetForwardButtonVisibility(bool visible) {
  if (noventra_docking_) return;
  if (features::IsWebUIBackForwardButtonEnabled()) {
    toolbar_webview_->SetForwardVisible(visible);
  } else {
    forward_->SetVisible(visible);
  }
}

gfx::Size ToolbarView::GetBackForwardButtonSize(bool minimum_size) const {
  if (back_) {
    return minimum_size ? back_->GetMinimumSize() : back_->GetPreferredSize();
  }
  const int size = GetLayoutConstant(LayoutConstant::kToolbarButtonHeight);
  return gfx::Size(size, size);
}

BEGIN_METADATA(ToolbarView)
ADD_READONLY_PROPERTY_METADATA(bool, AppMenuFocused)
END_METADATA
