// Copyright 2025 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/ui/views/frame/multi_contents_resize_area.h"

#include "base/i18n/number_formatting.h"
#include "base/i18n/rtl.h"
#include "base/numerics/safe_conversions.h"
#include "chrome/browser/ui/color/chrome_color_id.h"
#include "chrome/browser/ui/tabs/split_tab_menu_model.h"
#include "chrome/browser/ui/tabs/tab_strip_model.h"
#include "chrome/browser/ui/views/frame/multi_contents_view.h"
#include "chrome/grit/generated_resources.h"
#include "components/split_tabs/split_tab_visual_data.h"
#include "ui/base/l10n/l10n_util.h"
#include "ui/base/metadata/metadata_impl_macros.h"
#include "ui/compositor/layer.h"
#include "ui/compositor/layer_type.h"
#include "ui/gfx/geometry/rect.h"
#include "ui/gfx/geometry/size.h"
#include "ui/views/accessibility/view_accessibility.h"
#include "ui/views/background.h"
#include "ui/views/controls/focus_ring.h"
#include "ui/views/controls/highlight_path_generator.h"
#include "ui/views/controls/menu/menu_item_view.h"
#include "ui/views/controls/menu/menu_model_adapter.h"
#include "ui/views/controls/menu/menu_runner.h"
#include "ui/views/layout/flex_layout.h"

namespace {
constexpr int kHandleCornerRadius = 2;
constexpr int kHandleOffAxisSize = 24;
constexpr int kResizeIncrement = 50;
}  // namespace

DEFINE_CLASS_ELEMENT_IDENTIFIER_VALUE(MultiContentsResizeHandle,
                                      kMultiContentsResizeHandleElementId);

DEFINE_CLASS_ELEMENT_IDENTIFIER_VALUE(MultiContentsResizeArea,
                                      kMultiContentsResizeAreaElementId);

MultiContentsResizeHandle::MultiContentsResizeHandle() {
  // Cannot use `SOLID_COLOR_LAYER`, since resize handle has rounded corners,
  // and applying layer based rounded corners will clip the focus ring.
  SetPaintToLayer(ui::LAYER_TEXTURED);
  layer()->SetFillsBoundsOpaquely(false);

  SetCanProcessEventsWithinSubtree(false);
  SetFocusBehavior(FocusBehavior::ALWAYS);
  views::FocusRing::Install(this);
  GetViewAccessibility().SetRole(ax::mojom::Role::kSlider);
  GetViewAccessibility().SetName(
      l10n_util::GetStringUTF16(IDS_ACCNAME_SPLIT_TABS_RESIZE));
  SetProperty(views::kElementIdentifierKey,
              kMultiContentsResizeHandleElementId);

  SetBackground(views::CreateRoundedRectBackground(
      kColorSidePanelHoverResizeAreaHandle, kHandleCornerRadius));
}

void MultiContentsResizeHandle::UpdateVisibility() {
  layer()->SetVisible(parent()->GetVisible() &&
                      (HasFocus() || parent()->IsMouseHovered()));
}

void MultiContentsResizeHandle::AddedToWidget() {
  GetFocusManager()->AddFocusChangeListener(this);
}

void MultiContentsResizeHandle::RemovedFromWidget() {
  GetFocusManager()->RemoveFocusChangeListener(this);
}

void MultiContentsResizeHandle::OnDidChangeFocus(views::View* before,
                                                 views::View* now) {
  UpdateVisibility();
}

BEGIN_METADATA(MultiContentsResizeHandle)
END_METADATA

MultiContentsResizeArea::MultiContentsResizeArea(
    MultiContentsView* multi_contents_view,
    TabStripModel* tab_strip_model, int boundary)
    : ResizeArea(this),
      boundary_(boundary),
      multi_contents_view_(multi_contents_view),
      split_tab_menu_(std::make_unique<SplitTabMenuModel>(
          tab_strip_model,
          SplitTabMenuModel::MenuSource::kToolbarButton)) {
  set_context_menu_controller(this);
  flex_layout_manager_ =
      SetLayoutManager(std::make_unique<views::FlexLayout>());
  flex_layout_manager_->SetMainAxisAlignment(views::LayoutAlignment::kCenter)
      .SetCrossAxisAlignment(views::LayoutAlignment::kCenter);

  resize_handle_ = AddChildView(std::make_unique<MultiContentsResizeHandle>());
  resize_handle_->set_context_menu_controller(this);

  SetProperty(views::kElementIdentifierKey, kMultiContentsResizeAreaElementId);
  OnLayoutUpdated();
  tab_strip_model->AddObserver(this);
}

MultiContentsResizeArea::~MultiContentsResizeArea() {
  TabStripModelObserver::StopObservingAll(this);
}

void MultiContentsResizeArea::OnTabStripModelChanged(
    TabStripModel*,
    const TabStripModelChange&,
    const TabStripSelectionChange& selection) {
  if (selection.active_tab_changed()) {
    CancelMenu();
  }
}

void MultiContentsResizeArea::OnSplitTabChanged(const SplitTabChange&) {
  CancelMenu();
}

void MultiContentsResizeArea::OnResize(int amount, bool done) {
  multi_contents_view_->OnPaneResize(boundary_, amount, done);
}

void MultiContentsResizeArea::SetLayout(split_tabs::SplitTabLayout layout) {
  Axis old_axis = axis();
  const bool horizontal =
      layout == split_tabs::SplitTabLayout::kSideBySide ||
      (layout == split_tabs::SplitTabLayout::kGrid && boundary_ == 0);
  set_axis(horizontal ? Axis::kHorizontal : Axis::kVertical);
  if (axis() != old_axis) {
    OnLayoutUpdated();
  }
}

void MultiContentsResizeArea::OnGestureEvent(ui::GestureEvent* event) {
  // If the gesture event was a double tap and was not part of a resizing event,
  // swap the contents views.
  if (!is_resizing() && event->type() == ui::EventType::kGestureTap &&
      event->details().tap_count() == 2) {
    multi_contents_view_->OnSwap();
  }
  ResizeArea::OnGestureEvent(event);
}

void MultiContentsResizeArea::OnMouseReleased(const ui::MouseEvent& event) {
  // If the mouse event was a left double click and was not part of a resizing
  // event, swap the contents views.
  if (!is_resizing() && event.IsOnlyLeftMouseButton() &&
      event.GetClickCount() == 2) {
    multi_contents_view_->OnSwap();
  }
  ResizeArea::OnMouseReleased(event);
}

bool MultiContentsResizeArea::OnKeyPressed(const ui::KeyEvent& event) {
  int resize_amount = 0;
  if (event.key_code() == DecreaseContentsSizeKeyCode()) {
    resize_amount = -kResizeIncrement;
  } else if (event.key_code() == IncreaseContentsSizeKeyCode()) {
    resize_amount = kResizeIncrement;
  }

  if (resize_amount == 0) {
    return false;
  }

  OnResize(resize_amount, true);

  if ((axis() == Axis::kHorizontal ? multi_contents_view_->width()
                                   : multi_contents_view_->height()) > 0) {
    const int start_percent = base::ClampRound(
        multi_contents_view_->GetPaneResizeRatio(boundary_) * 100);
    const int end_percent = 100 - start_percent;

    auto [start_label_id, end_label_id] = GetAccessibleAlertStringIds();
    const int alert_string_id =
        axis() == Axis::kHorizontal
            ? IDS_SPLIT_VIEW_RESIZE_ACCESSIBLE_ALERT
            : IDS_SPLIT_VIEW_RESIZE_ACCESSIBLE_ALERT_STACKED;
    GetViewAccessibility().AnnounceText(l10n_util::GetStringFUTF16(
        alert_string_id, l10n_util::GetStringUTF16(start_label_id),
        base::FormatPercent(start_percent),
        l10n_util::GetStringUTF16(end_label_id),
        base::FormatPercent(end_percent)));
  }
  return true;
}

void MultiContentsResizeArea::OnMouseMoved(const ui::MouseEvent& event) {
  resize_handle_->UpdateVisibility();
}

void MultiContentsResizeArea::OnMouseExited(const ui::MouseEvent& event) {
  resize_handle_->UpdateVisibility();
}

void MultiContentsResizeArea::SetVisible(bool visible) {
  if (!visible) {
    CancelMenu();
  }
  views::View::SetVisible(visible);
  resize_handle_->UpdateVisibility();
}

void MultiContentsResizeArea::ShowContextMenuForViewImpl(
    views::View* source,
    const gfx::Point& point,
    ui::mojom::MenuSourceType source_type) {
  if (menu_runner_ && menu_runner_->IsRunning()) {
    return;
  }
  menu_runner_.reset();
  menu_model_adapter_ =
      std::make_unique<views::MenuModelAdapter>(split_tab_menu_.get());
  menu_runner_ = std::make_unique<views::MenuRunner>(
      menu_model_adapter_->CreateMenu(),
      views::MenuRunner::HAS_MNEMONICS | views::MenuRunner::CONTEXT_MENU);
  menu_runner_->RunMenuAt(source->GetWidget(), nullptr,
                          gfx::Rect(point, gfx::Size()),
                          views::MenuAnchorPosition::kTopLeft, source_type);
}

void MultiContentsResizeArea::CancelMenu() {
  if (menu_runner_ && menu_runner_->IsRunning()) {
    menu_runner_->Cancel();
  }
}

void MultiContentsResizeArea::OnLayoutUpdated() {
  flex_layout_manager_->SetOrientation(
      axis() == Axis::kHorizontal ? views::LayoutOrientation::kVertical
                                  : views::LayoutOrientation::kHorizontal);

  gfx::Size preferred_size(kHandleResizeAxisSize + kHandleResizeAxisPadding,
                           kHandleOffAxisSize);
  gfx::Size resize_handle_preferred_size(kHandleResizeAxisSize,
                                         kHandleOffAxisSize);
  if (axis() == Axis::kVertical) {
    preferred_size.Transpose();
    resize_handle_preferred_size.Transpose();
  }
  SetPreferredSize(preferred_size);
  resize_handle_->SetPreferredSize(resize_handle_preferred_size);
}

ui::KeyboardCode MultiContentsResizeArea::DecreaseContentsSizeKeyCode() {
  return axis() == Axis::kHorizontal
             ? (base::i18n::IsRTL() ? ui::VKEY_RIGHT : ui::VKEY_LEFT)
             : ui::VKEY_UP;
}

ui::KeyboardCode MultiContentsResizeArea::IncreaseContentsSizeKeyCode() {
  return axis() == Axis::kHorizontal
             ? (base::i18n::IsRTL() ? ui::VKEY_LEFT : ui::VKEY_RIGHT)
             : ui::VKEY_DOWN;
}

std::pair<int, int> MultiContentsResizeArea::GetAccessibleAlertStringIds() {
  return axis() == Axis::kHorizontal
             ? (base::i18n::IsRTL()
                    ? std::
                          pair{IDS_SPLIT_VIEW_RESIZE_RIGHT_SIDE_ACCESSIBLE_ALERT,
                               IDS_SPLIT_VIEW_RESIZE_LEFT_SIDE_ACCESSIBLE_ALERT}
                    : std::
                          pair{IDS_SPLIT_VIEW_RESIZE_LEFT_SIDE_ACCESSIBLE_ALERT,
                               IDS_SPLIT_VIEW_RESIZE_RIGHT_SIDE_ACCESSIBLE_ALERT})
             : std::pair{IDS_SPLIT_VIEW_RESIZE_TOP_SIDE_ACCESSIBLE_ALERT,
                         IDS_SPLIT_VIEW_RESIZE_BOTTOM_SIDE_ACCESSIBLE_ALERT};
}

BEGIN_METADATA(MultiContentsResizeArea)
END_METADATA
