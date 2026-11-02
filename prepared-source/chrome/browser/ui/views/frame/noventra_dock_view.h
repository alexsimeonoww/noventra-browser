// Copyright 2026 The Noventra Authors
// SPDX-License-Identifier: GPL-3.0-only
#ifndef CHROME_BROWSER_UI_VIEWS_FRAME_NOVENTRA_DOCK_VIEW_H_
#define CHROME_BROWSER_UI_VIEWS_FRAME_NOVENTRA_DOCK_VIEW_H_
#include <memory>
#include <set>
#include <vector>

#include "base/memory/raw_ptr.h"
#include "chrome/browser/ui/noventra/layout_model.h"
#include "ui/base/class_property.h"
#include "ui/views/accessible_pane_view.h"
extern const ui::ClassProperty<int>* const kNoventraDockKind;
extern const ui::ClassProperty<bool>* const kNoventraDockCompact;
extern const ui::ClassProperty<bool>* const kNoventraDockHorizontal;
extern const ui::ClassProperty<bool>* const kNoventraDockOverflowable;
extern const ui::ClassProperty<bool>* const kNoventraDockCompactOverflow;
extern const ui::ClassProperty<bool>* const kNoventraDockSharedFooter;
extern const ui::ClassProperty<bool>* const kNoventraDockUnifiedAddress;
extern const ui::ClassProperty<bool>* const kNoventraDockBookmarkPanel;
extern const ui::ClassProperty<bool>* const kNoventraDockResizeActive;
#include "ui/base/metadata/metadata_header_macros.h"
#include "ui/gfx/geometry/insets.h"
#include "ui/views/view_targeter_delegate.h"

namespace ui {
class LayerSolidColor;
class SimpleMenuModel;
}

class NoventraDockView : public views::AccessiblePaneView,
                         public views::ViewTargeterDelegate {
  METADATA_HEADER(NoventraDockView, views::AccessiblePaneView)
 public:
  explicit NoventraDockView(noventra::DockEdge edge);
  ~NoventraDockView() override;
  noventra::DockEdge edge() const { return edge_; }
  void Layout(PassKey) override;
  void OnThemeChanged() override;
  void UpdateResizeEdge();
  void OnBoundsChanged(const gfx::Rect& previous_bounds) override;
  void SetFloatingGlass(bool floating);
  void SetPaintGlassMaterial(bool paint);
  bool paint_glass_material() const { return paint_glass_material_; }
  std::vector<gfx::Rect> GetSharedNewTabGlassBounds() const;
  void SetCommandPopup(bool enabled);
  bool command_popup() const { return command_popup_; }
  bool HasVisibleControls() const;
  void PrepareForLayout(int available_height);
  // Undo presentation-only hiding/reordering before applying a saved layout.
  void RestoreCompactPresentation();
  bool compact_overflow() const { return !normal_order_.empty(); }
  views::View* CompactMenuAnchor() const;
  std::unique_ptr<ui::SimpleMenuModel> CompactSpacesMenu() const;
  int CrossAxisSize(int available_width, int available_height) const;
  bool TraverseUsingUpDownKeys() override;
  void SetCaptionExclusion(int leading_width, int trailing_width, int height);
  gfx::Insets CaptionInsetsForRow(int y) const;
  void SetVerticalContentInset(int inset);
  int vertical_content_inset() const { return vertical_content_inset_; }
  void SetCaptionClip(gfx::Rect leading, gfx::Rect trailing);
  views::View* TargetForRect(views::View* root, const gfx::Rect& rect) override;
  bool DoesIntersectRect(const views::View* target,
                         const gfx::Rect& rect) const override;
 private:
  const noventra::DockEdge edge_;
  bool compact_ = false;
  bool compact_tools_ = false;
  std::vector<raw_ptr<views::View>> normal_order_;
  std::set<views::View*> suppressed_controls_;
  bool floating_glass_ = false;
  bool command_popup_ = false;
  bool paint_glass_material_ = true;
  bool background_blur_enabled_ = false;
  gfx::Rect background_blur_bounds_;
  std::vector<gfx::Rect> shared_background_bounds_;
  void UpdateGlassBlur();
  int leading_caption_width_ = 0;
  int trailing_caption_width_ = 0;
  int caption_height_ = 0;
  int vertical_content_inset_ = 0;
  gfx::Rect leading_caption_clip_;
  gfx::Rect trailing_caption_clip_;
  std::unique_ptr<ui::LayerSolidColor> resize_edge_layer_;
};
#endif
