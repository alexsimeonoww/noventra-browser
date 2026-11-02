// Copyright 2025 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/ui/views/frame/layout/browser_view_tabbed_layout_impl.h"

#include <algorithm>
#include <memory>
#include <optional>
#include <utility>

#include "base/check.h"
#include "base/functional/bind.h"
#include "base/i18n/rtl.h"
#include "base/numerics/safe_conversions.h"
#include "base/trace_event/common/trace_event_common.h"
#include "base/trace_event/trace_event.h"
#include "build/build_config.h"
#include "chrome/browser/ui/animation/browser_animation_controller.h"
#include "chrome/browser/ui/animation/browser_animation_types.h"
#include "chrome/browser/ui/browser.h"
#include "chrome/browser/ui/browser_window/public/browser_window_interface.h"
#include "chrome/browser/ui/color/chrome_color_id.h"
#include "chrome/browser/ui/helium/helium_layout_state_controller.h"
#include "chrome/browser/ui/immersive/immersive_mode_controller.h"
#include "chrome/browser/ui/layout_constants.h"
#include "chrome/browser/ui/tabs/organizer/organizer_panel_controller.h"
#include "chrome/browser/ui/tabs/tab_strip_model.h"
#include "chrome/browser/ui/ui_features.h"
#include "chrome/browser/ui/views/animations/organizer_panel_animations.h"
#include "chrome/browser/ui/views/animations/side_panel_animations.h"
#include "chrome/browser/ui/views/animations/tab_strip_animations.h"
#include "chrome/browser/ui/views/bookmarks/bookmark_bar_view.h"
#include "chrome/browser/ui/views/frame/browser_view.h"
#include "chrome/browser/ui/views/frame/noventra_dock_controller.h"
#include "chrome/browser/ui/views/toolbar/toolbar_view.h"
#include "chrome/browser/ui/views/frame/custom_corners.h"
#include "chrome/browser/ui/views/frame/custom_corners_background.h"
#include "chrome/browser/ui/views/frame/layout/browser_view_layout_delegate.h"
#include "chrome/browser/ui/views/frame/layout/browser_view_layout_impl.h"
#include "chrome/browser/ui/views/frame/layout/browser_view_layout_params.h"
#include "chrome/browser/ui/views/frame/multi_contents_view.h"
#include "chrome/browser/ui/views/frame/shadow_frame_view.h"
#include "chrome/browser/ui/views/frame/tab_strip_region_view.h"
#include "chrome/browser/ui/views/frame/vertical_tab_strip_background_blur_backdrop.h"
#include "chrome/browser/ui/views/frame/vertical_tab_strip_region_view.h"
#include "chrome/browser/ui/views/helium/frame_corner_radius.h"
#include "chrome/browser/ui/views/infobars/infobar_container_view.h"
#include "chrome/browser/ui/views/side_panel/side_panel.h"
#include "chrome/browser/ui/views/side_panel/side_panel_animation_content_view.h"
#include "chrome/browser/ui/views/tabs/organizer/layout_constants.h"
#include "chrome/browser/ui/views/tabs/organizer/organizer_panel_host.h"
#include "chrome/browser/ui/views/tabs/organizer/organizer_panel_utils.h"
#include "chrome/browser/ui/views/tabs/organizer/organizer_tray_view.h"
#include "ui/base/ui_base_features.h"
#include "ui/compositor/layer.h"
#include "ui/gfx/geometry/insets.h"
#include "ui/gfx/geometry/outsets.h"
#include "ui/gfx/geometry/rect.h"
#include "ui/gfx/geometry/rounded_corners_f.h"
#include "ui/gfx/geometry/size.h"
#include "ui/gfx/geometry/size_conversions.h"
#include "ui/views/controls/separator.h"
#include "ui/views/layout/layout_provider.h"
#include "ui/views/view.h"
#include "ui/views/view_class_properties.h"
#include "ui/views/view_utils.h"

#if BUILDFLAG(IS_MAC)
#include "chrome/browser/ui/fullscreen_util_mac.h"
#endif

namespace {

// Minimum area next to caption buttons to use as a grab handle.
constexpr int kVerticalTabsGrabHandleSize = 40;

// Overlap the caption-button exclusion so the toolbar can sit flush
// against the frame controls when it is the top-most row.
constexpr int kTopToolbarExclusion = 10;

constexpr int kZenModeVTSPadding = 0;

struct ZenModeLayoutState {
  bool enabled = false;
  bool floating_vertical_tab_strip = false;
  float top_chrome_ratio = 1.0f;
  float side_chrome_ratio = 1.0f;
  int vertical_tab_strip_padding = 0;
  int vertical_tab_strip_top_padding = 0;

  bool TopChromeVisible() const { return top_chrome_ratio > 0.0f; }

  bool ShouldLayoutVerticalTabStrip() const {
    return !floating_vertical_tab_strip || side_chrome_ratio > 0.0f;
  }
};

int ScaleExtent(int extent, float ratio) {
  return base::ClampRound(std::max(0, extent) * ratio);
}

gfx::Rect SlideBoundsFromTop(const gfx::Rect& bounds, float ratio) {
  gfx::Rect slid(bounds);
  // Slide from fully above the parent's top edge to the view's shown bounds.
  slid.Offset(0, ScaleExtent(bounds.bottom(), ratio) - bounds.bottom());
  return slid;
}

int GetDynamicTabStripToolbarBottom(const views::View& toolbar,
                                    const gfx::Rect& toolbar_bounds,
                                    bool zen_mode,
                                    float top_chrome_ratio) {
  // The toolbar bounds may already be slid upward during zen reveal. Use the
  // stable shown height for the dynamic tab strip target instead of deriving a
  // target from animated bounds.
  if (zen_mode && top_chrome_ratio < 1.0f) {
    return toolbar.GetPreferredSize().height();
  }
  return toolbar_bounds.bottom();
}

gfx::Rect SlideBoundsFromSide(const gfx::Rect& bounds,
                              const gfx::Rect& visual_client_area,
                              float ratio,
                              bool right_aligned) {
  gfx::Rect slid(bounds);
  const int hidden_x = right_aligned ? visual_client_area.right()
                                     : visual_client_area.x() - bounds.width();
  slid.set_x(hidden_x + base::ClampRound((bounds.x() - hidden_x) * ratio));
  return slid;
}

ZenModeLayoutState GetZenModeLayoutState(
    const BrowserViewLayoutDelegate& delegate,
    bool vertical_tab_strip_active) {
  ZenModeLayoutState state;
  state.enabled = delegate.IsZenModeEnabledForLayout();
  state.floating_vertical_tab_strip = state.enabled &&
                                      vertical_tab_strip_active &&
                                      !delegate.IsZenModeSideChromePinned();
  state.top_chrome_ratio = delegate.GetTopChromeRevealRatio();
  state.side_chrome_ratio = delegate.GetSideChromeRevealRatio();

  if (!state.floating_vertical_tab_strip) {
    return state;
  }

  state.vertical_tab_strip_padding = kZenModeVTSPadding;
  const int revealed_top_padding = 0;
  if (delegate.IsZenModeTopChromePinned()) {
    state.vertical_tab_strip_top_padding = revealed_top_padding;
  } else {
    state.vertical_tab_strip_top_padding =
        ScaleExtent(state.vertical_tab_strip_padding,
                    1.0f - state.top_chrome_ratio) +
        ScaleExtent(revealed_top_padding, state.top_chrome_ratio);
  }
  return state;
}

// Maximum portion of the window a "size-restricted" contents-height side panel
// can take up. This is not the only limit on side panel size.
constexpr float kMaxContentsHeightSidePanelFraction = 2.f / 3.f;

// Increases the leading or trailing exclusion padding to `minimum`.
void IncreasePaddingToMinimum(BrowserLayoutParams& params, int minimum) {
  // Default to increasing the trailing exclusion padding. On ChromeOS, the
  // leading exclusion is sometimes used for a profile avatar while the trailing
  // exclusion is used for the caption buttons. This keeps the padding
  // consistent.
  if (params.trailing_exclusion.content.width() > 0.0) {
    params.trailing_exclusion.horizontal_padding =
        std::max(params.trailing_exclusion.horizontal_padding,
                 static_cast<float>(minimum));
  } else {
    params.leading_exclusion.horizontal_padding =
        std::max(params.leading_exclusion.horizontal_padding,
                 static_cast<float>(minimum));
  }
}

// Gets the sum of leading and trailing exclusions in `params` for minimum-size
// computation.
int GetExclusionWidth(const BrowserLayoutParams& params) {
  const float width = params.leading_exclusion.content.width() +
                      params.trailing_exclusion.content.width();
  const float padding = params.leading_exclusion.horizontal_padding +
                        params.trailing_exclusion.horizontal_padding;
  return base::ClampCeil(width + padding);
}

void InsetHorizontal(gfx::Rect& rect, int amount, bool leading) {
  rect.Inset(
      gfx::Insets::TLBR(0, leading ? amount : 0, 0, !leading ? amount : 0));
}

}  // namespace

// Allocate space across the vertical tabstrip, toolbar, and side panels,
// possibly modifying `params` to allocate grab handle space, and
// determining how much space to give to each of the left-size elements.
struct BrowserViewTabbedLayoutImpl::HorizontalLayout {
  VerticalTabStripCollapsedState vertical_tab_strip_collapsed_state =
      VerticalTabStripCollapsedState::kExpanded;
  int vertical_tab_strip_width = 0;
  int side_panel_width = 0;
  int min_content_width = 0;

  // The padding placed around a number of UI elements when the side panel is
  // present.
  int side_panel_padding = 0;

  // In some cases, even when there is a side panel, the top container
  // (containing the toolbar, etc.) are laid out at the top of the screen,
  // above the side panels - this is usually due to other layout
  // constraints.
  bool force_top_container_to_top = false;

  // True if the split view is present.
  bool is_split_view = false;

  bool has_side_panel() const { return side_panel_width > 0; }
};

// Describes how to render the organizer panel.
struct BrowserViewTabbedLayoutImpl::OrganizerPanelAnimation {
  OrganizerPanelLocation location = OrganizerPanelLocation::kNone;
  double reveal_amount = 0.0;
};

// Describes how to render the top of the vertical tab strip.
struct BrowserViewTabbedLayoutImpl::VerticalTabStripAnimation {
  // Is the vertical tab strip animating?
  BrowserAnimationMotion current_motion;
  // The y-value of the top of the tab strip.
  int top_offset = 0;
  // The relative size of the top corner.
  double top_corner = 0.0;
  // The relative size of the bottom corner.
  double bottom_corner = 0.0;
  // How much of the expand-on-hover is shown.
  double expand_on_hover_width = 0.0;
  // How opaque the expand-on-hover tabstrip is (transparent only).
  double expand_on_hover_opacity = 0.0;
  // How much the tab strip is expanded, not on-hover.
  double tab_strip_width = 0.0;
};

// Stores information about visual separators throughout the browser.
struct BrowserViewTabbedLayoutImpl::SeparatorInfo {
  // True if the separator should be rendered in the top container.
  bool top_container_separator = false;
  // True if the separator should be rendered in the multi-contents view.
  bool multi_contents_separator = false;
  // True if the shadow box should be rendered.
  bool shadow_box = false;
  // Whether to move the elements down to account for a separator that's present
  // only some of the time.
  bool adjust_for_missing_separator = false;
  // Whether to pad the top of the side panel.
  bool side_panel_top_padding = false;
};

// Describes how to transform the side panel animation content during
// tab transition.
struct BrowserViewTabbedLayoutImpl::SidePanelContentAnimation {
  // True if the side panel is animating in the first part of the animation
  // (opening the content height side panel and fading in the scrim view).
  bool is_animating_first_part = true;

  // The opacity of the scrim view.
  double scrim_opacity = 0.0;

  // The horizontal translation offset (in pixels) for the layer transform.
  double translation_x = 0.0;
};

// Data computed during layout which is discarded afterwards.
// Members are presented in the order they're computed.
struct BrowserViewTabbedLayoutImpl::TransientLayoutData {
  explicit TransientLayoutData(const BrowserLayoutParams& browser_params)
      : revised_params(browser_params) {}
  ~TransientLayoutData() = default;

  // These parameters should be used by the layout function and may include
  // additional padding, etc. They are generated
  BrowserLayoutParams revised_params;
  WindowState window_state;
  TabStripType tab_strip_type;
  HorizontalLayout horizontal_layout;
  OrganizerPanelAnimation organizer_panel_animation;
  VerticalTabStripAnimation vertical_tab_strip_animation;
  SeparatorInfo separator_info;
  SidePanelContentAnimation side_panel_content_animation;
};

BrowserViewTabbedLayoutImpl::BrowserViewTabbedLayoutImpl(
    std::unique_ptr<BrowserViewLayoutDelegate> delegate,
    BrowserViewLayoutViews views)
    : BrowserViewLayoutImpl(std::move(delegate), std::move(views)) {}

BrowserViewTabbedLayoutImpl::~BrowserViewTabbedLayoutImpl() = default;

BrowserViewTabbedLayoutImpl::SeparatorInfo
BrowserViewTabbedLayoutImpl::CalculateSeparatorInfo() const {
  SeparatorInfo info;

  const bool has_side_panel = layout_data_->horizontal_layout.has_side_panel();
  const bool is_force_to_top =
      has_side_panel &&
      layout_data_->horizontal_layout.force_top_container_to_top;
  const bool has_infobar = delegate().IsInfobarVisible();
  const bool is_split_view = layout_data_->horizontal_layout.is_split_view;
  const bool infobar_attached_to_rounded_frame =
      has_infobar && views().multi_contents_view->ShouldUseRoundedFrame();

  const bool would_have_separator = has_infobar || !is_split_view;
  switch (layout_data_->window_state) {
    case WindowState::kNormal:
    case WindowState::kMaximized: {
      // Side panel shadow overlay is disabled in Helium.
      info.shadow_box = false;
      const bool suppress_separator = infobar_attached_to_rounded_frame;
      // Keep the regular contents separator below the infobar.
      info.multi_contents_separator = !suppress_separator;
      info.top_container_separator = !suppress_separator && has_infobar;
      info.side_panel_top_padding = !is_force_to_top;
      info.adjust_for_missing_separator =
          suppress_separator && would_have_separator;
      break;
    }
    case WindowState::kFullscreen:
      // Mac and ChromeOS display separators on their slide-in top containers.
      // However, if top container elements are not visible (for example,
      // because they are offscreen), the separator is not shown.
      info.top_container_separator =
          delegate().IsToolbarVisible() || delegate().IsBookmarkBarVisible();
      info.side_panel_top_padding = true;
      break;
    case WindowState::kFullscreenWithToolbar:
      info.top_container_separator = would_have_separator;
      break;
  }

  return info;
}

BrowserViewTabbedLayoutImpl::SidePanelContentAnimation
BrowserViewTabbedLayoutImpl::CalculateSidePanelContentAnimation() const {
  SidePanelContentAnimation anim_info;

  SidePanelAnimationContentView* const anim_content =
      views().side_panel_animation_content;
  if (!IsParentedToAndVisible(anim_content, views().browser_view) ||
      !anim_content->layer()) {
    return anim_info;
  }

  auto* const controller = delegate().GetAnimationController();

  anim_info.scrim_opacity =
      controller
          ->GetCurrentValue(SidePanelAnimations::kSidePanel,
                            SidePanelAnimations::kContentScrimOpacity)
          .value_or(0.0);

  const std::optional<double> panel_width = controller->GetCurrentValue(
      SidePanelAnimations::kSidePanel, SidePanelAnimations::kPanelWidth);
  anim_info.is_animating_first_part = !panel_width || *panel_width == 0.0;

  const auto offset = controller->GetCurrentValue(
      SidePanelAnimations::kSidePanel,
      SidePanelAnimations::kContentTransitionOffset);
  if (!offset) {
    return anim_info;
  }

  double translation_x = *offset;
  const SidePanel* const side_panel = views().side_panel;
  const bool side_panel_leading =
      side_panel && (side_panel->IsRightAligned() == base::i18n::IsRTL());
  if (!side_panel_leading) {
    translation_x = -translation_x;
  }
  anim_info.translation_x = translation_x;

  return anim_info;
}

// Inset the leading edge of the tabstrip by the size of the swoop of the
// first tab; this is especially important for Mac, where the negative
// space of the caption button margins and the edge of the tabstrip should
// overlap. This only applies if there are no other leading buttons; if
// there are, we want a consistent gap from the caption buttons. The
// trailing edge receives the usual treatment, as it is the new tab button
// and not a tab.
int BrowserViewTabbedLayoutImpl::GetHorizontalTabStripLeadingMargin(
    const BrowserLayoutParams& params) const {
  int leading_margin = TabStyle::Get()->GetBottomCornerRadius();
  if (const gfx::Insets* internal_padding =
          views().horizontal_tab_strip_region_view->GetProperty(
              views::kInternalPaddingKey)) {
    leading_margin =
        std::max(0.f, params.leading_exclusion.horizontal_padding -
                          static_cast<float>(internal_padding->left()));
  }
  return leading_margin;
}

int BrowserViewTabbedLayoutImpl::GetVerticalTabStripContentOverlap() const {
#if BUILDFLAG(IS_LINUX) || BUILDFLAG(IS_WIN)
  // On fractional display scaling (e.g. 1.25x, 1.5x), overlap the content area
  // by 1 DIP over the vertical tab strip border to prevent subpixel seams.
  if (auto* const widget = views().browser_view->GetWidget()) {
    if (const auto display = widget->GetNearestDisplay()) {
      const float scale = display->device_scale_factor();
      return scale != std::floor(scale) ? 1 : 0;
    }
  }
#endif
  return 0;
}

std::pair<gfx::Size, gfx::Size>
BrowserViewTabbedLayoutImpl::GetMinimumTabStripSize(
    const BrowserLayoutParams& params) const {
  switch (delegate().GetTabStripType()) {
    case TabStripType::kHorizontal: {
      if (!ShowDynamicTabStrip(TabStripType::kHorizontal)) {
        return std::make_pair(gfx::Size(), gfx::Size());
      }
      auto result = views().horizontal_tab_strip_region_view->GetMinimumSize();
      result.Enlarge(GetExclusionWidth(params), 0);
      return std::make_pair(gfx::Size(), result);
    }
    case TabStripType::kVertical: {
      auto result = views().vertical_tab_strip_region_view->GetMinimumSize();
      if (GetVerticalTabStripCollapsedState() ==
          VerticalTabStripCollapsedState::kCollapsed) {
        // With a collapsed tabstrip, the tabstrip sits below the leading
        // exclusion. Add this additional height so that it's accounted for in
        // minimum size computations.
        result.set_height(
            result.height() +
            base::ClampCeil(
                params.leading_exclusion.ContentWithPadding().height()));
        // Reserve enough width to uncollapse the tabstrip even if it's
        // collapsed, or else uncollapsing the tabstrip will break the browser.
        result.set_width(std::max(
            result.width(), VerticalTabStripRegionView::kUncollapsedMinWidth));
      } else {
        result.set_width(std::max(
            result.width(),
            base::ClampCeil(
                params.leading_exclusion.ContentWithPadding().width())));
      }
      return std::make_pair(result, gfx::Size());
    }
    case TabStripType::kToolbar:
      // In compact layout, tab strip is in the toolbar
      return std::make_pair(gfx::Size(), gfx::Size());
    case TabStripType::kNone:
      return std::make_pair(gfx::Size(), gfx::Size());
  }
}

BrowserViewTabbedLayoutImpl::HorizontalLayout
BrowserViewTabbedLayoutImpl::CalculateHorizontalLayout(
    BrowserLayoutParams& params) const {
  HorizontalLayout layout;
  // Start with some preliminary values.
  layout.force_top_container_to_top =
      layout_data_->tab_strip_type != TabStripType::kHorizontal;

  layout.is_split_view = delegate().IsActiveTabSplit();

  if (IsDynamicTabStrip(layout_data_->tab_strip_type)) {
    layout.force_top_container_to_top = true;
  }

  if (views().side_panel &&
      views().side_panel->GetCurrentEntryType() == SidePanelType::kContent) {
    layout.force_top_container_to_top = true;
  }

  layout.min_content_width = kContentsContainerMinimumWidth;

  // Get information about the vertical tabstrip, if present.
  const int toolbar_minimum_width = views().toolbar->GetMinimumSize().width();
  int min_vertical_tab_strip_width = 0;
  int preferred_vertical_tab_strip_width = 0;
  if (layout_data_->tab_strip_type == TabStripType::kVertical &&
      IsParentedTo(views().vertical_tab_strip_region_view, views().browser_view)) {
    const auto* const vertical_tab_strip =
        views().vertical_tab_strip_region_view.get();
    preferred_vertical_tab_strip_width =
        vertical_tab_strip->GetPreferredSize().width();
    layout.vertical_tab_strip_collapsed_state =
        GetVerticalTabStripCollapsedState();
    if (layout.vertical_tab_strip_collapsed_state ==
        VerticalTabStripCollapsedState::kCollapsed) {
      // Collapsed tab strip always gets its preferred size.
      min_vertical_tab_strip_width = preferred_vertical_tab_strip_width;

    } else {
      // Minimum size is bounded from below by size of leading exclusion area.
      if (layout.vertical_tab_strip_collapsed_state ==
          VerticalTabStripCollapsedState::kExpanded) {
        min_vertical_tab_strip_width = std::max(
            vertical_tab_strip->GetMinimumSize().width(),
            base::ClampCeil(
                params.leading_exclusion.ContentWithPadding().width()));
      } else {
        min_vertical_tab_strip_width =
            vertical_tab_strip->GetMinimumSize().width();
      }

      // Figure out the maximum size of the vertical tabstrip that can still
      // accommodate the toolbar.
      //
      // Subtract everything that must go to the right of the vertical tabstrip
      // from the available width.
      const int remainder =
          params.visual_client_area.width() - toolbar_minimum_width -
          base::ClampCeil(
              params.trailing_exclusion.ContentWithPadding().width());
      preferred_vertical_tab_strip_width =
          std::max(min_vertical_tab_strip_width,
                   std::min(remainder, preferred_vertical_tab_strip_width));
    }
  }

  // Get information about the toolbar-height side panel, if present.
  int min_side_panel_width = 0;
  int preferred_side_panel_width = 0;
  if (const auto* const panel = views().side_panel.get();
      IsParentedToAndVisible(panel, views().browser_view)) {
    min_side_panel_width = panel->GetMinimumSize().width();
    preferred_side_panel_width = panel->GetPreferredSize().width();

    // Previously the reading mode side panel had an exception to extend to 90%
    // of the screen width, unlike other side panels. After a full-screen
    // reading mode experience was added with Immersive reading mode, the
    // exception was removed to allow reading mode to be capped at 66% of the
    // page like other side panels. However, if the maximum for other side
    // panels is reduced further in the future, special care should be taken to
    // ensure that reading mode side panel users aren't negatively impacted.
    // Another exception for reading mode may be needed, if this ever happens.
    // See crbug.com/394339052 for more details.
    if (panel->GetCurrentEntryType() == SidePanelType::kContent) {
      preferred_side_panel_width =
          std::min(preferred_side_panel_width,
                   base::ClampFloor(params.visual_client_area.width() *
                                    kMaxContentsHeightSidePanelFraction));
    }
    preferred_side_panel_width =
        std::max(min_side_panel_width, preferred_side_panel_width);

    // Add additional padding.
    layout.side_panel_padding =
        GetLayoutConstant(LayoutConstant::kSidePanelInset);

    // See if the toolbar-height side panel can fit next to the toolbar. If not,
    // it is forced into content height.
    if (!layout.force_top_container_to_top) {
      const int remainder = params.visual_client_area.width() -
                            (toolbar_minimum_width + layout.side_panel_padding);
      layout.force_top_container_to_top = remainder < min_side_panel_width;

      // If still allowing toolbar height, clamp the side panel based on what
      // the toolbar actually supports.
      if (!layout.force_top_container_to_top) {
        preferred_side_panel_width =
            std::min(preferred_side_panel_width, remainder);
      }
    }
  }

  // If the top container is not forced to the top, it occupies the same
  // horizontal row as the side panel. In this case, ensure that the minimum
  // width of the content area includes the minimum width of the toolbar.
  if (IsParentedToAndVisible(views().side_panel.get(), views().browser_view) &&
      !layout.force_top_container_to_top) {
    layout.min_content_width =
        std::max(layout.min_content_width, toolbar_minimum_width);
  }

  // Start with the minimum values for each element.
  layout.vertical_tab_strip_width = min_vertical_tab_strip_width;
  layout.side_panel_width = min_side_panel_width;

  // Determine how much space is left to allocate.
  int remaining = params.visual_client_area.width() - layout.min_content_width -
                  layout.side_panel_padding - min_vertical_tab_strip_width -
                  min_side_panel_width;

  // Keep the width of the tabstrip stable when possible; allocate as much space
  // as possible to it (up to its maximum).
  if (remaining > 0) {
    const int vertical_tab_strip_flex_amount =
        std::min(remaining, preferred_vertical_tab_strip_width -
                                min_vertical_tab_strip_width);
    layout.vertical_tab_strip_width += vertical_tab_strip_flex_amount;
    remaining -= vertical_tab_strip_flex_amount;
  }

  // Side panels won't both be fully present at the same time, so allocate the
  // rest of the remainder to both.
  if (remaining > 0) {
    const int toolbar_height_flex_amount =
        std::min(remaining, preferred_side_panel_width - min_side_panel_width);
    layout.side_panel_width += toolbar_height_flex_amount;
  }

  return layout;
}

BrowserViewTabbedLayoutImpl::OrganizerPanelAnimation
BrowserViewTabbedLayoutImpl::CalculateOrganizerPanelAnimation() const {
  OrganizerPanelAnimation anim;
  anim.reveal_amount =
      delegate()
          .GetAnimationController()
          ->GetCurrentValue(OrganizerPanelAnimations::kOrganizerPanel,
                            OrganizerPanelAnimations::kVisibleWidth)
          .value_or(0.0);
  if (anim.reveal_amount > 0.0) {
    anim.location = delegate().GetOrganizerPanelLocation();
  }
  return anim;
}

BrowserViewTabbedLayoutImpl::VerticalTabStripAnimation
BrowserViewTabbedLayoutImpl::CalculateVerticalTabStripAnimation() {
  // Since the state of the side panel and bookmarks can change outside of
  // tabstrip animations, maybe update the top corner animation value for a
  // collapsed tabstrip. This must be done before layout is calculated.

  const auto* const side_panel = views().side_panel.get();
  const bool has_side_panel = side_panel && side_panel->GetVisible();
  const double open_amount =
      has_side_panel ? side_panel->GetAnimationValue() : 0;
  const double side_panel_corner_value =
      gfx::Tween::DoubleValueBetween(open_amount, 0.0, -1.0);
  const bool is_split_view = layout_data_->horizontal_layout.is_split_view;
  const bool has_infobar = delegate().IsInfobarVisible();
  const int vertical_tab_strip_top = GetCollapsedVerticalTabStripRelativeTop();
  std::optional<double> max_uncollapsed_top_corner;

  auto* const controller = delegate().GetAnimationController();

  if (layout_data_->tab_strip_type == TabStripType::kVertical) {
    double top_corner_collapsed_state = 1.0;
    const bool would_have_separator = has_infobar || !is_split_view;
    switch (layout_data_->window_state) {
      case WindowState::kFullscreen:
        top_corner_collapsed_state = 1.0;
        break;
      case WindowState::kFullscreenWithToolbar:
        top_corner_collapsed_state =
            would_have_separator ? side_panel_corner_value : -1.0;
        max_uncollapsed_top_corner = top_corner_collapsed_state;
        break;
      case WindowState::kNormal:
      case WindowState::kMaximized:
        if (vertical_tab_strip_top > 0) {
          top_corner_collapsed_state =
              delegate().IsBookmarkBarVisible() || !would_have_separator
                  ? -1.0
                  : side_panel_corner_value;
        }
        break;
    }

    auto* const animations =
        controller->GetAnimationProvider<TabStripAnimations>();
    animations->UpdateDefaultValue(TabStripAnimations::kVerticalTabStrip,
                                   TabStripAnimations::kTopCorner,
                                   top_corner_collapsed_state);
  }

  // Now that the proper default value is set, calculate all of the corner
  // animation values.
  VerticalTabStripAnimation animation;
  animation.current_motion =
      controller->GetCurrentMotion(TabStripAnimations::kVerticalTabStrip);
  animation.top_offset = vertical_tab_strip_top;
  animation.expand_on_hover_width =
      *controller->GetCurrentValue(TabStripAnimations::kVerticalTabStrip,
                                   TabStripAnimations::kTabStripHoverWidth);
  animation.expand_on_hover_opacity =
      *controller->GetCurrentValue(TabStripAnimations::kVerticalTabStrip,
                                   TabStripAnimations::kTabStripHoverOpacity);
  animation.tab_strip_width =
      *controller->GetCurrentValue(TabStripAnimations::kVerticalTabStrip,
                                   TabStripAnimations::kTabStripWidth);
  animation.top_corner = *controller->GetCurrentValue(
      TabStripAnimations::kVerticalTabStrip, TabStripAnimations::kTopCorner);
  if (animation.expand_on_hover_width == 0.0 && max_uncollapsed_top_corner) {
    animation.top_corner =
        std::min(animation.top_corner, *max_uncollapsed_top_corner);
  }
  animation.bottom_corner = *controller->GetCurrentValue(
      TabStripAnimations::kVerticalTabStrip, TabStripAnimations::kBottomCorner);

  return animation;
}

int BrowserViewTabbedLayoutImpl::GetMinimumGrabHandlePadding() const {
  return kVerticalTabsGrabHandleSize -
         GetLayoutInsets(LayoutInset::TOOLBAR_INTERIOR_MARGIN).right();
}

gfx::Size BrowserViewTabbedLayoutImpl::GetMinimumMainAreaSize(
    const BrowserLayoutParams& params) const {
  gfx::Size toolbar_size = views().toolbar->GetMinimumSize();
  const auto tab_strip_type = delegate().GetTabStripType();
  if (tab_strip_type == TabStripType::kVertical ||
      GetHeliumLayout() != HeliumLayoutType::kClassic) {
    toolbar_size.Enlarge(GetExclusionWidth(params), 0);
  }
  const gfx::Size bookmark_bar_size =
      (views().bookmark_bar && views().bookmark_bar->GetVisible())
          ? views().bookmark_bar->GetMinimumSize()
          : gfx::Size();
  const gfx::Size infobar_container_size =
      views().infobar_container->GetMinimumSize();
  const gfx::Size contents_size = views().multi_contents_view->GetMinimumSize();

  int width = std::max({toolbar_size.width(), bookmark_bar_size.width(),
                        infobar_container_size.width(),
                            kContentsContainerMinimumWidth});
  const int height = toolbar_size.height() + bookmark_bar_size.height() +
                     infobar_container_size.height() + contents_size.height();

  return gfx::Size(width, height);
}

HeliumLayoutType BrowserViewTabbedLayoutImpl::GetHeliumLayout() const {
  const auto* browser_view =
      views::AsViewClass<BrowserView>(views().browser_view.get());
  auto* const controller = browser_view
      ? HeliumLayoutStateController::Get(
            browser_view->browser()->GetUnownedUserDataHost())
      : nullptr;

  return controller ? controller->GetBrowserLayout()
                    : HeliumLayoutType::kClassic;
}

bool BrowserViewTabbedLayoutImpl::IsDynamicTabStrip(
    TabStripType tab_strip_type) const {
  return GetHeliumLayout() == HeliumLayoutType::kDynamic &&
         tab_strip_type == TabStripType::kHorizontal &&
         IsParentedTo(views().top_container, views().browser_view) &&
         IsParentedTo(views().horizontal_tab_strip_region_view,
                      views().browser_view);
}

bool BrowserViewTabbedLayoutImpl::ShowDynamicTabStrip(
    TabStripType tab_strip_type) const {
  if (!IsDynamicTabStrip(tab_strip_type)) {
    return true;
  }

  const auto* browser_view =
      views::AsViewClass<BrowserView>(views().browser_view.get());
  const auto* browser = browser_view ? browser_view->browser() : nullptr;
  return !browser || !browser->GetTabStripModel() ||
         browser->GetTabStripModel()->count() > 1;
}

BrowserViewTabbedLayoutImpl::VerticalTabStripCollapsedState
BrowserViewTabbedLayoutImpl::GetVerticalTabStripCollapsedState() const {
  if (!views().vertical_tab_strip_region_view) {
    return VerticalTabStripCollapsedState::kExpanded;
  }
  const auto motion = delegate().GetAnimationController()->GetCurrentMotion(
      TabStripAnimations::kVerticalTabStrip);
  if (motion == TabStripAnimations::kCollapse) {
    return VerticalTabStripCollapsedState::kCollapsing;
  }
  if (motion == TabStripAnimations::kExpand) {
    return VerticalTabStripCollapsedState::kExpanding;
  }
  if (delegate().IsVerticalTabStripCollapsed()) {
    return VerticalTabStripCollapsedState::kCollapsed;
  }
  return VerticalTabStripCollapsedState::kExpanded;
}

int BrowserViewTabbedLayoutImpl::GetCollapsedVerticalTabStripRelativeTop()
    const {
  // When the top container isn't in the browser view, the exclusion won't apply
  // and the tabstrip goes all the way to the top.
  if (!IsParentedTo(views().top_container, views().browser_view)) {
    return 0;
  }

  const auto& params = layout_data_->revised_params;

  if (delegate().IsToolbarVisible()) {
    if (static_cast<ToolbarView*>(views().toolbar.get())->UsesNoventraSidebar()) {
      const auto& exclusion = delegate().IsVerticalTabStripRightAligned()
          ? params.trailing_exclusion : params.leading_exclusion;
      return views().toolbar->GetPreferredSize().height() +
          (exclusion.IsEmpty() ? 0 : base::ClampCeil(exclusion.ContentWithPadding().height()));
    }
    return GetBoundsWithExclusion(params, views().toolbar).height();
  }

  const auto& exclusion_area = delegate().IsVerticalTabStripRightAligned()
                                   ? params.trailing_exclusion
                                   : params.leading_exclusion;
  return exclusion_area.IsEmpty()
             ? 0
             : base::ClampCeil(exclusion_area.ContentWithPadding().height());
}

gfx::Size BrowserViewTabbedLayoutImpl::GetMinimumSize(
    const views::View* host) const {
  auto params = delegate().GetBrowserLayoutParams(/*use_browser_bounds=*/true);

  // This is a simplified version of the same method in
  // `BrowserViewLayoutImplOld` that assumes a standard browser.
  const auto [vertical_tabstrip_size, horizontal_tabstrip_size] =
      GetMinimumTabStripSize(params);
  if (!vertical_tabstrip_size.IsEmpty()) {
    IncreasePaddingToMinimum(params, GetMinimumGrabHandlePadding());
  }
  params.InsetHorizontal(vertical_tabstrip_size.width(), /*leading=*/true);
  const gfx::Size side_panel_size =
      views().side_panel && views().side_panel->GetVisible()
          ? views().side_panel->GetMinimumSize()
          : gfx::Size();
  const gfx::Size main_area_size = GetMinimumMainAreaSize(params);

  int min_height = horizontal_tabstrip_size.height() +
                   std::max({side_panel_size.height(), main_area_size.height(),
                             vertical_tabstrip_size.height()});

  // The browser window should have the same minimum width in all layouts.
  // Controls & stacked side views (VTS + split view + side panel) may overflow
  // or overlap at this width, but that's inevitable at scale this small.
  // If the user prefers this width, that's their choice and responsibility.
  const int min_width = kMainBrowserContentsMinimumWidth;

  // Maybe adjust for additional padding when the side panel is visible.
  if (side_panel_size.width() > 0 && !delegate().IsActiveTabSplit()) {
    const auto padding = GetLayoutConstant(LayoutConstant::kSidePanelInset);
    min_height += 2 * padding;
  }

  return gfx::Size(min_width, min_height);
}

BrowserViewTabbedLayoutImpl::ProposedLayout
BrowserViewTabbedLayoutImpl::CalculateProposedLayout(
    const BrowserLayoutParams& browser_params) const {
  // TODO(https://crbug.com/453717426): Consider caching layouts of the same
  // size if no `InvalidateLayout()` has happened.

  // Build the proposed layout here:
  TRACE_EVENT0("ui", "BrowserViewLayoutImpl::CalculateProposedLayout");
  ProposedLayout layout;

  // The window scrim covers the entire browser view.
  if (views().window_scrim) {
    layout.AddChild(views().window_scrim, browser_params.visual_client_area);
  }

  BrowserLayoutParams params = layout_data_->revised_params;
  bool needs_exclusion = true;
  const bool dynamic_tab_strip =
      IsDynamicTabStrip(layout_data_->tab_strip_type);
  const bool show_dynamic_tab_strip =
      ShowDynamicTabStrip(layout_data_->tab_strip_type);
  ZenModeLayoutState zen = GetZenModeLayoutState(
      delegate(), layout_data_->tab_strip_type == TabStripType::kVertical);
  auto* browser = static_cast<BrowserView*>(views().browser_view.get());
  if (browser->noventra_dock_controller() && browser->noventra_dock_controller()->active())
    zen = {};  // Native docks own reveal; legacy containers must not resize the page.
  std::optional<int> dynamic_tab_strip_top;
  const HorizontalLayout& horizontal_layout = layout_data_->horizontal_layout;
  const bool infobar_visible = delegate().IsInfobarVisible();
  const bool should_use_rounded_frame =
      views().multi_contents_view->ShouldUseRoundedFrame();
  const BrowserLayoutParams top_container_parent_params = params;
  const bool vertical_right_aligned =
      delegate().IsVerticalTabStripRightAligned();
  const auto* noventra_toolbar = static_cast<ToolbarView*>(views().toolbar.get());
  const bool noventra_sidebar = layout_data_->tab_strip_type == TabStripType::kVertical &&
      noventra_toolbar->UsesNoventraSidebar();

  if (IsParentedTo(views().organizer_tray, views().browser_view)) {
    const bool show_organizer_tray =
        layout_data_->organizer_panel_animation.location ==
        OrganizerPanelLocation::kOrganizerTray;
    gfx::Rect organizer_tray_bounds;
    if (show_organizer_tray) {
      const int target_width = organizer_panel::kOrganizerPanelMinWidth;
      views().organizer_tray->SetTargetWidth(target_width);
      views().organizer_tray->SetTopLeadingExclusion(
          gfx::ToCeiledSize(params.leading_exclusion.ContentWithPadding()));

      const double reveal_amount =
          delegate()
              .GetAnimationController()
              ->GetCurrentValue(OrganizerPanelAnimations::kOrganizerPanel,
                                OrganizerPanelAnimations::kVisibleWidth)
              .value_or(0.0);
      const int visible_width = base::ClampFloor(target_width * reveal_amount);

      organizer_tray_bounds =
          gfx::Rect(browser_params.visual_client_area.x(),
                    browser_params.visual_client_area.y(), visible_width,
                    browser_params.visual_client_area.height());
    }
    layout.AddChild(views().organizer_tray, organizer_tray_bounds,
                    show_organizer_tray);
  }

  // Lay out horizontal tab strip region if present.
  if (!dynamic_tab_strip &&
      IsParentedTo(views().horizontal_tab_strip_region_view,
                   views().browser_view)) {
    gfx::Rect tabstrip_bounds;
    if (layout_data_->tab_strip_type == TabStripType::kHorizontal) {
      const int leading_margin = GetHorizontalTabStripLeadingMargin(params);
      tabstrip_bounds = GetBoundsWithExclusion(
          params, views().horizontal_tab_strip_region_view, leading_margin);
      tabstrip_bounds =
          SlideBoundsFromTop(tabstrip_bounds, zen.top_chrome_ratio);
      if (tabstrip_bounds.height() > 0) {
        const int default_tabstrip_toolbar_overlap =
            ScaleExtent(GetLayoutConstant(LayoutConstant::kHeliumBasePadding),
                        zen.top_chrome_ratio);
        params.SetTop(tabstrip_bounds.bottom() -
                      default_tabstrip_toolbar_overlap);
      }
      needs_exclusion = false;
    }
    layout.AddChild(views().horizontal_tab_strip_region_view, tabstrip_bounds,
                    layout_data_->tab_strip_type == TabStripType::kHorizontal &&
                        tabstrip_bounds.height() > 0);
  }

  // This will be the area next to the vertical tab strip (if present) that will
  // be targeted for the content area when animating. It allows other elements
  // to "fly over" the contents.
  bool clip_content_for_animation = false;
  gfx::Rect unclipped_contents_region = params.visual_client_area;

  // Lay out vertical tab strip if visible.
  const VerticalTabStripAnimation& vertical_tab_strip_animation =
      layout_data_->vertical_tab_strip_animation;
  gfx::Rect vertical_tab_strip_bounds;
  if (IsParentedTo(views().vertical_tab_strip_region_view,
                   views().browser_view)) {
    if (layout_data_->tab_strip_type == TabStripType::kVertical) {
      const int vertical_tab_strip_hover_width =
          std::max(0,
                   views().vertical_tab_strip_region_view->uncollapsed_width() -
                       horizontal_layout.vertical_tab_strip_width) *
          vertical_tab_strip_animation.expand_on_hover_width;

      int vertical_tab_strip_width =
          horizontal_layout.vertical_tab_strip_width +
          vertical_tab_strip_hover_width;

      // The overall width during an expand must change monotonically.
      if (vertical_tab_strip_animation.current_motion ==
          TabStripAnimations::kExpand) {
        const int target_width =
            views().vertical_tab_strip_region_view->uncollapsed_width();
        if (last_vertical_tab_strip_width_ > target_width) {
          vertical_tab_strip_width =
              std::clamp(vertical_tab_strip_width, target_width,
                         last_vertical_tab_strip_width_);
        } else {
          vertical_tab_strip_width =
              std::clamp(vertical_tab_strip_width,
                         last_vertical_tab_strip_width_, target_width);
        }
      } else {
        last_vertical_tab_strip_width_ = vertical_tab_strip_width;
      }

      // Attach the sidebar to the window edge, including during Zen reveal.
      // Keep the floating panel's vertical spacing independent of this edge.
      const int vertical_tab_strip_x =
          vertical_right_aligned
              ? params.visual_client_area.right() - vertical_tab_strip_width
              : params.visual_client_area.x();

      int top_offset = vertical_tab_strip_animation.top_offset;
      if (zen.enabled && !noventra_toolbar->UsesNoventraSidebarControls()) {
        top_offset = ScaleExtent(top_offset, zen.top_chrome_ratio);
      }

      const int vertical_tab_strip_height =
          std::max(0, params.visual_client_area.height() - top_offset -
                          zen.vertical_tab_strip_top_padding -
                          zen.vertical_tab_strip_padding);
      vertical_tab_strip_bounds = {
          vertical_tab_strip_x,
          params.visual_client_area.y() + top_offset +
              zen.vertical_tab_strip_top_padding,
          vertical_tab_strip_width, vertical_tab_strip_height};
      if (zen.floating_vertical_tab_strip) {
        vertical_tab_strip_bounds = SlideBoundsFromSide(
            vertical_tab_strip_bounds, params.visual_client_area,
            zen.side_chrome_ratio, vertical_right_aligned);
      }

      if (layout_data_->window_state == WindowState::kFullscreenWithToolbar &&
          layout_data_->separator_info.adjust_for_missing_separator) {
        vertical_tab_strip_bounds.Inset(
            gfx::Insets::TLBR(views::Separator::kThickness, 0, 0, 0));
      }

      views().vertical_tab_strip_region_view->SetOrganizerPanelShowPercent(
          layout_data_->organizer_panel_animation.location ==
                  OrganizerPanelLocation::kVerticalTabStrip
              ? layout_data_->organizer_panel_animation.reveal_amount
              : 0.0);

      vertical_tab_strip_width = vertical_tab_strip_bounds.width();
      if (!zen.floating_vertical_tab_strip) {
        // Expand on hover overlaps the contents instead of resizing them.
        const int inset_amount = vertical_tab_strip_width -
                                 vertical_tab_strip_hover_width -
                                 GetVerticalTabStripContentOverlap();
        params.InsetHorizontal(inset_amount,
                               /*leading=*/!vertical_right_aligned);
      }

      // Let the vertical tab strip animate out over the content.
      if (vertical_tab_strip_animation.current_motion) {
        const bool vertical_tab_strip_resize_animation =
            vertical_tab_strip_animation.current_motion ==
                TabStripAnimations::kExpand ||
            vertical_tab_strip_animation.current_motion ==
                TabStripAnimations::kCollapse;
        clip_content_for_animation =
            clip_content_for_animation || vertical_tab_strip_resize_animation;
        InsetHorizontal(unclipped_contents_region,
                        VerticalTabStripRegionView::kCollapsedWidth,
                        !vertical_right_aligned);
      } else {
        InsetHorizontal(unclipped_contents_region,
                        horizontal_layout.vertical_tab_strip_width,
                        !vertical_right_aligned);
      }
    }
    layout.AddChild(views().vertical_tab_strip_region_view,
                    vertical_tab_strip_bounds,
                    layout_data_->tab_strip_type == TabStripType::kVertical &&
                        zen.ShouldLayoutVerticalTabStrip());
  }

  if (IsParentedTo(views().vertical_tab_strip_background_blur_backdrop,
                   views().browser_view)) {
    layout.AddChild(
        views().vertical_tab_strip_background_blur_backdrop,
        vertical_tab_strip_bounds,
        in_glass_mode() && features::kGlassExpandOnHoverEnabled.Get() &&
            layout_data_->vertical_tab_strip_animation.expand_on_hover_width >
                0.0f);
  }

  // When the tabstrip isn't at the top or in constrained widths, the top
  // container is laid out before all side panels.
  if (horizontal_layout.force_top_container_to_top &&
      IsParentedTo(views().top_container, views().browser_view)) {
    auto& top_container_layout =
        layout.AddChild(views().top_container, gfx::Rect());

    // Calculate the params for laying out the top container.
    const BrowserLayoutParams& parent_params =
        layout_data_->tab_strip_type == TabStripType::kVertical
            ? top_container_parent_params
            : params;
    auto top_container_params =
        parent_params.InLocalCoordinates(parent_params.visual_client_area);
    const bool sidebar_header_only = noventra_sidebar &&
        !noventra_toolbar->IsNoventraSidebarAddressVisible() &&
        !delegate().IsBookmarkBarVisible();
    if (sidebar_header_only) {
      // Child coordinates stay local to the narrow container; move its parent
      // separately for the right-sidebar layout.
      top_container_params.visual_client_area.set_width(
          horizontal_layout.vertical_tab_strip_width);
    }

    const gfx::Rect top_container_local_bounds = CalculateTopContainerLayout(
        top_container_layout, top_container_params, needs_exclusion);
    top_container_layout.bounds = GetTopContainerBoundsInParent(
        top_container_local_bounds, parent_params);
    if (sidebar_header_only && vertical_right_aligned) {
      top_container_layout.bounds.set_x(
          parent_params.visual_client_area.right() -
          top_container_layout.bounds.width());
    }

    if (layout_data_->tab_strip_type == TabStripType::kVertical &&
        !zen.floating_vertical_tab_strip && !noventra_sidebar) {
      auto& top_container_children = top_container_layout.children;
      auto bookmarks_child = top_container_children.find(views().bookmark_bar);
      if (bookmarks_child != top_container_children.end() &&
          !bookmarks_child->second.bounds.IsEmpty()) {
        const int vertical_width = horizontal_layout.vertical_tab_strip_width;
        gfx::Rect& bookmarks_bounds = bookmarks_child->second.bounds;
        bookmarks_bounds.set_width(
            std::max(0, bookmarks_bounds.width() - vertical_width));
        if (!vertical_right_aligned) {
          bookmarks_bounds.set_x(bookmarks_bounds.x() + vertical_width);
        }
      }
      auto separator_child =
          top_container_children.find(views().top_container_separator);
      if (separator_child != top_container_children.end() &&
          !separator_child->second.bounds.IsEmpty()) {
        const int vertical_width = horizontal_layout.vertical_tab_strip_width;
        gfx::Rect& separator_bounds = separator_child->second.bounds;
        separator_bounds.set_width(
            std::max(0, separator_bounds.width() - vertical_width));
        if (!vertical_right_aligned) {
          separator_bounds.set_x(separator_bounds.x() + vertical_width);
        }
      }
    }

    if (dynamic_tab_strip &&
        layout_data_->tab_strip_type == TabStripType::kHorizontal) {
      const int toolbar_tabstrip_overlap =
          GetLayoutConstant(LayoutConstant::kHeliumBasePadding);
      if (const auto* toolbar_layout =
              top_container_layout.GetLayoutFor(views().toolbar);
          toolbar_layout && !toolbar_layout->bounds.IsEmpty()) {
        const int toolbar_bottom = GetDynamicTabStripToolbarBottom(
            *views().toolbar, toolbar_layout->bounds, zen.enabled,
            zen.top_chrome_ratio);
        dynamic_tab_strip_top = top_container_layout.bounds.y() +
                                toolbar_bottom - toolbar_tabstrip_overlap;
      } else {
        dynamic_tab_strip_top = top_container_layout.bounds.y();
      }
    }

    if (!noventra_sidebar || noventra_toolbar->IsNoventraSidebarAddressVisible() ||
        delegate().IsBookmarkBarVisible()) {
      // Reserve the editor/bookmarks row when explicitly shown. The ordinary
      // sidebar has no row above the page. Keeping the editor in normal flow
      // preserves the WebContents/native-control paint and input order.
      params.SetTop(top_container_layout.bounds.bottom());
    }

    if (zen.enabled && noventra_toolbar->UsesNoventraSidebarControls()) {
      // The header and tabs form one sidebar, revealed together from its edge.
      // Moving the container retains native child coordinates and hit testing.
      top_container_layout.bounds = SlideBoundsFromSide(
          top_container_layout.bounds, parent_params.visual_client_area,
          zen.side_chrome_ratio, vertical_right_aligned);
    } else if (zen.enabled) {
      AdjustTopContainerForZenMode(top_container_layout, params,
                                   zen.top_chrome_ratio);
    }

    // The floating VTS offset only accounts for toolbar height, but the
    // top container may be taller (e.g. bookmarks bar, zen border strip).
    // Ensure the floating VTS starts below the full top container.
    if (zen.floating_vertical_tab_strip && !noventra_toolbar->UsesNoventraSidebarControls() && zen.TopChromeVisible() &&
        top_container_layout.bounds.height() > 0) {
      auto it = layout.children.find(views().vertical_tab_strip_region_view);
      if (it != layout.children.end()) {
        const int tc_bottom = top_container_layout.bounds.bottom();
        const int floating_top = tc_bottom + zen.vertical_tab_strip_top_padding;
        if (it->second.bounds.y() < floating_top) {
          it->second.bounds.set_y(floating_top);
          it->second.bounds.set_height(
              std::max(0, browser_params.visual_client_area.bottom() -
                              floating_top - zen.vertical_tab_strip_padding));
        }
      }
    }
  }

  if (dynamic_tab_strip) {
    DCHECK_EQ(layout_data_->tab_strip_type, TabStripType::kHorizontal);

    const int tabstrip_height =
        views().horizontal_tab_strip_region_view->GetPreferredSize().height();
    gfx::Rect tabstrip_bounds;
    if (show_dynamic_tab_strip) {
      tabstrip_bounds = gfx::Rect(
          params.visual_client_area.x(),
          dynamic_tab_strip_top.value_or(params.visual_client_area.y()),
          params.visual_client_area.width(), tabstrip_height);
      tabstrip_bounds =
          SlideBoundsFromTop(tabstrip_bounds, zen.top_chrome_ratio);
    } else {
      tabstrip_bounds =
          gfx::Rect(params.visual_client_area.x(), -tabstrip_height,
                    params.visual_client_area.width(), tabstrip_height);
    }
    layout.AddChild(views().horizontal_tab_strip_region_view, tabstrip_bounds,
                    show_dynamic_tab_strip && tabstrip_bounds.height() > 0);
  }

  // Lay out the main area background.
  ProposedLayout* main_background_layout = nullptr;
  if (IsParentedTo(views().main_background_region, views().browser_view)) {
    gfx::Rect main_background_bounds;
    if (layout_data_->separator_info.adjust_for_missing_separator) {
      // In the future this should just be a 1px shim to avoid covering
      // transparency in the frame/tab strip, instead of extending the entire
      // background to the window edge.
      main_background_bounds = gfx::Rect(
          browser_params.visual_client_area.x(), params.visual_client_area.y(),
          browser_params.visual_client_area.width(),
          params.visual_client_area.height());
    } else {
      main_background_bounds = params.visual_client_area;
    }
    const bool main_background_visibility = horizontal_layout.has_side_panel();
    main_background_layout =
        &layout.AddChild(views().main_background_region, main_background_bounds,
                         main_background_visibility);
  }

  // Lay out toolbar-height side panel.
  bool side_panel_leading = false;
  bool leading_side_chrome_attached = false;
  bool trailing_side_chrome_attached = false;
  bool leading_vertical_tab_strip_attached = false;
  bool trailing_vertical_tab_strip_attached = false;
  const double side_panel_reveal_amount =
      horizontal_layout.has_side_panel()
          ? views().side_panel->GetAnimationValue()
          : 0.0;
  bool side_panel_is_animating = false;
  if (horizontal_layout.has_side_panel()) {
    const SidePanel* const side_panel = views().side_panel;
    side_panel_leading = side_panel->IsRightAligned() == base::i18n::IsRTL();
    if (side_panel_reveal_amount > 0.0) {
      if (side_panel_leading) {
        leading_side_chrome_attached = true;
      } else {
        trailing_side_chrome_attached = true;
      }
    }

    if (side_panel_reveal_amount < 1.0) {
      clip_content_for_animation = true;
      side_panel_is_animating = true;
    }

    // Not all of the width may be visible on the screen.
    const int target_width = horizontal_layout.side_panel_width;
    const int visible_width =
        base::ClampFloor(target_width * side_panel_reveal_amount);

    // Add `container_inset_padding` to the top of the side panel to separate it
    // from the horizontal tab strip. SidePanel draws the top on top of the top
    // content separator and some units of the toolbar by default, which is not
    // needed for the side panel.
    const int top = params.visual_client_area.y() +
                    (layout_data_->separator_info.side_panel_top_padding
                         ? horizontal_layout.side_panel_padding
                         : 0);
    gfx::Rect side_panel_bounds(
        side_panel_leading
            ? params.visual_client_area.x() - (target_width - visible_width)
            : params.visual_client_area.right() - visible_width,
        top, target_width, params.visual_client_area.bottom() - top);
    layout.AddChild(views().side_panel, side_panel_bounds);

    // Lay out the animating contents.
    if (IsParentedToAndVisible(views().side_panel_animation_content,
                               views().browser_view)) {
      gfx::Rect side_panel_final_bounds(
          side_panel_leading ? params.visual_client_area.x()
                             : params.visual_client_area.right() - target_width,
          side_panel_bounds.y(), target_width, side_panel_bounds.height());
      gfx::Rect side_panel_animation_content_bounds =
          views().side_panel->GetContentAnimationBounds(
              side_panel_final_bounds);

      layout.AddChild(views().side_panel_animation_content,
                      side_panel_animation_content_bounds);
    }

    params.InsetHorizontal(visible_width, side_panel_leading);
    if (side_panel_reveal_amount == 1.0) {
      InsetHorizontal(unclipped_contents_region, visible_width,
                      side_panel_leading);
    }
  }

  gfx::Insets shadow_overlay_insets;
  if (layout_data_->separator_info.shadow_box) {
    // As the side panel animates in, the main panel shrinks and moves over to
    // accommodate the panel.
    const int scaled_main_area_padding = base::ClampRound(
        side_panel_reveal_amount * horizontal_layout.side_panel_padding);
    const int scaled_top_padding =
        layout_data_->separator_info.side_panel_top_padding
            ? scaled_main_area_padding
            : views::Separator::kThickness;
    shadow_overlay_insets = gfx::Insets::TLBR(
        scaled_top_padding, side_panel_leading ? 0 : scaled_main_area_padding,
        scaled_main_area_padding,
        side_panel_leading ? scaled_main_area_padding : 0);
    params.Inset(shadow_overlay_insets);
    if (!side_panel_is_animating) {
      unclipped_contents_region.Inset(shadow_overlay_insets);
    }
  }

  // Lay out the shadow overlay.
  layout.AddChild(views().main_shadow_overlay, params.visual_client_area,
                  layout_data_->separator_info.shadow_box);

  // Lay out top container. The top container is laid out after the
  // toolbar-height side panel with a horizontal tabstrip if the browser is wide
  // enough.
  if (!horizontal_layout.force_top_container_to_top &&
      IsParentedTo(views().top_container, views().browser_view)) {
    auto& top_container_layout =
        layout.AddChild(views().top_container, gfx::Rect());
    const gfx::Rect top_container_local_bounds = CalculateTopContainerLayout(
        top_container_layout,
        params.InLocalCoordinates(params.visual_client_area), needs_exclusion);
    top_container_layout.bounds =
        GetTopContainerBoundsInParent(top_container_local_bounds, params);
    params.SetTop(top_container_layout.bounds.bottom());
  }

  if (layout_data_->tab_strip_type == TabStripType::kVertical &&
      !zen.floating_vertical_tab_strip) {
    if (delegate().IsVerticalTabStripRightAligned()) {
      trailing_side_chrome_attached = true;
      trailing_vertical_tab_strip_attached = true;
    } else {
      leading_side_chrome_attached = true;
      leading_vertical_tab_strip_attached = true;
    }
  }

  // Lay out infobar container.
  if (IsParentedTo(views().infobar_container, views().browser_view)) {
    views().infobar_container->SetRoundedFrameState(
        should_use_rounded_frame, leading_side_chrome_attached,
        trailing_side_chrome_attached);
    gfx::Rect infobar_bounds;
    if (infobar_visible) {
      // Infobars slide down with top container reveal, but not when they're in
      // the toolbar-height side panel shadow box. This is because they only
      // slide down when they are visually adjacent to the toolbar/bookmarks
      // bar.
      const int additional_offset = layout_data_->separator_info.shadow_box
                                        ? 0
                                        : delegate().GetExtraInfobarOffset();
      const int infobar_height =
          views().infobar_container->GetPreferredSize().height();
      // The content starts below the infobar, but is not affected by the
      // extra offset from the toolbar reveal animation. The infobar slides
      // down to stay visible below the revealed toolbar, while the content
      // stays in place. See https://crbug.com/40278831.
      infobar_bounds =
          gfx::Rect(params.visual_client_area.x(),
                    params.visual_client_area.y() + additional_offset,
                    params.visual_client_area.width(), infobar_height);
      params.SetTop(params.visual_client_area.y() + infobar_height);
    }
    layout.AddChild(views().infobar_container, infobar_bounds, infobar_visible);
  }

  // Top separator is unnecessary when already in the shadow box; this is
  // especially obvious in split view, where turning the separator off provides
  // the required top padding.
  const auto& separator_info = layout_data_->separator_info;
  // Sidebar reveal changes the header's animation ratio, but that header is
  // beside the page, not above it. Never turn its hover animation into a page
  // separator or a changing content inset.
  const bool sidebar_controls =
      noventra_sidebar && noventra_toolbar->UsesNoventraSidebarControls();
  const bool sidebar_header =
      sidebar_controls &&
      !noventra_toolbar->IsNoventraSidebarAddressVisible() &&
      !delegate().IsBookmarkBarVisible();
  views().multi_contents_view->SetTopChromeAttachmentState(
      infobar_visible && should_use_rounded_frame,
      separator_info.multi_contents_separator &&
          (infobar_visible ||
           (!sidebar_header &&
            (sidebar_controls || !zen.enabled || zen.TopChromeVisible()))));

  // Updating the top, left and right insets for contents container view when
  // in split view. This is dependent on a number of other browser states.
  if (horizontal_layout.is_split_view) {
    const int top_inset =
        layout_data_->window_state == WindowState::kFullscreen ||
                infobar_visible || sidebar_header
            ? MultiContentsView::kSplitViewContentInset
            : zen.enabled && !sidebar_controls
                  ? ScaleExtent(MultiContentsView::kSplitViewContentInset,
                                std::clamp(1.0f - zen.top_chrome_ratio, 0.0f,
                                           1.0f))
                  : 0;
    const bool include_leading_inset =
        separator_info.shadow_box ||
        !(horizontal_layout.has_side_panel() && side_panel_leading);
    const bool include_trailing_inset =
        separator_info.shadow_box ||
        !(horizontal_layout.has_side_panel() && !side_panel_leading);
    const int default_inset = MultiContentsView::kSplitViewContentInset;
    const int panel_side_inset =
        base::ClampRound((1.0 - side_panel_reveal_amount) * default_inset);
    const int leading_inset =
        leading_vertical_tab_strip_attached
            ? 0
            : (include_leading_inset ? default_inset : panel_side_inset);
    const int trailing_inset =
        trailing_vertical_tab_strip_attached
            ? 0
            : (include_trailing_inset ? default_inset : panel_side_inset);

    const gfx::Insets contents_insets = gfx::Insets::TLBR(
        top_inset, leading_inset, default_inset, trailing_inset);

    views().multi_contents_view->SetSplitViewInsets(contents_insets);
  }

  views().multi_contents_view->SetAttachedSideChromeVisibility(
      leading_side_chrome_attached, trailing_side_chrome_attached);

  // Update the multi-contents view about if we will be animating content
  // bounds. This is to make optimizations during animations e.g. avoid
  // repositioning status bubble.
  const bool is_origin_changing =
      (side_panel_is_animating && !views().side_panel->IsRightAligned()) ||
      (vertical_tab_strip_animation.current_motion && !base::i18n::IsRTL());
  views().multi_contents_view->SetIsAnimatingContent(
      side_panel_is_animating || vertical_tab_strip_animation.current_motion,
      is_origin_changing);

  // Lay out contents container. The contents container contains the multi-
  // contents view when multi-contents are enabled. The checks here are to
  // force the logic to be updated when multi-contents is fully rolled-out.
  CHECK(IsParentedToAndVisible(views().multi_contents_view,
                               views().browser_view));

  // Because side panels have minimum width, in a small browser, it is possible
  // for the combination of minimum-sized contents pane and minimum-sized side
  // panel may exceed the width of the window. In this case, the contents pane
  // slides under the side panel.
  int content_left = params.visual_client_area.x();
  int content_right = params.visual_client_area.right();
  if (const int deficit = horizontal_layout.min_content_width -
                          params.visual_client_area.width();
      deficit > 0) {
    // However, do not let this go past the edge of the allowed area.
    content_left =
        std::max(content_left, browser_params.visual_client_area.x());
    content_right =
        std::min(content_right, browser_params.visual_client_area.right());
  }
  auto& contents_layout =
      layout.AddChild(views().multi_contents_view,
                      gfx::Rect(content_left, params.visual_client_area.y(),
                                content_right - content_left,
                                params.visual_client_area.height()));

  // Maybe expand and clip the web contents to avoid issues during animation.
  if (features::UseSidePanelFlyoverAnimation()) {
    if (clip_content_for_animation) {
      // Vertical span is whatever it is, minus any shadow overlay insets.
      unclipped_contents_region.set_y(contents_layout.bounds.y());
      unclipped_contents_region.set_height(contents_layout.bounds.height());
      if (side_panel_is_animating) {
        // During animation, want the vertical extent of the contents region to
        // be the exact extent that the contents will reach in the fully-closed
        // state. This means adjusting for the shadow box insets *and* (in some
        // cases) the addition of a separator.
        int top_inset = shadow_overlay_insets.top();
        if (layout_data_->separator_info.adjust_for_missing_separator) {
          top_inset = std::max(0, top_inset - views::Separator::kThickness);
        }
        unclipped_contents_region.Outset(gfx::Outsets::TLBR(
            top_inset, 0, shadow_overlay_insets.bottom(), 0));
      }

      // Avoid cases where these areas are somehow misaligned (shouldn't happen,
      // but want to avoid visual artifacts if they are).
      contents_layout.bounds.Intersect(unclipped_contents_region);
      const auto clip_insets =
          unclipped_contents_region.InsetsFrom(contents_layout.bounds);

      // Set the target size of the contents.
      views().multi_contents_view->SetTargetContentBounds(
          MultiContentsView::TargetContentBounds(
              unclipped_contents_region.size(), clip_insets));

      // Paint the main background during animations when clipping is
      // required. This prevents "cracking" at the edge of the contents area
      // as the clip region is manipulated.
      if (main_background_layout) {
        main_background_layout->visibility = true;
      }
    } else {
      views().multi_contents_view->SetTargetContentBounds(std::nullopt);
    }
  }

  // If the window goes out of glass mode, these changes won't hurt anything.
  if (in_glass_mode()) {
    gfx::RoundedCornersF content_corners;
    if (layout_data_->tab_strip_type == TabStripType::kVertical &&
        !is_fullscreen(layout_data_->window_state)) {
      // Note that this will set a lower leading corner on the multi contents
      // view even if there's a shadow box, but since the curve is effectively
      // the same this will not produce a visual bug.
      const int radius =
          views().browser_view->GetLayoutProvider()->GetCornerRadiusMetric(
              views::ShapeContextTokens::kContentSeparatorRadius);
      if (base::i18n::IsRTL()) {
        content_corners.set_lower_right(radius);
      } else {
        content_corners.set_lower_left(radius);
      }
    }
    views().multi_contents_view->SetBackgroundRadii(content_corners);
  }

  // Only the legacy parent owns the proposed bounds used for exclusions.
  // Docked tab regions receive their bounds from the native dock controller.
  if (layout_data_->tab_strip_type == TabStripType::kVertical &&
      IsParentedTo(views().vertical_tab_strip_region_view, views().browser_view)) {
    const auto toolbar_bounds =
        layout.GetBoundsFor(views().toolbar, views().browser_view);
    const auto tabstrip_bounds = layout.GetBoundsFor(
        views().vertical_tab_strip_region_view, views().browser_view);
    CHECK(tabstrip_bounds);

    bool has_leading_exclusion = false;

    // If the toolbar is not in the browser, then the exclusion isn't either.
    if (toolbar_bounds) {
      const BrowserLayoutExclusionArea& side_exclusion =
          vertical_right_aligned ? browser_params.trailing_exclusion
                                 : browser_params.leading_exclusion;
      const int side_exclusion_width =
          base::ClampCeil(side_exclusion.ContentWithPadding().width());
      const int side_distance_from_client_edge =
          vertical_right_aligned ? browser_params.visual_client_area.right() -
                                       tabstrip_bounds->right()
                                 : tabstrip_bounds->x();
      has_leading_exclusion =
          side_exclusion_width - side_distance_from_client_edge - 10 > 0;
    }

    views().vertical_tab_strip_region_view->SetHasLeadingExclusionForLayout(
        has_leading_exclusion);
    using FloatingStyle = VerticalTabStripRegionView::FloatingStyle;
    FloatingStyle floating_style = FloatingStyle::kNone;
    int floating_corner_radius = GetGenericFrameRadius();
    if (zen.floating_vertical_tab_strip) {
      floating_style = FloatingStyle::kDetached;
    } else if (vertical_tab_strip_animation.expand_on_hover_width > 0.0) {
      floating_style = vertical_right_aligned ? FloatingStyle::kAttachedTrailing
                                              : FloatingStyle::kAttachedLeading;
      // Square off the corners as the strip reaches its collapsed width, so
      // they never uncover the page's own rounded corners.
      floating_corner_radius = std::clamp(
          tabstrip_bounds->width() - horizontal_layout.vertical_tab_strip_width,
          0, floating_corner_radius);
    }
    // Zen mode can hide the toolbar, leaving nothing above the strip to join.
    const bool below_toolbar = toolbar_bounds && toolbar_bounds->bottom() > 0;
    views().vertical_tab_strip_region_view->SetFloatingStyleForLayout(
        floating_style, floating_corner_radius, below_toolbar);
  }

  if (IsParentedTo(views().side_panel_content_transition_scrim,
                   views().browser_view)) {
    const bool is_scrim_visible =
        layout_data_->side_panel_content_animation.scrim_opacity > 0.0;
    gfx::Rect scrim_bounds;
    if (is_scrim_visible) {
      int scrim_top = params.visual_client_area.y();
      if (!horizontal_layout.force_top_container_to_top &&
          IsParentedTo(views().top_container, views().browser_view)) {
        // The top container was already laid out, so we can use its top edge.
        scrim_top =
            layout.GetBoundsFor(views().top_container, views().browser_view)
                ->y();
      }

      scrim_bounds =
          gfx::Rect(unclipped_contents_region.x(), scrim_top,
                    unclipped_contents_region.width(),
                    browser_params.visual_client_area.bottom() - scrim_top);
    }
    layout.AddChild(views().side_panel_content_transition_scrim, scrim_bounds,
                    is_scrim_visible);
  }

  return layout;
}

gfx::Rect BrowserViewTabbedLayoutImpl::CalculateTopContainerLayout(
    ProposedLayout& layout,
    BrowserLayoutParams params,
    bool needs_exclusion) const {
  const int original_top = params.visual_client_area.y();
  const bool dynamic_tab_strip =
      IsDynamicTabStrip(layout_data_->tab_strip_type);
  const bool show_dynamic_tab_strip =
      ShowDynamicTabStrip(layout_data_->tab_strip_type);
  ZenModeLayoutState zen = GetZenModeLayoutState(
      delegate(), layout_data_->tab_strip_type == TabStripType::kVertical);
  auto* browser = static_cast<BrowserView*>(views().browser_view.get());
  if (browser->noventra_dock_controller() && browser->noventra_dock_controller()->active())
    zen = {};  // Native docks own reveal; legacy containers must not resize the page.

  if (IsParentedTo(views().horizontal_tab_strip_region_view,
                   views().top_container)) {
    gfx::Rect tabstrip_bounds;
    if (layout_data_->tab_strip_type == TabStripType::kHorizontal) {
      const int leading_margin = GetHorizontalTabStripLeadingMargin(params);
      tabstrip_bounds = GetBoundsWithExclusion(
          params, views().horizontal_tab_strip_region_view, leading_margin);
      tabstrip_bounds =
          SlideBoundsFromTop(tabstrip_bounds, zen.top_chrome_ratio);
      if (tabstrip_bounds.height() > 0) {
        params.SetTop(tabstrip_bounds.bottom() -
                      ScaleExtent(GetLayoutConstant(
                                      LayoutConstant::kTabstripToolbarOverlap),
                                  zen.top_chrome_ratio));
      }
      needs_exclusion = false;
    }
    layout.AddChild(views().horizontal_tab_strip_region_view, tabstrip_bounds,
                    layout_data_->tab_strip_type == TabStripType::kHorizontal &&
                        tabstrip_bounds.height() > 0);
  }

  // Lay out toolbar. If toolbar is in the top-most row, it will go into the
  // exclusion area.
  // A docked browser reparents its controls out of the legacy toolbar. An
  // empty toolbar must not acquire the caption exclusion's height: that would
  // leave a permanent strip above the page even when the overlay is hidden.
  const bool docked = browser->noventra_dock_controller() &&
                      browser->noventra_dock_controller()->active();
  const bool toolbar_visible = delegate().IsToolbarVisible() &&
      (!docked || views().toolbar->GetPreferredSize().height() > 0);
  const bool noventra_sidebar = layout_data_->tab_strip_type == TabStripType::kVertical &&
      static_cast<ToolbarView*>(views().toolbar.get())->UsesNoventraSidebar();
  if (IsParentedTo(views().toolbar, views().top_container)) {
    gfx::Rect toolbar_bounds;
    if (toolbar_visible) {
      // Toolbar is in the top row in all layouts except Classic.
      const bool is_not_classic =
          GetHeliumLayout() != HeliumLayoutType::kClassic;
      const int exclusion = is_not_classic ? kTopToolbarExclusion : 0;

      toolbar_bounds =
          needs_exclusion && !noventra_sidebar
              ? GetBoundsWithExclusion(params, views().toolbar, exclusion)
              : gfx::Rect(params.visual_client_area.x(),
                          params.visual_client_area.y(),
                          params.visual_client_area.width(),
                          views().toolbar->GetPreferredSize().height());
      if (noventra_sidebar) {
        const auto* toolbar = static_cast<ToolbarView*>(views().toolbar.get());
        if (toolbar->IsNoventraSidebarAddressVisible()) {
          toolbar_bounds = GetBoundsWithExclusion(params, views().toolbar, 0);
        } else {
          const int sidebar_width = layout_data_->horizontal_layout.vertical_tab_strip_width;
          const bool right = delegate().IsVerticalTabStripRightAligned();
          toolbar_bounds.set_width(sidebar_width);
          if (delegate().IsBookmarkBarVisible() && right) {
            toolbar_bounds.set_x(params.visual_client_area.right() - sidebar_width);
          }
          const auto& exclusion = right ? params.trailing_exclusion
                                        : params.leading_exclusion;
          if (!exclusion.IsEmpty()) {
            toolbar_bounds.Offset(0, base::ClampCeil(exclusion.ContentWithPadding().height()));
          }
        }
      }
      if (!static_cast<ToolbarView*>(views().toolbar.get())->UsesNoventraSidebarControls()) {
        toolbar_bounds = SlideBoundsFromTop(toolbar_bounds, zen.top_chrome_ratio);
      }
      params.SetTop(toolbar_bounds.bottom());
      needs_exclusion = false;
    }
    layout.AddChild(views().toolbar, toolbar_bounds,
                    toolbar_visible && toolbar_bounds.height() > 0);
  }

  // Clamp the margins visually by overlapping toolbar and the tab strip
  if (dynamic_tab_strip && show_dynamic_tab_strip && zen.TopChromeVisible()) {
    const int toolbar_tabstrip_overlap =
        toolbar_visible ? GetLayoutConstant(LayoutConstant::kHeliumBasePadding)
                        : 0;
    params.SetTop(
        params.visual_client_area.y() +
        ScaleExtent(views().horizontal_tab_strip_region_view->GetPreferredSize()
                            .height() -
                        toolbar_tabstrip_overlap,
                    zen.top_chrome_ratio));
  }

  // Lay out the bookmarks bar if one is present.
  const bool bookmarks_visible = delegate().IsBookmarkBarVisible();
  const float bookmarks_reveal_ratio =
      noventra_sidebar &&
              static_cast<ToolbarView*>(views().toolbar.get())
                  ->UsesNoventraSidebarControls()
          ? 1.0f
          : zen.top_chrome_ratio;
  if (IsParentedTo(views().bookmark_bar, views().top_container)) {
    const int sidebar_width = noventra_sidebar
        ? layout_data_->horizontal_layout.vertical_tab_strip_width : 0;
    const gfx::Rect bookmarks_bounds(
        params.visual_client_area.x() +
            (delegate().IsVerticalTabStripRightAligned() ? 0 : sidebar_width),
        params.visual_client_area.y(),
        std::max(0, params.visual_client_area.width() - sidebar_width),
        bookmarks_visible && bookmarks_reveal_ratio > 0.0f
            ? ScaleExtent(views().bookmark_bar->GetPreferredSize().height(),
                          bookmarks_reveal_ratio)
            : 0);
    layout.AddChild(views().bookmark_bar, bookmarks_bounds,
                    bookmarks_visible && bookmarks_reveal_ratio > 0.0f &&
                        bookmarks_bounds.height() > 0);
    params.SetTop(noventra_sidebar
        ? std::max(params.visual_client_area.y(), bookmarks_bounds.bottom())
        : bookmarks_bounds.bottom());
  }

  // Maybe show the separator in the top container.
  if (IsParentedTo(views().top_container_separator, views().top_container)) {
    gfx::Rect separator_bounds;
    if (layout_data_->separator_info.top_container_separator &&
        zen.TopChromeVisible() && !noventra_sidebar) {
      const int separator_height = ScaleExtent(
          views().top_container_separator->GetPreferredSize().height(),
          zen.top_chrome_ratio);
      separator_bounds = gfx::Rect(
          params.visual_client_area.x(), params.visual_client_area.y(),
          params.visual_client_area.width(), separator_height);
      params.SetTop(separator_bounds.bottom());
    }
    layout.AddChild(views().top_container_separator, separator_bounds,
                    layout_data_->separator_info.top_container_separator &&
                        zen.TopChromeVisible() &&
                        separator_bounds.height() > 0);
  }

  return gfx::Rect(params.visual_client_area.x(), original_top,
                   params.visual_client_area.width(),
                   params.visual_client_area.y() - original_top);
}

void BrowserViewTabbedLayoutImpl::AdjustTopContainerForZenMode(
    ProposedLayout& top_container_layout,
    BrowserLayoutParams& params,
    float top_chrome_ratio) const {
#if BUILDFLAG(IS_LINUX)
  constexpr int kZenModeTopBorderHeight = 4;
  if (top_chrome_ratio >= 1.0f || views().browser_view->bounds().y() > 0) {
    return;
  }
  if (top_container_layout.bounds.height() < kZenModeTopBorderHeight) {
    top_container_layout.bounds.set_height(kZenModeTopBorderHeight);
    params.SetTop(top_container_layout.bounds.bottom());
  }
#endif  // BUILDFLAG(IS_LINUX)
}

void BrowserViewTabbedLayoutImpl::ConfigureTopContainerBackground(
    const BrowserLayoutParams& params,
    CustomCornersBackground* background) {
  CHECK(layout_data_);
  const bool frame_has_top_border = views().browser_view->bounds().y() > 0;
  const bool zen_mode = delegate().IsZenModeEnabledForLayout();
  const bool zen_hiding =
      zen_mode && delegate().GetTopChromeRevealRatio() < 1.0f;

  if (zen_hiding && frame_has_top_border) {
    background->SetVisible(false);
    background->SetCorners({});
  } else {
    background->SetVisible(true);
  }

  if (zen_mode && !frame_has_top_border) {
    if (zen_hiding) {
      background->SetPrimaryColor(CustomCornersBackground::FrameTheme());
    } else {
      background->SetPrimaryColor(CustomCornersBackground::ToolbarTheme());
    }
    const auto window_corner = background->GetWindowCorner(/*upper=*/true);
    CustomCornersBackground::Corners corners;
    if (delegate().IsZenModeSideChromePinned()) {
      const bool vts_right = delegate().IsVerticalTabStripRightAligned();
      if (vts_right) {
        corners[CornerOrientation::kTopLeading] = window_corner;
      } else {
        corners[CornerOrientation::kTopTrailing] = window_corner;
      }
    } else {
      corners[CornerOrientation::kTopLeading] = window_corner;
      corners[CornerOrientation::kTopTrailing] = window_corner;
    }
    background->SetCorners(corners);
    return;
  }

  // In immersive mode and ChromeOS touch UI mode, the top Chrome UI is
  // parented to the `top_container()` and the frame header is not visible.
  // In these cases, the top container's background color should match the
  // frame color to ensure visual consistency.
  if (layout_data_->tab_strip_type == TabStripType::kHorizontal &&
      !IsDynamicTabStrip(layout_data_->tab_strip_type) &&
      IsParentedTo(views().horizontal_tab_strip_region_view,
                   views().top_container)) {
    background->SetPrimaryColor(ui::kColorFrameActive);
  } else {
    background->SetPrimaryColor(CustomCornersBackground::ToolbarTheme());
  }

}

void BrowserViewTabbedLayoutImpl::OnLayoutParamsChanged(
    const BrowserLayoutParams& old_params,
    const BrowserLayoutParams& new_params) {
  CHECK(layout_data_);
  // If the size of the visual client area changes, the size of our working
  // client area should change by the same amount.
  const gfx::Insets insets =
      new_params.visual_client_area.InsetsFrom(old_params.visual_client_area);
  layout_data_->revised_params.visual_client_area.Inset(insets);
}

void BrowserViewTabbedLayoutImpl::OnGlassModeChanged() {
  if (IsParentedToAndVisible(views().horizontal_tab_strip_region_view,
                             views().browser_view)) {
    views().horizontal_tab_strip_region_view->InvalidateLayout();
  }
  if (IsParentedToAndVisible(views().vertical_tab_strip_region_view,
                             views().browser_view)) {
    views().vertical_tab_strip_region_view->InvalidateLayout();
  }
}

void BrowserViewTabbedLayoutImpl::DoPreLayoutComputations(
    const BrowserLayoutParams& params) {
  layout_data_ = std::make_unique<TransientLayoutData>(params);
  layout_data_->window_state = delegate().GetBrowserWindowState();
  layout_data_->tab_strip_type = delegate().GetTabStripType();
  layout_data_->horizontal_layout =
      CalculateHorizontalLayout(layout_data_->revised_params);
  layout_data_->organizer_panel_animation = CalculateOrganizerPanelAnimation();
  layout_data_->vertical_tab_strip_animation =
      CalculateVerticalTabStripAnimation();
  layout_data_->separator_info = CalculateSeparatorInfo();
  layout_data_->side_panel_content_animation =
      CalculateSidePanelContentAnimation();
}

void BrowserViewTabbedLayoutImpl::DoPostLayoutVisualAdjustments(
    const BrowserLayoutParams& params) {
  int side_panel_start = layout_data_->revised_params.visual_client_area.x();
  int side_panel_end =
      layout_data_->revised_params.visual_client_area.right();

  if (layout_data_->tab_strip_type == TabStripType::kVertical &&
      IsParentedTo(views().vertical_tab_strip_region_view, views().browser_view)) {
    const gfx::Rect vertical_tab_strip_bounds =
        views().vertical_tab_strip_region_view->bounds();
    if (delegate().IsVerticalTabStripRightAligned()) {
      side_panel_end = vertical_tab_strip_bounds.x();
    } else {
      side_panel_start = vertical_tab_strip_bounds.right();
    }

    views().vertical_tab_strip_region_view->UpdatePanelClips();

  }

  if (IsParentedTo(views().organizer_tray, views().browser_view)) {
    if (views().organizer_tray->GetVisible()) {
      views().organizer_tray->UpdatePanelClip();
    }
  }

  views().side_panel_content_transition_scrim->layer()->SetOpacity(
      layout_data_->side_panel_content_animation.scrim_opacity);

  // Animate the tab content horizontally to create the
  // illusion of the tab content sliding into the side panel.
  SidePanelAnimationContentView* const anim_content =
      views().side_panel_animation_content;
  if (IsParentedToAndVisible(anim_content, views().browser_view)) {
    anim_content->UpdateHorizontalTranslation(
        layout_data_->side_panel_content_animation.translation_x);

    // Clip the animation layer to prevent it from running into the
    // vertical tab strip.
    const gfx::Rect allowed_bounds = views().multi_contents_view->bounds();
    const gfx::Rect transformed_bounds =
        anim_content->bounds() +
        gfx::Vector2d(layout_data_->side_panel_content_animation.translation_x,
                      0);
    const gfx::Rect visible_bounds =
        gfx::IntersectRects(transformed_bounds, allowed_bounds);
    if (visible_bounds != transformed_bounds &&
        layout_data_->side_panel_content_animation.is_animating_first_part) {
      const gfx::Rect local_clip =
          visible_bounds - transformed_bounds.OffsetFromOrigin();
      anim_content->ClipBounds(local_clip);
    } else {
      anim_content->ClipBounds(gfx::Rect());
    }
  }

  // Clip the side panel so it doesn't run off the edge of the browser or into
  // the vertical tab strip.
  if (IsParentedToAndVisible(views().side_panel, views().browser_view)) {
    const int left_overlap =
        std::max(0, side_panel_start - views().side_panel->x());
    const int right_overlap = std::max(
        0, views().side_panel->bounds().right() - side_panel_end);
    if (left_overlap > 0 || right_overlap > 0) {
      gfx::Rect side_panel_visible_bounds =
          views().side_panel->GetLocalBounds();
      side_panel_visible_bounds.Inset(
          gfx::Insets::TLBR(0, left_overlap, 0, right_overlap));
      if (side_panel_visible_bounds.IsEmpty()) {
        // Can't let this be empty or the clipping won't work, so if it's zero
        // width add a pixel that won't matter.
        if (left_overlap) {
          side_panel_visible_bounds.Outset(gfx::Outsets::TLBR(0, 0, 0, 1));
        } else {
          side_panel_visible_bounds.Outset(gfx::Outsets::TLBR(0, 1, 0, 0));
        }
      }
      views().side_panel->SetClipToVisibleArea(side_panel_visible_bounds);
    } else {
      views().side_panel->SetClipToVisibleArea(std::nullopt);
    }
  }
}

void BrowserViewTabbedLayoutImpl::DoPostLayoutCleanup() {
  layout_data_.reset();
}
