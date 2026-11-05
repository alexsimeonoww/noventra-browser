// Copyright 2025 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_UI_VIEWS_FRAME_MULTI_CONTENTS_RESIZE_AREA_H_
#define CHROME_BROWSER_UI_VIEWS_FRAME_MULTI_CONTENTS_RESIZE_AREA_H_

#include <memory>

#include "base/memory/raw_ptr.h"
#include "chrome/browser/ui/tabs/tab_strip_model_observer.h"
#include "components/split_tabs/split_tab_visual_data.h"
#include "ui/base/interaction/element_identifier.h"
#include "ui/events/keycodes/keyboard_codes.h"
#include "ui/views/context_menu_controller.h"
#include "ui/views/controls/resize_area.h"
#include "ui/views/controls/resize_area_delegate.h"
#include "ui/views/focus/focus_manager.h"
#include "ui/views/view.h"

class MultiContentsView;
class SplitTabMenuModel;
class TabStripModel;

namespace split_tabs {
enum class SplitTabLayout;
}

namespace views {
class FlexLayout;
class MenuModelAdapter;
class MenuRunner;
}

// Keyboard-accessible drag handle icon intended to be drawn on top of a
// MultiContentsResizeArea.
class MultiContentsResizeHandle : public views::View,
                                  public views::FocusChangeListener {
  METADATA_HEADER(MultiContentsResizeHandle, views::View)

 public:
  DECLARE_CLASS_ELEMENT_IDENTIFIER_VALUE(kMultiContentsResizeHandleElementId);

  MultiContentsResizeHandle();

  void UpdateVisibility();

  // views::View:
  void AddedToWidget() override;
  void RemovedFromWidget() override;

  // FocusChangeListener:
  void OnDidChangeFocus(views::View* before, views::View* now) override;
};

// ResizeArea meant to draw in between WebContents within a MultiContentsView,
// and responsiveness to key events via a focusable MultiContentsResizeHandle.
class MultiContentsResizeArea : public views::ResizeArea,
                                public views::ContextMenuController,
                                public views::ResizeAreaDelegate,
                                public TabStripModelObserver {
  METADATA_HEADER(MultiContentsResizeArea, ResizeArea)

 public:
  static constexpr int kHandleResizeAxisPadding = 4;
  static constexpr int kHandleResizeAxisSize = 2;

  DECLARE_CLASS_ELEMENT_IDENTIFIER_VALUE(kMultiContentsResizeAreaElementId);

  MultiContentsResizeArea(MultiContentsView* multi_contents_view,
                          TabStripModel* tab_strip_model, int boundary = 0);
  void OnResize(int amount, bool done) override;
  ~MultiContentsResizeArea() override;

  void SetLayout(split_tabs::SplitTabLayout layout);

  // TabStripModelObserver:
  void OnTabStripModelChanged(
      TabStripModel* tab_strip_model,
      const TabStripModelChange& change,
      const TabStripSelectionChange& selection) override;
  void OnSplitTabChanged(const SplitTabChange& change) override;

  // views::ResizeArea:
  void OnGestureEvent(ui::GestureEvent* event) override;
  void OnMouseReleased(const ui::MouseEvent& event) override;
  bool OnKeyPressed(const ui::KeyEvent& event) override;
  void OnMouseMoved(const ui::MouseEvent& event) override;
  void OnMouseExited(const ui::MouseEvent& event) override;
  void SetVisible(bool visible) override;

  // views::ContextMenuController:
  void ShowContextMenuForViewImpl(
      views::View* source,
      const gfx::Point& point,
      ui::mojom::MenuSourceType source_type) override;

 private:
  void CancelMenu();

  // Sets the layout manager orientation and preferred size according to the
  // split view layout.
  void OnLayoutUpdated();

  ui::KeyboardCode DecreaseContentsSizeKeyCode();
  ui::KeyboardCode IncreaseContentsSizeKeyCode();

  std::pair<int, int> GetAccessibleAlertStringIds();

  int boundary_ = 0;
  raw_ptr<MultiContentsView> multi_contents_view_;
  raw_ptr<MultiContentsResizeHandle> resize_handle_;

  raw_ptr<views::FlexLayout> flex_layout_manager_;

  std::unique_ptr<SplitTabMenuModel> split_tab_menu_;
  std::unique_ptr<views::MenuModelAdapter> menu_model_adapter_;
  std::unique_ptr<views::MenuRunner> menu_runner_;
};

#endif  // CHROME_BROWSER_UI_VIEWS_FRAME_MULTI_CONTENTS_RESIZE_AREA_H_
