// Copyright 2025 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/ui/views/tabs/common/split_tab_view.h"

#include <vector>
#include <limits>

#include "chrome/browser/ui/color/chrome_color_id.h"
#include "chrome/browser/ui/layout_constants.h"
#include "chrome/browser/ui/tabs/tab_style.h"
#include "chrome/browser/ui/views/tabs/common/tab_collection_animating_layout_manager.h"
#include "chrome/browser/ui/views/tabs/common/tab_collection_node.h"
#include "chrome/browser/ui/views/tabs/common/tab_drag_handler.h"
#include "chrome/browser/ui/views/tabs/common/tab_strip_collection_controller.h"
#include "chrome/browser/ui/views/tabs/common/tab_view.h"
#include "chrome/browser/ui/views/tabs/hovercard/tab_hover_card_controller.h"
#include "chrome/browser/ui/views/tabs/tab/glow_hover_controller.h"
#include "components/tabs/public/tab_collection.h"
#include "components/tabs/public/tab_collection_types.h"
#include "components/tabs/public/tab_interface.h"
#include "content/public/browser/navigation_controller.h"
#include "ui/base/metadata/metadata_impl_macros.h"
#include "ui/gfx/geometry/rect.h"
#include "ui/views/border.h"
#include "ui/views/layout/delegating_layout_manager.h"
#include "ui/views/layout/proposed_layout.h"
#include "ui/views/view.h"
#include "ui/views/view_class_properties.h"
#include "ui/views/view_utils.h"
#include "ui/views/widget/widget.h"

SplitTabView::SplitTabView(TabCollectionNode* collection_node)
    : collection_node_(collection_node),
      hover_controller_(gfx::Animation::ShouldRenderRichAnimation()
                            ? std::make_unique<GlowHoverController>(
                                  this,
                                  TabView::kGlowHoverAnimationDuration)
                            : nullptr) {
  SetLayoutManager(std::make_unique<views::DelegatingLayoutManager>(this));

  collection_node->set_detach_child_from_node(base::BindRepeating(
      &SplitTabView::RemoveChildViewForReparenting, base::Unretained(this)));

  node_destroyed_subscription_ =
      collection_node_->RegisterWillDestroyCallback(base::BindOnce(
          &SplitTabView::ResetCollectionNode, base::Unretained(this)));

  CHECK(collection_node_->GetController());
  auto* state_controller =
      collection_node_->GetController()->GetStateController();
  CHECK(state_controller);
  OnCollapseStateChanged(state_controller->GetCollapseState());
  collapsed_state_changed_subscription_ =
      state_controller->RegisterOnCollapseChanged(base::BindRepeating(
          &SplitTabView::OnCollapseStateChanged, base::Unretained(this)));

  // Ensures this view gets mouse events as well its children.
  SetNotifyEnterExitOnChild(true);
}

SplitTabView::~SplitTabView() = default;

void SplitTabView::OnThemeChanged() {
  views::View::OnThemeChanged();
  UpdateBorder();
}

void SplitTabView::AddedToWidget() {
  paint_as_active_subscription_ =
      GetWidget()->RegisterPaintAsActiveChangedCallback(base::BindRepeating(
          &SplitTabView::UpdateBorder, base::Unretained(this)));

  // Moving a dock can reparent a closed split still animating out. Its model
  // has already been retired, so retain the closing view's painted state.
  if (!collection_node_) {
    return;
  }

  OnDataChanged();
  UpdateHovered(IsMouseHovered());
}

void SplitTabView::RemovedFromWidget() {
  paint_as_active_subscription_ = {};
}

void SplitTabView::OnMouseEntered(const ui::MouseEvent& event) {
  UpdateHovered(true);
}

void SplitTabView::OnMouseExited(const ui::MouseEvent& event) {
  UpdateHovered(false);
}

void SplitTabView::OnMouseMoved(const ui::MouseEvent& event) {
  // Linux enter/leave events are sometimes flaky, so we don't want to "miss"
  // an enter event and fail to hover the tab.
  UpdateHovered(true);
}

void SplitTabView::OnPaint(gfx::Canvas* canvas) {
  if (!pinned_ && collection_node_ &&
      collection_node_->orientation() == TabStripOrientation::kVertical) {
    // A quiet rail identifies a split without boxing every member separately.
    cc::PaintFlags flags;
    flags.setAntiAlias(true);
    flags.setColor(GetColorProvider()->GetColor(kColorTabDividerFrameActive));
    canvas->DrawRoundRect(gfx::Rect(1, 5, 2, std::max(0, height() - 10)), 1, flags);
  }
  if (pinned_) {
    const auto children = collection_node_
                              ? collection_node_->GetDirectChildren()
                              : TabCollectionNode::ChildViews();
    std::optional<SkColor> background_color =
        !children.empty()
            ? views::AsViewClass<TabView>(children[0])->GetBackgroundColor()
            : std::nullopt;
    if (background_color.has_value()) {
      cc::PaintFlags flags;
      flags.setAntiAlias(true);
      flags.setColor(background_color.value());
      const float corner_radius =
          GetLayoutConstant(LayoutConstant::kVerticalTabCornerRadius) -
          GetInsets().top() / 2.0;
      canvas->DrawRoundRect(GetContentsBounds(), corner_radius, flags);
    }
  }

  views::View::OnPaint(canvas);
}

views::ProposedLayout SplitTabView::CalculateProposedLayout(
    const views::SizeBounds& size_bounds) const {
  if (collection_node_ &&
      collection_node_->orientation() == TabStripOrientation::kHorizontal) {
    return CalculateHorizontalLayout(size_bounds);
  }
  return CalculateVerticalLayout(size_bounds);
}

gfx::Size SplitTabView::GetMinimumSize() const {
  if (collection_node_ &&
      collection_node_->orientation() == TabStripOrientation::kHorizontal) {
    int min_width = 0;
    for (views::View* child : children()) {
      min_width += child->GetMinimumSize().width();
    }
    const int tab_overlap = TabStyle::Get()->GetTabOverlap();
    return gfx::Size(std::max(0, min_width - tab_overlap),
                     TabStyle::Get()->GetStandardHeight());
  }
  return views::View::GetMinimumSize();
}

std::optional<BrowserRootView::DropIndex> SplitTabView::GetLinkDropIndex(
    const gfx::Point& loc_in_view) {
  if (!collection_node_ || !collection_node_->GetController()) {
    return std::nullopt;
  }

  const TabCollectionNode* closest = nullptr;
  int distance = std::numeric_limits<int>::max();
  for (const auto& child : collection_node_->children()) {
    const auto point = views::View::ConvertPointToTarget(child->view(), this, child->view()->GetLocalBounds().CenterPoint());
    const int candidate = (point - loc_in_view).LengthSquared();
    if (candidate < distance) { closest = child.get(); distance = candidate; }
  }
  return closest ? collection_node_->GetController()->GetDragHandler().GetLinkDropIndexForNode(*closest, std::nullopt) : std::nullopt;

}

double SplitTabView::GetHoverAnimationValue() const {
  if (!hover_controller_) {
    return hovered_ ? 1.0 : 0.0;
  }
  return hover_controller_->GetAnimationValue();
}

void SplitTabView::ResetCollectionNode() {
  CHECK(collection_node_);
  TabHoverCardController* hover_card_controller =
      collection_node_->GetController()->GetHoverCardController();
  if (hover_card_controller) {
    hover_card_controller->UpdateHoverCard(
        nullptr, TabSlotController::HoverCardUpdateType::kTabRemoved);
  }

  node_destroyed_subscription_ = {};
  collapsed_state_changed_subscription_ = {};
  collection_node_ = nullptr;
}

void SplitTabView::OnDataChanged() {
  const tabs::TabCollection* tab_collection =
      std::get<tabs::ConstDanglingUntriagedTabCollection>(
          collection_node_->GetNodeData());
  const std::vector<tabs::TabInterface*> tabs =
      tab_collection->GetTabsRecursive();
  pinned_ = tabs[0]->IsPinned();

  UpdateBorder();
}

void SplitTabView::UpdateBorder() {
  if (pinned_) {
    const bool is_frame_active =
        GetWidget() ? GetWidget()->ShouldPaintAsActive() : true;
    SetBorder(views::CreateRoundedRectBorder(
        GetLayoutConstant(LayoutConstant::kVerticalTabPinnedBorderThickness),
        GetLayoutConstant(LayoutConstant::kVerticalTabCornerRadius),
        is_frame_active ? kColorTabDividerFrameActive
                        : kColorTabDividerFrameInactive));
  } else if (GetBorder()) {
    SetBorder(nullptr);
  }
}

void SplitTabView::UpdateHovered(bool hovered) {
  if (hovered_ == hovered) {
    return;
  }

  hovered_ = hovered;

  float radial_highlight_opacity = 1.0f;
  for (views::View* child : children()) {
    if (auto* tab_view = views::AsViewClass<TabView>(child)) {
      tab_view->UpdateHovered(hovered_);
      radial_highlight_opacity = tab_view->radial_highlight_opacity();
    }
  }

  if (hover_controller_) {
    if (hovered_) {
      hover_controller_->SetSubtleOpacityScale(radial_highlight_opacity);
      hover_controller_->Show(TabStyle::ShowHoverStyle::kSubtle);
    } else {
      hover_controller_->Hide(TabStyle::HideHoverStyle::kGradual);
    }
  }

  SchedulePaint();
}

views::ProposedLayout SplitTabView::CalculateHorizontalLayout(
    const views::SizeBounds& size_bounds) const {
  views::ProposedLayout layouts;
  const auto children = collection_node_ ? collection_node_->GetDirectChildren()
                                         : TabCollectionNode::ChildViews();
  if (children.size() < 2 || children.size() > 4) {
    layouts.host_size = gfx::Size(0, 0);
    return layouts;
  }

  const int height = TabStyle::Get()->GetStandardHeight();
  const int tab_overlap = TabStyle::Get()->GetTabOverlap();

  // Layout children horizontally side-by-side in order.
  int x = 0;

  for (size_t i = 0; i < children.size(); ++i) {
    views::View* child = children[i];
    gfx::Rect bounds = gfx::Rect(child->GetPreferredSize());
    bounds.set_x(x);
    bounds.set_height(height);

    // Fill available width evenly if bounded.
    if (size_bounds.width().is_bounded()) {
      const int available_width = size_bounds.width().value() + tab_overlap * (children.size() - 1);
      bounds.set_width(available_width / children.size());
    }

    if (i < children.size() - 1) {
      x += bounds.width() - tab_overlap;
    } else {
      x += bounds.width();
    }
    layouts.child_layouts.emplace_back(child, child->GetVisible(), bounds);
  }

  layouts.host_size = gfx::Size(x, height);
  return layouts;
}

views::ProposedLayout SplitTabView::CalculateVerticalLayout(
    const views::SizeBounds& size_bounds) const {
  views::ProposedLayout layouts;
  int width = 0;
  int height = 0;

  const auto children = collection_node_ ? collection_node_->GetDirectChildren()
                                         : TabCollectionNode::ChildViews();
  if (children.size() < 2 || children.size() > 4) {
    layouts.host_size = gfx::Size(0, 0);
    return layouts;
  }


  int y = 2;
  for (auto* child : children) {
    const int child_height = child->GetPreferredSize().height();
    width = size_bounds.width().is_bounded() ? size_bounds.width().value() : child->GetPreferredSize().width();
    layouts.child_layouts.emplace_back(child, child->GetVisible(), gfx::Rect(5, y, std::max(0, width - 5), child_height));
    y += child_height;
  }
  height = y + 2;
  layouts.host_size = gfx::Size(width, height);
  return layouts;
}

void SplitTabView::OnCollapseStateChanged(
    tabs::VerticalTabStripCollapseState state) {
  collapsed_ = state == tabs::VerticalTabStripCollapseState::kCollapsed;
}

std::unique_ptr<views::View> SplitTabView::RemoveChildViewForReparenting(
    views::View* child_view) {
  DCHECK(std::ranges::contains(children(), child_view));
  CHECK(collection_node_);

  auto children = collection_node_->GetDirectChildren();
  auto source_layout_info = std::make_unique<
      TabCollectionAnimatingLayoutManager::SourceLayoutInfo>(
      TabCollectionAnimatingLayoutManager::SourceLayoutInfo{
          .animation_axis =
              TabCollectionAnimatingLayoutManager::AnimationAxis::kHorizontal,
          // Note: Tabs are removed from the split view collection from the
          // front first so it is necessary to test the number of children
          // in the collection when computing the animation direction.
          .animation_direction =
              (children.size() == 2 && children[0] == child_view)
                  ? TabCollectionAnimatingLayoutManager::AnimationDirection::
                        kEndToStart
                  : TabCollectionAnimatingLayoutManager::AnimationDirection::
                        kStartToEnd,
      });

  // Ensure we remove the child view before setting source layout info to
  // prevent the manager from clearing the metadata.
  auto removed_child_view = RemoveChildViewT(child_view);
  TabCollectionAnimatingLayoutManager::SetSourceLayoutInfo(
      child_view, std::move(source_layout_info));

  return removed_child_view;
}

BEGIN_METADATA(SplitTabView)
END_METADATA
