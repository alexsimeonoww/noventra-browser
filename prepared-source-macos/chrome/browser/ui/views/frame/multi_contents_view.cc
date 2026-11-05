// Copyright 2025 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/ui/views/frame/multi_contents_view.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <utility>

#include "base/check_deref.h"
#include "base/notreached.h"
#include "build/build_config.h"
#include "chrome/browser/actor/ui/actor_overlay_web_view.h"
#include "chrome/browser/browser_process.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/sessions/session_service.h"
#include "chrome/browser/sessions/session_service_factory.h"
#include "chrome/browser/ui/browser_element_identifiers.h"
#include "chrome/browser/ui/color/chrome_color_id.h"
#include "chrome/browser/ui/read_anything/read_anything_immersive_overlay_view.h"
#include "chrome/browser/ui/sad_tab_helper.h"
#include "chrome/browser/ui/tabs/tab_strip_model.h"
#include "chrome/browser/ui/view_ids.h"
#include "chrome/browser/ui/views/frame/browser_view.h"
#include "chrome/browser/ui/views/frame/contents_container_view.h"
#include "chrome/browser/ui/views/frame/contents_separator.h"
#include "chrome/browser/ui/views/frame/contents_web_view.h"
#include "chrome/browser/ui/views/frame/multi_contents_background_view.h"
#include "chrome/browser/ui/views/frame/multi_contents_drop_target_view.h"
#include "chrome/browser/ui/views/frame/multi_contents_resize_area.h"
#include "chrome/browser/ui/views/frame/multi_contents_view_delegate.h"
#include "chrome/browser/ui/views/frame/multi_contents_view_drop_target_controller.h"
#include "chrome/browser/ui/views/frame/safe_invoke/safe_invoke.h"
#include "chrome/browser/ui/views/frame/scrim_view.h"
#include "chrome/browser/ui/views/frame/themed_background.h"
#include "chrome/browser/ui/views/new_tab_footer/footer_web_view.h"
#include "chrome/browser/ui/views/toolbar/toolbar_view.h"
#include "chrome/common/pref_names.h"
#include "chrome/common/webui_url_constants.h"
#include "components/prefs/pref_service.h"
#include "components/split_tabs/split_tab_visual_data.h"
#include "components/tabs/public/split_tab_data.h"
#include "components/tabs/public/tab_interface.h"
#include "content/public/browser/web_contents.h"
#include "content/public/common/url_constants.h"
#include "ui/base/metadata/metadata_impl_macros.h"
#include "ui/base/ozone_buildflags.h"
#include "ui/color/color_id.h"
#include "ui/color/color_provider.h"
#include "ui/compositor/layer.h"
#include "ui/gfx/geometry/outsets.h"
#include "ui/gfx/geometry/rect.h"
#include "ui/gfx/geometry/rounded_corners_f.h"
#include "ui/views/accessibility/view_accessibility.h"
#include "ui/views/background.h"
#include "ui/views/border.h"
#include "ui/views/controls/label.h"
#include "ui/views/layout/flex_layout_types.h"
#include "ui/views/layout/layout_types.h"
#include "ui/views/layout/proposed_layout.h"
#include "ui/views/view_class_properties.h"
#include "ui/views/view_utils.h"
#include "ui/views/widget/widget.h"

#if BUILDFLAG(IS_MAC)
#include "chrome/browser/ui/fullscreen_util_mac.h"
#endif

namespace {
constexpr int kSnapDistance = 15;
constexpr float kZenTopChromeAttachedThreshold = 0.5f;

bool IsTopChromeAttached(const BrowserView& browser_view,
                         bool should_show_top_separator) {
  return should_show_top_separator &&
         (!browser_view.IsZenModeEnabledForLayout() ||
          (browser_view.toolbar() &&
           browser_view.toolbar()->UsesNoventraSidebarControls()) ||
          browser_view.GetTopChromeRevealRatio() >=
              kZenTopChromeAttachedThreshold);
}

}  // namespace

void MultiContentsView::ContentsSeparators::Reset() {
  top_separator = nullptr;
  leading_separator = nullptr;
  trailing_separator = nullptr;
  leading_side_chrome_attached = false;
  trailing_side_chrome_attached = false;
}

MultiContentsView::MultiContentsView(
    BrowserView* browser_view,
    std::unique_ptr<MultiContentsViewDelegate> delegate)
    : browser_view_(browser_view),
      delegate_(std::move(delegate)),
      split_view_insets_(gfx::Insets::TLBR(0,
                                           kSplitViewContentInset,
                                           kSplitViewContentInset,
                                           kSplitViewContentInset)) {
  SetLayoutManager(std::make_unique<views::DelegatingLayoutManager>(this));
  SetProperty(views::kElementIdentifierKey, kMultiContentsViewElementId);

  background_view_ =
      AddChildView(std::make_unique<MultiContentsBackgroundView>(browser_view));

  contents_container_views_.push_back(
      AddChildView(std::make_unique<ContentsContainerView>(browser_view_)));
  contents_container_views_[0]
      ->contents_view()
      ->set_is_primary_web_contents_for_window(true);

  resize_area_ = AddChildView(std::make_unique<MultiContentsResizeArea>(
      this, browser_view_->browser()->tab_strip_model()));
  resize_area_->SetVisible(false);

  contents_container_views_.push_back(
      AddChildView(std::make_unique<ContentsContainerView>(browser_view_)));
  contents_container_views_[1]->SetVisible(false);

  for (int i = 2; i < 4; ++i) {
    auto* container = AddChildView(std::make_unique<ContentsContainerView>(browser_view_));
    container->SetVisible(false);
    contents_container_views_.push_back(container);
    auto* handle = AddChildView(std::make_unique<MultiContentsResizeArea>(
        this, browser_view_->browser()->tab_strip_model(), i - 1));
    handle->SetVisible(false);
    extra_resize_areas_.push_back(handle);
  }

  drop_target_view_ =
      AddChildView(std::make_unique<MultiContentsDropTargetView>());
  drop_target_controller_ =
      std::make_unique<MultiContentsViewDropTargetController>(
          *drop_target_view_, *delegate_, g_browser_process->local_state(),
          browser_view_->browser()->GetTabStripModel());

  contents_separators_.top_separator =
      AddChildView(ContentsSeparator::CreateLayerBasedContentsSeparator());
  contents_separators_.top_separator->SetProperty(
      views::kElementIdentifierKey, kContentsSeparatorTopEdgeElementId);

  contents_separators_.leading_separator =
      AddChildView(ContentsSeparator::CreateLayerBasedContentsSeparator());
  contents_separators_.leading_separator->SetProperty(
      views::kElementIdentifierKey, kContentsSeparatorLeadingEdgeElementId);

  contents_separators_.trailing_separator =
      AddChildView(ContentsSeparator::CreateLayerBasedContentsSeparator());
  contents_separators_.trailing_separator->SetProperty(
      views::kElementIdentifierKey, kContentsSeparatorTrailingEdgeElementId);

  // Create the view that will house the Lens overlay. This view is visible but
  // transparent view that is used as a container for the Lens overlay WebView.
  // It must have a higher index than contents_view so that it is drawn on top
  // of it. Uses a fill layout so that the overlay WebView can fill the entire
  // container.
  auto lens_overlay_view = std::make_unique<views::View>();
  lens_overlay_view->SetID(VIEW_ID_LENS_OVERLAY);
  lens_overlay_view->SetProperty(views::kElementIdentifierKey,
                                 kLensOverlayViewElementId);
  lens_overlay_view->SetVisible(false);
  lens_overlay_view->SetLayoutManager(std::make_unique<views::FillLayout>());
  lens_overlay_view_ = AddChildView(std::move(lens_overlay_view));

  for (auto& contents_container_view : contents_container_views_) {
    auto& view_map = container_focusable_map_[contents_container_view];

    auto* contents_view = contents_container_view->contents_view();
    view_map[contents_view->GetClassName()] = contents_view;

    contents_focused_subscriptions_.push_back(
        contents_view->AddWebContentsFocusedCallback(base::BindRepeating(
            &MultiContentsView::OnWebContentsFocused, base::Unretained(this))));

    if (auto* footer = contents_container_view->new_tab_footer_view()) {
      view_map[footer->GetClassName()] = footer;
      contents_focused_subscriptions_.push_back(
          footer->AddWebContentsFocusedCallback(base::BindRepeating(
              &MultiContentsView::OnNtpFooterFocused, base::Unretained(this))));
    }

    if (auto* actor_overlay =
            contents_container_view->actor_overlay_web_view()) {
      view_map[actor_overlay->GetClassName()] = actor_overlay;
      contents_focused_subscriptions_.push_back(
          actor_overlay->AddWebContentsFocusedCallback(
              base::BindRepeating(&MultiContentsView::OnActorOverlayFocused,
                                  base::Unretained(this))));
    }

    if (auto* read_anything_overlay =
            contents_container_view->read_anything_immersive_overlay_view()) {
      view_map[read_anything_overlay->GetClassName()] = read_anything_overlay;
      contents_focused_subscriptions_.push_back(
          read_anything_overlay->AddWebViewFocusedCallback(base::BindRepeating(
              &MultiContentsView::OnReadAnythingOverlayFocused,
              base::Unretained(this), contents_container_view)));
    }
  }

  auto* const profile_prefs = browser_view_->GetProfile()->GetPrefs();
  is_drag_drop_pref_enabled_ =
      profile_prefs->GetBoolean(prefs::kSplitViewDragAndDropEnabled);
  is_rounded_frame_pref_enabled_ =
      profile_prefs->GetBoolean(prefs::kHeliumRoundedFrame);

  pref_change_registrar_.Init(profile_prefs);
  pref_change_registrar_.Add(
      prefs::kSplitViewDragAndDropEnabled,
      base::BindRepeating(&MultiContentsView::OnDragAndDropPrefStateChange,
                          base::Unretained(this)));
  pref_change_registrar_.Add(
      prefs::kHeliumRoundedFrame,
      base::BindRepeating(&MultiContentsView::OnRoundedFramePrefStateChange,
                          base::Unretained(this)));

  UpdateContentsBorderAndOverlay();
}

MultiContentsView::~MultiContentsView() {
  HideNoventraSplitDropPreview();
  noventra_split_drop_preview_ = nullptr;
  // Clear the map and vectors before `RemoveAllChildViews()` to avoid having
  // dangling pointers.
  container_focusable_map_.clear();
  contents_container_views_.clear();
  extra_resize_areas_.clear();
  if (drop_target_controller_) {
    drop_target_controller_.reset();
  }
  drop_target_view_ = nullptr;
  lens_overlay_view_ = nullptr;
  resize_area_ = nullptr;
  contents_separators_.Reset();
  background_view_ = nullptr;
  RemoveAllChildViews();
}

ContentsWebView* MultiContentsView::GetActiveContentsView() const {
  return GetActiveContentsContainerView()->contents_view();
}

ContentsWebView* MultiContentsView::GetInactiveContentsView() const {
  return GetInactiveContentsContainerView()->contents_view();
}

ContentsContainerView* MultiContentsView::GetActiveContentsContainerView()
    const {
  return contents_container_views_[active_index_];
}

ContentsContainerView* MultiContentsView::GetInactiveContentsContainerView()
    const {
  return contents_container_views_[GetInactiveIndex()];
}

const gfx::RoundedCornersF& MultiContentsView::GetBackgroundRadii() const {
  return background_view_->GetRoundedCorners();
}

void MultiContentsView::SetBackgroundRadii(const gfx::RoundedCornersF& radii) {
  if (radii == GetBackgroundRadii()) {
    return;
  }

  background_view_->SetRoundedCorners(radii);

  if (!IsInSplitView()) {
    GetActiveContentsContainerView()->SetRoundedCorners(radii);
    GetInactiveContentsContainerView()->SetRoundedCorners(
        gfx::RoundedCornersF());
  }
}

ContentsContainerView* MultiContentsView::GetContentsContainerViewFor(
    content::WebContents* web_contents) const {
  for (auto& container_view : contents_container_views_) {
    if (container_view->contents_view()->web_contents() == web_contents) {
      return container_view;
    }
  }
  return nullptr;
}

const MultiContentsView::FocusableViewMap*
MultiContentsView::GetFocusableViewsMapFor(
    const ContentsContainerView* container) const {
  auto it = container_focusable_map_.find(container);
  if (it != container_focusable_map_.end()) {
    return &it->second;
  }

  return nullptr;
}

tabs::TabInterface* MultiContentsView::GetNoventraSplitDropTarget(
    const gfx::Point& pointer) const {
  for (const auto& pane : contents_container_views_) {
    if (!pane->GetVisible() || !pane->GetBoundsInScreen().Contains(pointer))
      continue;
    auto* contents = pane->contents_view()->web_contents();
    return contents ? tabs::TabInterface::MaybeGetFromContents(contents)
                    : nullptr;
  }
  return nullptr;
}

gfx::Rect MultiContentsView::GetNoventraSplitDropBounds(
    const tabs::TabInterface* source,
    const tabs::TabInterface* target,
    bool after,
    bool stacked) const {
  if (!source || !target || source == target) {
    return gfx::Rect();
  }
  auto* model = browser_view_->browser()->GetTabStripModel();
  if (!model->ContainsIndex(model->GetIndexOfTab(source)) ||
      !model->ContainsIndex(model->GetIndexOfTab(target))) {
    return gfx::Rect();
  }
  auto* target_split =
      target->IsSplit() ? model->GetSplitData(*target->GetSplit()) : nullptr;
  const auto visual = target_split ? *target_split->visual_data()
                                   : split_tabs::SplitTabVisualData();
  auto members = target_split ? target_split->ListTabs()
                              : std::vector<tabs::TabInterface*>{
                                    const_cast<tabs::TabInterface*>(target)};
  const bool existing_member =
      std::ranges::find(members, source) != members.end();
  // Moving the first tab away (even temporarily in JoinNoventraSplit), or
  // moving a tab before it, resets the multi-pane weights in
  // SetWebContentsAtIndex.
  const bool resets_weights = existing_member && (members.front() == source ||
                                                  members.front() == target);
  std::erase(members, source);
  const auto position = std::ranges::find(members, target);
  if (position == members.end() || members.size() >= 4) {
    return gfx::Rect();
  }
  const int slot = position - members.begin() + (after ? 1 : 0);
  const int count = members.size() + 1;
  const auto layout =
      stacked ? split_tabs::SplitTabLayout::kStacked : visual.split_layout();
  auto* visible_contents =
      contents_container_views_[0]->contents_view()->web_contents();
  auto* visible_tab =
      visible_contents
          ? tabs::TabInterface::MaybeGetFromContents(visible_contents)
          : nullptr;
  const bool target_is_visible = IsInSplitView() && visible_tab &&
                                 target->GetSplit() == visible_tab->GetSplit();
  if (existing_member && target_is_visible && layout == visual.split_layout() &&
      (count == 2 || layout == split_tabs::SplitTabLayout::kGrid ||
       !resets_weights) &&
      slot < split_count_) {
    auto bounds = contents_container_views_[slot]->GetBoundsInScreen();
    bounds.Inset(4);
    return bounds;
  }

  // Match the future live layout, without changing selection or resizing its
  // WebContents during capture. New groups reset three/four-pane weights; two
  // panes retain the target split's ratio, including an orientation change.
  gfx::Rect bounds = GetBoundsInScreen();
  bounds.Inset(IsInSplitView() ? split_view_insets_
                               : gfx::Insets(kSplitViewContentInset));
  if (layout == split_tabs::SplitTabLayout::kGrid && count >= 3) {
    constexpr int kGap = 8;
    const auto axis_sizes = [](int length, double ratio) {
      const int usable = std::max(0, length - kGap);
      const int minimum = std::min(64, usable / 2);
      const double fraction =
          std::isfinite(ratio) ? std::clamp(ratio, 0.0, 1.0) : 0.5;
      const int first =
          std::clamp(static_cast<int>(std::round(usable * fraction)), minimum,
                     usable - minimum);
      return std::pair(first, usable - first);
    };
    const auto [left, right] = axis_sizes(bounds.width(), visual.split_ratio());
    const auto [top, bottom] =
        axis_sizes(bounds.height(), visual.grid_row_ratio());
    if (slot < 2) {
      bounds.set_x(bounds.x() + (slot == 1 ? left + kGap : 0));
      bounds.set_width(slot == 0 ? left : right);
      bounds.set_height(top);
    } else {
      bounds.set_y(bounds.y() + top + kGap);
      bounds.set_height(bottom);
      if (count == 4) {
        bounds.set_x(bounds.x() + (slot == 3 ? left + kGap : 0));
        bounds.set_width(slot == 2 ? left : right);
      }
    }
  } else {
    const bool horizontal = layout != split_tabs::SplitTabLayout::kStacked;
    const int length = horizontal ? bounds.width() : bounds.height();
    int start = 0;
    int end = 0;
    if (count == 2) {
      constexpr int kGap = MultiContentsResizeArea::kHandleResizeAxisPadding +
                           MultiContentsResizeArea::kHandleResizeAxisSize;
      int first = std::round(visual.split_ratio() * (length - kGap));
      int second = length - first - kGap;
      const int min_percentage = kMinWebContentsSizePercentage * length;
      const int minimum =
          std::min(min_contents_size_for_testing_.value_or(kMinWebContentsSize),
                   std::max(kConstrainedMinWebContentsSize, min_percentage));
      if (first < minimum) {
        second -= minimum - first;
        first = minimum;
      } else if (second < minimum) {
        first -= minimum - second;
        second = minimum;
      }
      start = slot == 0 ? 0 : first + kGap;
      end = start + (slot == 0 ? first : second);
    } else {
      constexpr int kGap = 8;
      const int usable = std::max(0, length - kGap * (count - 1));
      // CalculateProposedLayout accumulates uniform double weights before
      // converting to integer pixels; retain that rounding for each boundary.
      double accumulated = 0;
      for (int i = 0; i <= slot; ++i) {
        accumulated += 1.0 / count;
        const int boundary =
            i == count - 1 ? length
                           : static_cast<int>(usable * accumulated) + kGap * i;
        if (i == slot) {
          end = boundary;
        } else {
          start = boundary + kGap;
        }
      }
    }
    if (horizontal) {
      bounds.set_x(bounds.x() + start);
      bounds.set_width(std::max(0, end - start));
    } else {
      bounds.set_y(bounds.y() + start);
      bounds.set_height(std::max(0, end - start));
    }
  }
  bounds.Inset(4);
  return bounds;
}

void MultiContentsView::ShowNoventraSplitDropPreview(
    const gfx::Rect& screen_bounds,
    const gfx::Point& pointer,
    const std::u16string& label) {
  if (!noventra_split_drop_preview_) {
    auto overlay = std::make_unique<views::View>();
    overlay->SetProperty(views::kViewIgnoredByLayoutKey, true);
    overlay->SetCanProcessEventsWithinSubtree(false);
    overlay->GetViewAccessibility().SetName(u"Split destination preview");
    overlay->SetPaintToLayer();
    overlay->layer()->SetFillsBoundsOpaquely(false);
    overlay->layer()->SetRoundedCornerRadius(gfx::RoundedCornersF(8));
    overlay->layer()->SetIsFastRoundedCorner(true);
    noventra_split_drop_preview_ = AddChildView(std::move(overlay));
  }
  const SkColor accent = GetColorProvider()->GetColor(ui::kColorSysPrimary);
  noventra_split_drop_preview_->GetViewAccessibility().SetDescription(label);
  noventra_split_drop_preview_->SetBackground(
      views::CreateSolidBackground(SkColorSetA(accent, 42)));
  noventra_split_drop_preview_->SetBorder(views::CreateSolidBorder(2, accent));
  gfx::Rect bounds = screen_bounds;
  bounds.Offset(-GetBoundsInScreen().OffsetFromOrigin());
  bounds.Intersect(GetLocalBounds());
  noventra_split_drop_preview_->SetBoundsRect(bounds);
  noventra_split_drop_preview_->SetVisible(!bounds.IsEmpty());
  ReorderChildView(noventra_split_drop_preview_, children().size() - 1);
  if (!noventra_tab_drag_ghost_) {
    noventra_tab_drag_ghost_ = std::make_unique<views::Widget>();
    views::Widget::InitParams params(
        views::Widget::InitParams::CLIENT_OWNS_WIDGET,
        views::Widget::InitParams::TYPE_TOOLTIP);
    params.parent = GetWidget()->GetNativeView();
    params.context = GetWidget()->GetNativeWindow();
    params.accept_events = false;
    params.opacity = views::Widget::InitParams::WindowOpacity::kTranslucent;
    params.name = "NoventraTabDragGhost";
    noventra_tab_drag_ghost_->Init(std::move(params));
    auto* text = noventra_tab_drag_ghost_->SetContentsView(
        std::make_unique<views::Label>());
    text->SetBorder(views::CreateEmptyBorder(gfx::Insets::VH(6, 12)));
    text->SetElideBehavior(gfx::ELIDE_TAIL);
  }
  auto* text = views::AsViewClass<views::Label>(
      noventra_tab_drag_ghost_->GetContentsView());
  text->SetText(label);
  text->SetBackground(views::CreateRoundedRectBackground(
      GetColorProvider()->GetColor(ui::kColorDialogBackground), 8));
  text->SetEnabledColor(
      GetColorProvider()->GetColor(ui::kColorLabelForeground));
  gfx::Rect ghost(pointer.x() + 16, pointer.y() + 20, 300, 34);
  ghost.AdjustToFit(browser_view_->GetBoundsInScreen());
  noventra_tab_drag_ghost_->SetBounds(ghost);
  noventra_tab_drag_ghost_->ShowInactive();
}

void MultiContentsView::HideNoventraSplitDropPreview() {
  if (noventra_split_drop_preview_)
    noventra_split_drop_preview_->SetVisible(false);
  noventra_tab_drag_ghost_.reset();
}

bool MultiContentsView::IsInSplitView() const {
  return split_count_ > 1;
}

void MultiContentsView::SetWebContentsAtIndex(
    content::WebContents* web_contents,
    int index) {
  CHECK(index >= 0 && index < 4);
  if (index == 0 && contents_container_views_[0]->contents_view()->web_contents() != web_contents)
    pane_weights_.assign(split_count_, 1.0 / split_count_);
  contents_container_views_[index]->contents_view()->SetWebContents(web_contents);

  if (index == 1 && !contents_container_views_[1]->GetVisible()) {
    contents_container_views_[1]->SetVisible(true);
    resize_area_->SetVisible(true);
    UpdateContentsBorderAndOverlay();
  }

  SafeInvoke(web_contents)
      .Then(Overload<content::WebContents*>(
          &tabs::TabInterface::MaybeGetFromContents))
      .Then(&SadTabHelper::From)
      .Then(&SadTabHelper::ReinstallInWebView);
}

void MultiContentsView::ShowSplitView(
    split_tabs::SplitTabVisualData visual_data, int count) {
  CHECK(count >= 2 && count <= 4);
  if (split_count_ != count) pane_weights_.assign(count, 1.0 / count);
  split_count_ = count;
  for (int i = count; i < 4; ++i) {
    contents_container_views_[i]->contents_view()->SetWebContents(nullptr);
    contents_container_views_[i]->SetVisible(false);
  }
  for (int i = 0; i < count; ++i) contents_container_views_[i]->SetVisible(true);
  if (visual_data_.split_layout() != visual_data.split_layout()) {
    pane_weights_.assign(count, 1.0 / count);
  }
  InvalidateLayout();
  UpdateSplitVisualData(visual_data);
  UpdateResizeAreas();
  UpdateContentsBorderAndOverlay();
}

void MultiContentsView::CloseSplitView() {
  if (!IsInSplitView()) {
    return;
  }

  if (active_index_ != 0) {
    std::swap(contents_container_views_[0], contents_container_views_[active_index_]);
    active_index_ = 0;
  }
  for (int i = 1; i < 4; ++i) {
    contents_container_views_[i]->contents_view()->SetWebContents(nullptr);
    contents_container_views_[i]->SetVisible(false);
    contents_container_views_[i]->contents_view()->set_is_primary_web_contents_for_window(false);
  }
  split_count_ = 1;
  initial_grid_resize_size_.reset();
  initial_grid_resize_split_.reset();
  resize_area_->SetVisible(false);
  for (const auto& handle : extra_resize_areas_) handle->SetVisible(false);
  InvalidateLayout();
  UpdateContentsBorderAndOverlay();

  SafeInvoke(GetActiveContentsView()->web_contents())
      .Then(Overload<content::WebContents*>(
          &tabs::TabInterface::MaybeGetFromContents))
      .Then(&SadTabHelper::From)
      .Then(&SadTabHelper::ReinstallInWebView);
}

void MultiContentsView::SwapContentsInSplitView() {
  // Reorder the child views so that focus order will be consistent with
  // contents_container_views_.
  ContentsContainerView* start_view = contents_container_views_[0];
  ContentsContainerView* end_view = contents_container_views_[1];
  size_t start_view_child_index = GetIndexOf(start_view).value();
  size_t end_view_child_index = GetIndexOf(end_view).value();
  ReorderChildView(start_view, end_view_child_index);
  ReorderChildView(end_view, start_view_child_index);

  std::swap(contents_container_views_[0], contents_container_views_[1]);

  active_index_ = active_index_ == 0 ? 1 : 0;
}

void MultiContentsView::SetActiveIndex(int index) {
  // Index should never be less than 0 or equal to or greater than the total
  // number of contents views.
  CHECK(index >= 0 && index < 4);
  // We will only activate a visible contents view.
  CHECK(contents_container_views_[index]->GetVisible());
  active_index_ = index;
  GetActiveContentsView()->set_is_primary_web_contents_for_window(true);
  for (int i = 0; i < 4; ++i) {
    contents_container_views_[i]->contents_view()->set_is_primary_web_contents_for_window(i == index);
  }
  UpdateContentsBorderAndOverlay();
}

void MultiContentsView::UpdateSplitVisualData(
    const split_tabs::SplitTabVisualData& visual_data) {
  if (visual_data_ == visual_data) {
    return;
  }

  if (visual_data_.split_layout() != visual_data.split_layout()) {
    pane_weights_.assign(split_count_, 1.0 / split_count_);
    initial_pane_weights_.reset();
    initial_grid_resize_size_.reset();
    initial_grid_resize_split_.reset();
    initial_start_size_on_resize_.reset();
  }
  visual_data_ = visual_data;
  UpdateResizeAreas();
  UpdateContentsBorderAndOverlay();
  InvalidateLayout();
}

void MultiContentsView::SetHighlightActiveContentsView(bool is_highlighted) {
  if (active_contents_view_highlighted_ != is_highlighted) {
    active_contents_view_highlighted_ = is_highlighted;
    UpdateContentsBorderAndOverlay();
  }
}

void MultiContentsView::ExecuteOnEachVisibleContentsView(
    base::RepeatingCallback<void(ContentsWebView*)> callback) {
  for (auto& contents_container_view : contents_container_views_) {
    if (contents_container_view->GetVisible()) {
      callback.Run(contents_container_view->contents_view());
    }
  }
}

void MultiContentsView::OnSwap() {
  CHECK(IsInSplitView());
  delegate_->ReverseWebContents();
}

void MultiContentsView::SetTargetContentBounds(
    std::optional<TargetContentBounds> target_content_bounds) {
  if (target_content_bounds_ == target_content_bounds) {
    return;
  }
  target_content_bounds_ = target_content_bounds;

  InvalidateLayout(/*avoid_propagate_during_layout=*/true);
}

void MultiContentsView::SetIsAnimatingContent(bool is_animating,
                                              bool is_origin_changing) {
  for (auto& contents_container_view : contents_container_views_) {
    contents_container_view->contents_view()->SetIsAnimatingBounds(
        is_animating, is_origin_changing);
  }
}

std::vector<views::View*> MultiContentsView::GetAccessiblePanes() {
  std::vector<views::View*> accessible_panes;
  for (auto& contents_container_view : contents_container_views_) {
    if (!contents_container_view->GetVisible()) continue;
    auto contents_accessible_panes =
        contents_container_view->GetAccessiblePanes();
    accessible_panes.insert(accessible_panes.end(),
                            contents_accessible_panes.begin(),
                            contents_accessible_panes.end());
  }
  return accessible_panes;
}

void MultiContentsView::OnResize(int resize_amount, bool done_resizing) {
  if (!initial_start_size_on_resize_.has_value()) {
    initial_start_size_on_resize_ = std::make_optional(
        GetResizeAxisComponent(contents_container_views_[0]->size()));
  }
  double total_size =
      GetResizeAxisComponent(contents_container_views_[0]->size()) +
      GetResizeAxisComponent(contents_container_views_[1]->size());
  double new_start_size = initial_start_size_on_resize_.value() + resize_amount;

  // If new_start_size is within the snap point sizes, update to the snap
  // point.
  delegate_->ResizeWebContents(
      CalculateRatioWithSnapPoints(new_start_size, total_size), done_resizing);

  if (done_resizing) {
    initial_start_size_on_resize_ = std::nullopt;
  }
}

double MultiContentsView::CalculateRatioWithSnapPoints(
    double start_size,
    double total_size) const {
  for (const double& snap_point : snap_points_) {
    double dp_snap_point = snap_point * total_size;
    if (std::abs(dp_snap_point - start_size) < kSnapDistance) {
      return snap_point;
    }
  }
  return start_size / total_size;
}

void MultiContentsView::OnThemeChanged() {
  views::View::OnThemeChanged();
  UpdateContentsBorderAndOverlay();
}

void MultiContentsView::OnFullscreenStateChanged() {
  UpdateContentsBorderAndOverlay();
  InvalidateLayout();
}

int MultiContentsView::GetInactiveIndex() const {
  return active_index_ == 0 ? 1 : 0;
}

void MultiContentsView::MaybeUpdateContentsBorderAndOverlay() {
  top_chrome_attached_for_border_ = IsTopChromeAttachedForBorder();
  if (IsInSplitView() || ShouldUseRoundedFrame()) {
    UpdateContentsBorderAndOverlay();
  }
}

void MultiContentsView::MaybeUpdateBorderForZenReveal() {
  const bool top_chrome_attached = IsTopChromeAttachedForBorder();
  if (top_chrome_attached_for_border_ &&
      *top_chrome_attached_for_border_ == top_chrome_attached) {
    return;
  }
  top_chrome_attached_for_border_ = top_chrome_attached;
  if (IsInSplitView() || ShouldUseRoundedFrame()) {
    UpdateContentsBorderAndOverlay();
  }
}

int MultiContentsView::AttachedChromeEdges::TopInset() const {
  return top ? 0 : MultiContentsView::kSplitViewContentInset;
}

MultiContentsView::AttachedChromeEdges
MultiContentsView::GetAttachedChromeEdges() const {
  const bool split_view = IsInSplitView();
  AttachedChromeEdges edges;
  edges.top = IsTopChromeAttachedForBorder();
  edges.leading = contents_separators_.leading_side_chrome_attached ||
                  (split_view && split_view_insets_.left() == 0);
  edges.trailing = contents_separators_.trailing_side_chrome_attached ||
                   (split_view && split_view_insets_.right() == 0);
  return edges;
}

void MultiContentsView::OnWebContentsFocused(views::WebView* web_view) {
  if (IsInSplitView()) {
    // Check whether the widget is visible as otherwise during browser hide,
    // inactive web contents gets focus. See crbug.com/419335827
    if (GetActiveContentsView()->web_contents() != web_view->web_contents() &&
        GetWidget()->IsVisible()) {
      delegate_->WebContentsFocused(web_view->web_contents());
    }
  }
}

void MultiContentsView::OnActorOverlayFocused(views::WebView* web_view) {
  if (IsInSplitView() && GetWidget()->IsVisible()) {
    for (auto& contents_container_view : contents_container_views_) {
      if (contents_container_view->actor_overlay_web_view() &&
          contents_container_view->actor_overlay_web_view() == web_view &&
          GetActiveContentsView() !=
              contents_container_view->contents_view()) {
        return delegate_->WebContentsFocused(
            contents_container_view->contents_view()->web_contents());
      }
    }
  }
}

void MultiContentsView::OnNtpFooterFocused(views::WebView* web_view) {
  if (IsInSplitView() && GetWidget()->IsVisible()) {
    for (auto& contents_container_view : contents_container_views_) {
      if (contents_container_view->new_tab_footer_view() &&
          contents_container_view->new_tab_footer_view() == web_view &&
          GetActiveContentsView() !=
              contents_container_view->contents_view()) {
        return delegate_->WebContentsFocused(
            contents_container_view->contents_view()->web_contents());
      }
    }
  }
}

void MultiContentsView::OnReadAnythingOverlayFocused(
    ContentsContainerView* container,
    views::WebView* web_view) {
  if (IsInSplitView() && GetWidget()->IsVisible()) {
    if (GetActiveContentsContainerView() != container) {
      delegate_->WebContentsFocused(container->contents_view()->web_contents());
    }
  }
}

// TODO(crbug.com/397777917): Consider using FlexSpecification weights and
// interior margins instead of a custom layout once this bug is resolved.
views::ProposedLayout MultiContentsView::CalculateProposedLayout(
    const views::SizeBounds& size_bounds) const {
  views::ProposedLayout layouts;
  if (!size_bounds.is_fully_bounded()) {
    return layouts;
  }
  const int width = size_bounds.width().value();
  const int height = size_bounds.height().value();

  gfx::Rect available_space = gfx::Rect(width, height);

  const bool is_in_split = IsInSplitView();
  const bool should_use_rounded_frame = ShouldUseRoundedFrame();
  const AttachedChromeEdges attached_chrome = GetAttachedChromeEdges();
  const bool show_background = drop_target_view_->GetVisible() || is_in_split ||
                               should_use_rounded_frame;
  layouts.child_layouts.emplace_back(background_view_.get(), show_background,
                                     available_space);

  if (IsDragAndDropEnabled()) {
    available_space = CalculateDropTargetLayout(
        available_space, attached_chrome, layouts.child_layouts);
  }

  available_space =
      CalculateSeparatorLayouts(available_space, layouts.child_layouts);

  if (IsInSplitView()) {
    available_space.Inset(split_view_insets_);
  } else if (should_use_rounded_frame) {
    gfx::Insets single_insets(kSplitViewContentInset);
    single_insets.set_top(attached_chrome.TopInset());
    single_insets.set_left(contents_separators_.leading_side_chrome_attached
                               ? 0
                               : kSplitViewContentInset);
    single_insets.set_right(contents_separators_.trailing_side_chrome_attached
                                ? 0
                                : kSplitViewContentInset);
    available_space.Inset(single_insets);
  }
  if (GetSplitLayout() == split_tabs::SplitTabLayout::kGrid &&
      split_count_ >= 3) {
    constexpr int kGap = 8;
    const auto axis_sizes = [](int length, double ratio) {
      const int usable = std::max(0, length - kGap);
      const int minimum = std::min(64, usable / 2);
      const double fraction =
          std::isfinite(ratio) ? std::clamp(ratio, 0.0, 1.0) : 0.5;
      const int start =
          std::clamp(static_cast<int>(std::round(usable * fraction)), minimum,
                     usable - minimum);
      return std::pair(start, usable - start);
    };
    const auto [left_width, right_width] =
        axis_sizes(available_space.width(), visual_data_.split_ratio());
    const auto [top_height, bottom_height] =
        axis_sizes(available_space.height(), visual_data_.grid_row_ratio());
    const int x = available_space.x();
    const int y = available_space.y();
    layouts.child_layouts.emplace_back(contents_container_views_[0].get(), true,
                                       gfx::Rect(x, y, left_width, top_height));
    layouts.child_layouts.emplace_back(
        contents_container_views_[1].get(), true,
        gfx::Rect(x + left_width + kGap, y, right_width, top_height));
    layouts.child_layouts.emplace_back(
        contents_container_views_[2].get(), true,
        gfx::Rect(x, y + top_height + kGap,
                  split_count_ == 3 ? available_space.width() : left_width,
                  bottom_height));
    layouts.child_layouts.emplace_back(
        contents_container_views_[3].get(), split_count_ == 4,
        split_count_ == 4
            ? gfx::Rect(x + left_width + kGap, y + top_height + kGap,
                        right_width, bottom_height)
            : gfx::Rect());
    layouts.child_layouts.emplace_back(
        resize_area_.get(), true,
        gfx::Rect(x + left_width, y, kGap,
                  split_count_ == 3 ? top_height : available_space.height()));
    layouts.child_layouts.emplace_back(
        extra_resize_areas_[0].get(), true,
        gfx::Rect(x, y + top_height, available_space.width(), kGap));
    layouts.child_layouts.emplace_back(extra_resize_areas_[1].get(), false,
                                       gfx::Rect());
    layouts.child_layouts.emplace_back(lens_overlay_view_.get(),
                                       lens_overlay_view_->GetVisible(),
                                       gfx::Rect(width, height));
    layouts.host_size = gfx::Size(width, height);
    return layouts;
  }
  if (split_count_ > 2) {
    const bool horizontal = GetSplitLayout() == split_tabs::SplitTabLayout::kSideBySide;
    const int length = horizontal ? available_space.width() : available_space.height();
    const int gap = 8;
    const int usable = std::max(0, length - gap * (split_count_ - 1));
    int offset = 0;
    double accumulated = 0;
    for (int i = 0; i < split_count_; ++i) {
      accumulated += pane_weights_[i];
      const int end = i == split_count_ - 1 ? length : static_cast<int>(usable * accumulated) + gap * i;
      const gfx::Rect bounds = horizontal
          ? gfx::Rect(available_space.x() + offset, available_space.y(), std::max(0, end-offset), available_space.height())
          : gfx::Rect(available_space.x(), available_space.y() + offset, available_space.width(), std::max(0, end-offset));
      layouts.child_layouts.emplace_back(contents_container_views_[i].get(), true, bounds);
      if (i < split_count_ - 1) {
        auto* handle = i == 0 ? resize_area_.get() : extra_resize_areas_[i-1].get();
        const gfx::Rect handle_bounds = horizontal
            ? gfx::Rect(bounds.right(), bounds.y(), gap, bounds.height())
            : gfx::Rect(bounds.x(), bounds.bottom(), bounds.width(), gap);
        layouts.child_layouts.emplace_back(handle, true, handle_bounds);
      }
      offset = end + gap;
    }
    layouts.child_layouts.emplace_back(lens_overlay_view_.get(), lens_overlay_view_->GetVisible(), gfx::Rect(width, height));
    layouts.host_size = gfx::Size(width, height);
    return layouts;
  }
  ViewSizes sizes = GetViewSizes(available_space);

  gfx::Rect start_rect, resize_rect, end_rect;
  if (GetSplitLayout() != split_tabs::SplitTabLayout::kStacked) {
    start_rect = gfx::Rect(available_space.origin(),
                           gfx::Size(sizes.start, available_space.height()));
    resize_rect = gfx::Rect(start_rect.top_right(),
                            gfx::Size(sizes.resize, available_space.height()));
    end_rect = gfx::Rect(resize_rect.top_right(),
                         gfx::Size(sizes.end, available_space.height()));
  } else {
    start_rect = gfx::Rect(available_space.origin(),
                           gfx::Size(available_space.width(), sizes.start));
    resize_rect = gfx::Rect(start_rect.bottom_left(),
                            gfx::Size(available_space.width(), sizes.resize));
    end_rect = gfx::Rect(resize_rect.bottom_left(),
                         gfx::Size(available_space.width(), sizes.end));
  }

  layouts.child_layouts.emplace_back(contents_container_views_[0].get(),
                                     contents_container_views_[0]->GetVisible(),
                                     start_rect);
  layouts.child_layouts.emplace_back(resize_area_.get(),
                                     resize_area_->GetVisible(), resize_rect);
  layouts.child_layouts.emplace_back(contents_container_views_[1].get(),
                                     contents_container_views_[1]->GetVisible(),
                                     end_rect);
  layouts.child_layouts.emplace_back(lens_overlay_view_.get(),
                                     lens_overlay_view_->GetVisible(),
                                     gfx::Rect(width, height));

  layouts.host_size = gfx::Size(width, height);
  return layouts;
}

void MultiContentsView::BeforeApplyLayout(const views::ProposedLayout& layout) {
  if (!target_content_bounds_ || split_count_ > 2) {
    for (auto& contents : contents_container_views_) {
      contents->SetTargetContentBounds(std::nullopt);
    }
    return;
  }

  if (!IsInSplitView()) {
    const auto outsets = -target_content_bounds_->clipped_area.ToOutsets();
    GetActiveContentsContainerView()->SetTargetContentBounds(outsets);
    GetInactiveContentsContainerView()->SetTargetContentBounds(std::nullopt);
    return;
  }

  // Need to calculate what the layout *would* be at the actual size.
  //
  // This is a bit more expensive than a normal layout but only happens during
  // animation when the target bounds are set.
  views::ProposedLayout target_layout;
  {
    // Need to temporarily override the split view insets.
    const auto split_view_insets =
        gfx::Insets::TLBR(split_view_insets_.top(), kSplitViewContentInset,
                          kSplitViewContentInset, kSplitViewContentInset);
    base::AutoReset inset_override(&split_view_insets_, split_view_insets);
    target_layout = CalculateProposedLayout(
        views::SizeBounds(target_content_bounds_->actual_size));
  }

  const auto& default_clip = target_content_bounds_->clipped_area;

  // Due to the way web contents resize, and the fact that both split views will
  // grow or shrink as the animation progresses, always clip from the trailing
  // edge for both split webviews. This reduces jumping/popping for very slow
  // websites.

  gfx::Outsets first_outsets = gfx::Outsets::TLBR(
      default_clip.top(), 0, default_clip.bottom(), default_clip.left());
  auto* const first = contents_container_views_[0].get();
  auto* const first_current = layout.GetLayoutFor(first);
  auto* const first_target = target_layout.GetLayoutFor(first);
  if (first_current && first_target) {
    first_outsets.set_right(std::max(
        0, first_target->bounds.width() - first_current->bounds.width()));
  }

  gfx::Outsets second_outsets = gfx::Outsets::TLBR(
      default_clip.top(), 0, default_clip.bottom(), default_clip.right());
  auto* const second = contents_container_views_[1].get();
  auto* const second_current = layout.GetLayoutFor(second);
  auto* const second_target = target_layout.GetLayoutFor(second);
  if (second_current && second_target) {
    second_outsets.set_right(std::max(
        0, second_target->bounds.width() - second_current->bounds.width()));
  }

  first->SetTargetContentBounds(first_outsets);
  second->SetTargetContentBounds(second_outsets);
}

gfx::Rect MultiContentsView::CalculateDropTargetLayout(
    const gfx::Rect& available_space,
    const AttachedChromeEdges& attached_chrome,
    views::ProposedLayout::ChildLayoutList& child_layouts) const {
  CHECK(IsDragAndDropEnabled());
  if (!drop_target_view_->GetVisible()) {
    UpdateContentsBorderAndOverlay();
    child_layouts.emplace_back(drop_target_view_.get(), false, gfx::Rect());
    return available_space;
  }

  CHECK(drop_target_view_->side().has_value());
  const auto drop_side = drop_target_view_->side().value();
  const bool is_drop_target_on_start =
      drop_side == MultiContentsDropTargetView::DropSide::START;
  const bool is_drop_target_on_end =
      drop_side == MultiContentsDropTargetView::DropSide::END;
  const bool is_drop_target_on_bottom =
      drop_side == MultiContentsDropTargetView::DropSide::BOTTOM;
  const bool should_use_rounded_frame = ShouldUseRoundedFrame();
  const bool zen_mode = browser_view_->IsZenModeEnabledForLayout();
  bool drop_target_side_chrome_attached = false;
  if (is_drop_target_on_start) {
    drop_target_side_chrome_attached = attached_chrome.leading;
  } else if (is_drop_target_on_end) {
    drop_target_side_chrome_attached = attached_chrome.trailing;
  }
  const views::Widget* widget = browser_view_->GetWidget();
  const bool use_system_side_corners =
      !drop_target_side_chrome_attached && !(widget && widget->IsFullscreen());
  drop_target_view_->SetFrameGeometry(
      drop_target_side_chrome_attached,
      drop_target_side_chrome_attached || !should_use_rounded_frame,
      !is_drop_target_on_bottom && use_system_side_corners &&
          !attached_chrome.top &&
          (!should_use_rounded_frame || zen_mode),
      use_system_side_corners);
  UpdateContentsBorderAndOverlay();

  const int drop_target_size = drop_target_view_->GetSizeForAvailableSpace(
      is_drop_target_on_bottom ? available_space.height()
                               : available_space.width());

  gfx::Rect drop_target_bounds = available_space;
  gfx::Rect remaining_space = available_space;
  switch (drop_side) {
    case MultiContentsDropTargetView::DropSide::START:
      remaining_space.Inset(gfx::Insets().set_left(drop_target_size));
      break;
    case MultiContentsDropTargetView::DropSide::END:
      remaining_space.Inset(gfx::Insets().set_right(drop_target_size));
      break;
    case MultiContentsDropTargetView::DropSide::BOTTOM:
      remaining_space.Inset(gfx::Insets().set_bottom(drop_target_size));
      break;
    default:
      NOTREACHED();
  }
  drop_target_bounds.Subtract(remaining_space);

  if (!is_drop_target_on_bottom) {
    int top_margin = 0;
    if (IsInSplitView() || should_use_rounded_frame || zen_mode) {
      top_margin = attached_chrome.TopInset();
    } else if (contents_separators_.should_show_top) {
      top_margin =
          contents_separators_.top_separator->GetPreferredSize().height();
    }
    drop_target_bounds.Inset(gfx::Insets().set_top(top_margin));
  }

  child_layouts.emplace_back(drop_target_view_.get(), true, drop_target_bounds);
  return remaining_space;
}

gfx::Rect MultiContentsView::CalculateSeparatorLayouts(
    const gfx::Rect& available_space,
    views::ProposedLayout::ChildLayoutList& child_layouts) const {
  if (IsInSplitView() || ShouldUseRoundedFrame()) {
    child_layouts.emplace_back(contents_separators_.top_separator.get(), false,
                               gfx::Rect());
    child_layouts.emplace_back(contents_separators_.leading_separator.get(),
                               false, gfx::Rect());
    child_layouts.emplace_back(contents_separators_.trailing_separator.get(),
                               false, gfx::Rect());
    return available_space;
  }

  const int width = available_space.width();
  const int height = available_space.height();

  const int separator_height =
      contents_separators_.should_show_top
          ? contents_separators_.top_separator->GetPreferredSize().height()
          : 0;
  child_layouts.emplace_back(
      contents_separators_.top_separator.get(),
      contents_separators_.should_show_top,
      gfx::Rect(available_space.origin(), {width, separator_height}));

  const bool should_show_leading =
      drop_target_view_->side() == MultiContentsDropTargetView::DropSide::START;
  const int leading_separator_width =
      should_show_leading
          ? contents_separators_.leading_separator->GetPreferredSize().width()
          : 0;
  child_layouts.emplace_back(
      contents_separators_.leading_separator.get(), should_show_leading,
      gfx::Rect(available_space.origin(), {leading_separator_width, height}));

  const bool should_show_trailing =
      drop_target_view_->side() == MultiContentsDropTargetView::DropSide::END;

  const int trailing_separator_width =
      should_show_trailing
          ? contents_separators_.trailing_separator->GetPreferredSize().width()
          : 0;
  child_layouts.emplace_back(
      contents_separators_.trailing_separator.get(), should_show_trailing,
      gfx::Rect(available_space.right() - trailing_separator_width,
                available_space.y(), trailing_separator_width, height));

  return gfx::Rect(available_space.x() + leading_separator_width,
                   available_space.y() + separator_height,
                   width - trailing_separator_width - leading_separator_width,
                   height - separator_height);
}

MultiContentsView::ViewSizes MultiContentsView::GetViewSizes(
    gfx::Rect available_space) const {
  const int available_size =
      GetSplitLayout() != split_tabs::SplitTabLayout::kStacked
          ? available_space.width()
          : available_space.height();
  ViewSizes sizes;
  if (IsInSplitView()) {
    CHECK(contents_container_views_[0]->GetVisible() &&
          contents_container_views_[1]->GetVisible());
    sizes.resize = GetSplitLayout() != split_tabs::SplitTabLayout::kStacked
                       ? resize_area_->GetPreferredSize().width()
                       : resize_area_->GetPreferredSize().height();
    sizes.start = std::round(visual_data_.split_ratio() *
                             (available_size - sizes.resize));
    sizes.end = available_size - sizes.start - sizes.resize;
  } else {
    CHECK(!contents_container_views_[1]->GetVisible());
    sizes.start = available_size;
  }
  return ClampToMinSize(available_space, sizes);
}

MultiContentsView::ViewSizes MultiContentsView::ClampToMinSize(
    gfx::Rect available_space,
    ViewSizes sizes) const {
  if (!IsInSplitView()) {
    // Don't clamp if in a single-view state, where other views should be 0
    // width.
    return sizes;
  }

  const int min_size = GetMinViewSize(available_space);
  if (sizes.start < min_size) {
    const int diff = min_size - sizes.start;
    sizes.start += diff;
    sizes.end -= diff;
  } else if (sizes.end < min_size) {
    const int diff = min_size - sizes.end;
    sizes.end += diff;
    sizes.start -= diff;
  }
  return sizes;
}

int MultiContentsView::GetMinViewSize(gfx::Rect available_space) const {
  CHECK(IsInSplitView());

  // The minimum size (in the resize axis) for a content view in a split should
  // be the lesser of kMinWebContentsSize, and kMinWebContentsSizePercentage as
  // a percentage of the MultiContentsView's available size with a lower bound
  // of kConstrainedMinWebContentsSize.
  const int min_percentage =
      kMinWebContentsSizePercentage *
      (visual_data_.split_layout() != split_tabs::SplitTabLayout::kStacked
           ? available_space.width()
           : available_space.height());
  const int min_fixed_value =
      min_contents_size_for_testing_.value_or(kMinWebContentsSize);
  return std::min(min_fixed_value,
                  std::max(kConstrainedMinWebContentsSize, min_percentage));
}

bool MultiContentsView::ShouldUseRoundedFrame() const {
#if BUILDFLAG(IS_MAC)
  const bool is_fullscreen =
      fullscreen_utils::IsInContentFullscreen(browser_view_->browser());
#else
  const views::Widget* widget = browser_view_->GetWidget();
  const bool is_fullscreen = widget && widget->IsFullscreen();
#endif
  return browser_view_->GetIsNormalType() && is_rounded_frame_pref_enabled_ &&
         !is_fullscreen;
}

bool MultiContentsView::IsTopChromeAttachedForBorder() const {
  return top_edge_attached_to_infobar_ ||
         IsTopChromeAttached(*browser_view_,
                             contents_separators_.should_show_top);
}

void MultiContentsView::UpdateContentsBorderAndOverlay() const {
  const views::Widget* widget = browser_view_->GetWidget();
  const bool is_fullscreen = widget && widget->IsFullscreen();
  const bool is_in_split = IsInSplitView();
  const bool is_side_by_side =
      GetSplitLayout() == split_tabs::SplitTabLayout::kSideBySide;
  const bool should_round_single_contents =
      !is_in_split && ShouldUseRoundedFrame();
  const bool is_drop_target_visible = drop_target_view_->GetVisible();
  const bool is_drop_target_on_start =
      is_drop_target_visible &&
      drop_target_view_->side() == MultiContentsDropTargetView::DropSide::START;
  const bool is_drop_target_on_end =
      is_drop_target_visible &&
      drop_target_view_->side() == MultiContentsDropTargetView::DropSide::END;
  const bool is_drop_target_on_bottom =
      is_drop_target_visible &&
      drop_target_view_->side() == MultiContentsDropTargetView::DropSide::BOTTOM;
  const AttachedChromeEdges attached_chrome = GetAttachedChromeEdges();
  const bool top_edge_is_window_edge = !attached_chrome.top;
  const bool can_use_system_corners =
      (is_in_split || should_round_single_contents) && !is_fullscreen;
  const bool leading_edge_is_window_edge =
      !attached_chrome.leading && !is_drop_target_on_start;
  const bool trailing_edge_is_window_edge =
      !attached_chrome.trailing && !is_drop_target_on_end;

  const bool use_system_top_left_radius = can_use_system_corners &&
                                          top_edge_is_window_edge &&
                                          leading_edge_is_window_edge;
  const bool use_system_top_right_radius = can_use_system_corners &&
                                           top_edge_is_window_edge &&
                                           trailing_edge_is_window_edge;
  const bool use_system_bottom_left_radius =
      can_use_system_corners && leading_edge_is_window_edge &&
      !is_drop_target_on_bottom;
  const bool use_system_bottom_right_radius =
      can_use_system_corners && trailing_edge_is_window_edge &&
      !is_drop_target_on_bottom;

  const bool grid = GetSplitLayout() == split_tabs::SplitTabLayout::kGrid;
  int pane_index = 0;
  for (auto& contents_container_view : contents_container_views_) {
    const int index = pane_index++;
    const bool is_active =
        contents_container_view->contents_view() == GetActiveContentsView();
    const bool is_first_view =
        !is_in_split ||
        contents_container_view == contents_container_views_.front();
    const bool is_last_view =
        !is_in_split ||
        contents_container_view == contents_container_views_[split_count_ - 1];
    const bool top_row =
        !is_in_split ||
        (grid && split_count_ >= 3
             ? index < 2
             : is_side_by_side || (grid && split_count_ == 2) || is_first_view);
    const bool bottom_row =
        !is_in_split ||
        (grid && split_count_ >= 3
             ? index >= 2
             : is_side_by_side || (grid && split_count_ == 2) || is_last_view);
    const bool leading_column =
        !is_in_split ||
        (grid ? index % 2 == 0 : !is_side_by_side || is_first_view);
    const bool trailing_column =
        !is_in_split ||
        (grid ? index % 2 == 1 || (split_count_ == 3 && index == 2)
              : !is_side_by_side || is_last_view);
    const bool is_single_contents_view =
        !is_in_split && contents_container_view->GetVisible();
    const bool should_round_contents =
        is_in_split ||
        (is_single_contents_view && should_round_single_contents);
    if (!should_round_contents) {
      contents_container_view->SetRoundedCorners(
          is_active ? GetBackgroundRadii() : gfx::RoundedCornersF());
    }
    contents_container_view->UpdateBorderAndOverlay(
        is_in_split, should_round_contents, is_active,
        is_active && active_contents_view_highlighted_,
        use_system_top_left_radius && top_row && leading_column,
        use_system_top_right_radius && top_row && trailing_column,
        use_system_bottom_left_radius && bottom_row && leading_column,
        use_system_bottom_right_radius && bottom_row && trailing_column);
  }
}

MultiContentsViewDropTargetController&
MultiContentsView::drop_target_controller() const {
  CHECK(IsDragAndDropEnabled());
  return *drop_target_controller_;
}

bool MultiContentsView::IsDragAndDropEnabled() const {
  // Split view drag and drop is only supported on normal browser types.
  if (!browser_view_->GetIsNormalType() || !is_drag_drop_pref_enabled_) {
    return false;
  }

  const auto* active_contents_view = GetActiveContentsView();
  if (!active_contents_view) {
    return true;
  }

  const auto* web_contents = active_contents_view->web_contents();
  return !web_contents ||
         web_contents->GetLastCommittedURL() ==
             chrome::ChromeUINewTabURLAsGURL() ||
         web_contents->GetLastCommittedURL() ==
             chrome::ChromeUINewTabPageURLAsGURL() ||
         !web_contents->GetLastCommittedURL().SchemeIs(
             content::kChromeUIScheme);
}

void MultiContentsView::OnDragAndDropPrefStateChange() {
  is_drag_drop_pref_enabled_ =
      browser_view_->GetProfile()->GetPrefs()->GetBoolean(
          prefs::kSplitViewDragAndDropEnabled);
  InvalidateLayout();
}

void MultiContentsView::OnRoundedFramePrefStateChange() {
  is_rounded_frame_pref_enabled_ =
      browser_view_->GetProfile()->GetPrefs()->GetBoolean(
          prefs::kHeliumRoundedFrame);
  UpdateContentsBorderAndOverlay();
  InvalidateLayout();
  browser_view_->InvalidateLayout();
}

void MultiContentsView::SetShouldShowTopSeparator(bool should_show) {
  SetTopChromeAttachmentState(top_edge_attached_to_infobar_, should_show);
}

void MultiContentsView::SetTopChromeAttachmentState(
    bool top_edge_attached_to_infobar,
    bool should_show_top_separator) {
  if (top_edge_attached_to_infobar_ == top_edge_attached_to_infobar &&
      contents_separators_.should_show_top == should_show_top_separator) {
    return;
  }
  top_edge_attached_to_infobar_ = top_edge_attached_to_infobar;
  contents_separators_.should_show_top = should_show_top_separator;
  MaybeUpdateContentsBorderAndOverlay();
  // This can be called during BrowserView layout, so protect against creating a
  // layout loop.
  InvalidateLayout(/*avoid_propagate_during_layout=*/true);
}

void MultiContentsView::SetAttachedSideChromeVisibility(
    bool leading_attached,
    bool trailing_attached) {
  if (contents_separators_.leading_side_chrome_attached == leading_attached &&
      contents_separators_.trailing_side_chrome_attached == trailing_attached) {
    return;
  }
  contents_separators_.leading_side_chrome_attached = leading_attached;
  contents_separators_.trailing_side_chrome_attached = trailing_attached;
  MaybeUpdateContentsBorderAndOverlay();
  InvalidateLayout(/*avoid_propagate_during_layout=*/true);
}

void MultiContentsView::SetSplitViewInsets(const gfx::Insets& insets) {
  if (split_view_insets_ == insets) {
    return;
  }
  split_view_insets_ = insets;
  if (IsInSplitView() || ShouldUseRoundedFrame()) {
    UpdateContentsBorderAndOverlay();
  }
  // This can be called during BrowserView layout, so protect against creating a
  // layout loop.
  InvalidateLayout(/*avoid_propagate_during_layout=*/true);
}

int MultiContentsView::GetResizeAxisComponent(const gfx::Size& size) const {
  return (visual_data_.split_layout() != split_tabs::SplitTabLayout::kStacked)
             ? size.width()
             : size.height();
}

BEGIN_METADATA(MultiContentsView)
END_METADATA

void MultiContentsView::OnPaneResize(int boundary, int amount, bool done) {
  if (GetSplitLayout() == split_tabs::SplitTabLayout::kGrid &&
      split_count_ >= 3) {
    OnGridResize(boundary, amount, done);
    return;
  }
  if (split_count_ <= 2) { OnResize(amount, done); return; }
  if (boundary < 0 || boundary + 1 >= split_count_) return;
  if (!initial_pane_weights_) initial_pane_weights_ = pane_weights_;
  const int length = GetSplitLayout() == split_tabs::SplitTabLayout::kSideBySide ? width() : height();
  const double usable = std::max(1, length - 8 * (split_count_ - 1));
  const auto& initial = *initial_pane_weights_;
  const double pair = initial[boundary] + initial[boundary + 1];
  const double minimum = std::min(64.0 / usable, pair / 2.0);
  pane_weights_[boundary] = std::clamp(initial[boundary] + amount / usable, minimum, pair - minimum);
  pane_weights_[boundary + 1] = pair - pane_weights_[boundary];
  InvalidateLayout();
  if (done) initial_pane_weights_.reset();
}

void MultiContentsView::UpdateResizeAreas() {
  const bool grid = GetSplitLayout() == split_tabs::SplitTabLayout::kGrid;
  resize_area_->SetLayout(GetSplitLayout());
  resize_area_->SetVisible(split_count_ >= 2);
  for (size_t i = 0; i < extra_resize_areas_.size(); ++i) {
    extra_resize_areas_[i]->SetLayout(GetSplitLayout());
    extra_resize_areas_[i]->SetVisible(grid ? i == 0 && split_count_ >= 3
                                            : static_cast<int>(i) + 2 <
                                                  split_count_);
  }
}

double MultiContentsView::GetPaneResizeRatio(int boundary) const {
  if (GetSplitLayout() == split_tabs::SplitTabLayout::kGrid) {
    return boundary == 1 ? visual_data_.grid_row_ratio()
                         : visual_data_.split_ratio();
  }
  if (split_count_ > 2 && boundary >= 0 && boundary + 1 < split_count_) {
    const double pair = pane_weights_[boundary] + pane_weights_[boundary + 1];
    return pair > 0 ? pane_weights_[boundary] / pair : 0.5;
  }
  return visual_data_.split_ratio();
}

void MultiContentsView::OnGridResize(int boundary, int amount, bool done) {
  if (boundary < 0 || boundary > 1) {
    return;
  }
  auto* model = browser_view_->browser()->GetTabStripModel();
  auto* active = model->GetActiveTab();
  if (!active || !active->IsSplit()) {
    initial_grid_resize_size_.reset();
    initial_grid_resize_split_.reset();
    return;
  }
  const auto split = active->GetSplit().value();
  if (initial_grid_resize_split_ && *initial_grid_resize_split_ != split) {
    initial_grid_resize_size_.reset();
    initial_grid_resize_split_.reset();
    return;
  }
  initial_grid_resize_split_ = split;
  const bool rows = boundary == 1;
  const int start = rows ? contents_container_views_[0]->height()
                         : contents_container_views_[0]->width();
  const int end = rows ? contents_container_views_[2]->height()
                       : contents_container_views_[1]->width();
  const int total = start + end;
  if (total <= 0) {
    return;
  }
  if (!initial_grid_resize_size_) {
    initial_grid_resize_size_ = start;
  }
  const int minimum = std::min(64, total / 2);
  const double size =
      std::clamp(*initial_grid_resize_size_ + amount, minimum, total - minimum);
  const double ratio = std::clamp(CalculateRatioWithSnapPoints(size, total),
                                  static_cast<double>(minimum) / total,
                                  1.0 - static_cast<double>(minimum) / total);
  // Notifications may synchronously update layout. Do not retain tab pointers
  // across the model call, and finish the capture bookkeeping before it.
  if (done) {
    initial_grid_resize_size_.reset();
    initial_grid_resize_split_.reset();
  }
  if (rows) {
    model->UpdateSplitGridRowRatio(split, ratio, !done);
  } else {
    model->UpdateSplitRatio(split, ratio, !done);
  }
  // A release often repeats the last intermediate ratio. The model then emits
  // no change notification, so explicitly persist the completed drag, as the
  // two-pane resize delegate does.
  if (done && model->ContainsSplit(split)) {
    auto* browser = browser_view_->browser();
    if (auto* service =
            SessionServiceFactory::GetForProfile(browser->GetProfile())) {
      service->SetSplitTabData(browser->GetSessionID(), split,
                               model->GetSplitData(split)->visual_data());
    }
  }
}
