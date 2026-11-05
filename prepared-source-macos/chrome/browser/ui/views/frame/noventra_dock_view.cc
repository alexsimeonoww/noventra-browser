// Copyright 2026 The Noventra Authors
// SPDX-License-Identifier: GPL-3.0-only
#include "chrome/browser/ui/views/frame/noventra_dock_view.h"

#include <algorithm>

#include "base/functional/bind.h"
#include "base/logging.h"
#include "chrome/browser/ui/color/chrome_color_id.h"
#include "chrome/browser/ui/layout_constants.h"
#include "chrome/browser/ui/view_ids.h"
#include "chrome/browser/ui/views/extensions/extensions_toolbar_desktop.h"
#include "chrome/browser/ui/views/frame/browser_view.h"
#include "chrome/browser/ui/views/frame/contents_web_view.h"
#include "chrome/browser/ui/views/frame/multi_contents_view.h"
#include "chrome/browser/ui/views/frame/noventra_bookmarks_view.h"
#include "chrome/browser/ui/views/frame/vertical_tab_strip_region_view.h"
#include "chrome/browser/ui/views/helium/native_frame_materials.h"
#include "chrome/browser/ui/views/location_bar/location_bar_view.h"
#include "chrome/browser/ui/views/toolbar/toolbar_button.h"
#include "third_party/skia/include/core/SkPathBuilder.h"
#include "third_party/skia/include/core/SkRegion.h"
#include "ui/accessibility/ax_enums.mojom.h"
#include "ui/base/metadata/metadata_impl_macros.h"
#include "ui/color/color_id.h"
#include "ui/color/color_provider.h"
#include "ui/compositor/layer.h"
#include "ui/compositor/layer_solid_color.h"
#include "ui/gfx/canvas.h"
#include "ui/gfx/geometry/rect_f.h"
#include "ui/gfx/geometry/rounded_corners_f.h"
#include "ui/gfx/geometry/skia_conversions.h"
#include "ui/gfx/scoped_canvas.h"
#include "ui/views/accessibility/view_accessibility.h"
#include "ui/views/background.h"
#include "ui/views/bubble/edge_anchor.h"
#include "ui/views/focus/focus_manager.h"
#include "ui/views/layout/animating_layout_manager.h"
#include "ui/views/layout/flex_layout.h"
#include "ui/views/view_targeter.h"
#include "ui/views/view_utils.h"
#include "ui/views/widget/widget.h"

DEFINE_UI_CLASS_PROPERTY_KEY(int, kNoventraDockKind, 0)
DEFINE_UI_CLASS_PROPERTY_KEY(bool, kNoventraDockCompact, false)
DEFINE_UI_CLASS_PROPERTY_KEY(bool, kNoventraDockHorizontal, false)
DEFINE_UI_CLASS_PROPERTY_KEY(bool, kNoventraDockOverflowable, false)
DEFINE_UI_CLASS_PROPERTY_KEY(bool, kNoventraDockCompactOverflow, false)
DEFINE_UI_CLASS_PROPERTY_KEY(bool, kNoventraDockSharedFooter, false)
DEFINE_UI_CLASS_PROPERTY_KEY(bool, kNoventraDockUnifiedAddress, false)
DEFINE_UI_CLASS_PROPERTY_KEY(bool, kNoventraDockBookmarkPanel, false)
DEFINE_UI_CLASS_PROPERTY_KEY(bool, kNoventraDockResizeActive, false)

namespace {
class DockMaterialBackground : public views::Background {
 public:
  void Paint(gfx::Canvas* canvas, views::View* view) const override {
    // The floating command field paints its own rounded, opaque surface.
    if (static_cast<NoventraDockView*>(view)->command_popup()) {
      return;
    }
    if (!helium::ShouldUseNativeFrameMaterials(view)) {
      canvas->DrawColor(view->GetColorProvider()->GetColor(kColorToolbar));
      return;
    }
    auto* dock = static_cast<NoventraDockView*>(view);
    // The trusted New Tab already paints this tint. Painting another material
    // and filtering its translucent backing compounds alpha until the floating
    // dock looks solid. Reuse only the pixels covered by the existing material.
    if (dock->paint_glass_material()) {
      gfx::ScopedCanvas scoped_canvas(canvas);
      const auto shared_bounds = dock->GetSharedNewTabGlassBounds();
      VLOG(1) << "NoventraGlass dock.paint dock=" << dock
              << " shared_count=" << shared_bounds.size()
              << " blur_sigma=" << dock->layer()->background_blur();
      for (const gfx::Rect& shared : shared_bounds) {
        canvas->ClipRect(shared, SkClipOp::kDifference);
      }
      canvas->DrawColor(helium::GetNoventraGlassTint(view));
    }
  }
};

gfx::Size DockRegionPreferredSize(views::View* child) {
  if (auto* extensions = views::AsViewClass<ExtensionsToolbarDesktop>(child)) {
    // The animation wrapper reports its previous allocated size. A region
    // revealed after pinning while hidden needs room for its target children
    // before the fade can run, including when its previous width was zero.
    auto size = extensions->GetTargetLayoutManager()->GetPreferredSize(extensions);
    // Reserve the outgoing animated extent too. Shrinking the host directly
    // to the target would cancel the native removal animation mid-flight.
    size.SetToMax(extensions->GetPreferredSize());
    return size;
  }
  return child->GetPreferredSize();
}
bool OccupiesDockSpace(views::View* child) {
  if (auto* library = views::AsViewClass<NoventraBookmarksView>(child);
      library && !library->HasDockContent()) {
    return false;
  }
  return child->GetVisible() && (child->GetProperty(kNoventraDockKind) != 4 ||
                                 DockRegionPreferredSize(child).width() > 0);
}
int MinimumHorizontalExtent(views::View* child) {
  switch (child->GetProperty(kNoventraDockKind)) {
    case 1:
      return 160;
    case 3:
      return 232;
    case 4:
      return DockRegionPreferredSize(child).width();
    default:
      return std::max(GetLayoutConstant(LayoutConstant::kToolbarButtonHeight),
                      child->GetPreferredSize().width());
  }
}
struct VerticalDockLayout {
  std::vector<std::pair<views::View*, gfx::Rect>> children;
  NoventraBookmarksView* library = nullptr;
  int library_height = 0;
  int library_gap = 0;
  views::View* spaces = nullptr;
  views::View* footer_menu = nullptr;
  views::View* footer_bookmarks = nullptr;
  bool horizontal_footer = false;
  int footer_height = 0;
  int tabs = 0;
  int height = 0;
};

bool SidebarAddressInTabs(const NoventraDockView& dock) {
  bool address_before_tabs = false;
  for (views::View* child : dock.children()) {
    if (!OccupiesDockSpace(child)) {
      continue;
    }
    if (child->GetProperty(kNoventraDockKind) == 1) {
      address_before_tabs = true;
    }
    if (child->GetProperty(kNoventraDockKind) == 2) {
      return address_before_tabs;
    }
  }
  return false;
}

views::View* SidebarNewTab(const NoventraDockView& dock) {
  if (SidebarAddressInTabs(dock)) {
    for (views::View* child : dock.children()) {
      if (OccupiesDockSpace(child) &&
          child->GetID() == VIEW_ID_DYNAMIC_NEW_TAB_BUTTON) {
        return child;
      }
    }
  }
  return nullptr;
}

// Measurement and placement must share the same row packing. Expanded sidebars
// use native button-width cells; extension regions reserve enough whole cells
// for their target and outgoing animation and wrap intact when necessary.
// Narrow rails retain their 40-pixel cells and native vertical extension stack.
VerticalDockLayout VerticalDockBounds(const NoventraDockView& dock,
                                      int width,
                                      bool compact,
                                      int tab_height,
                                      bool expand_library = false) {
  width = std::max(0, width);
  const int cell_width = compact ? 40 : 36;
  const int row_height = compact ? 40 : 36;
  // Four navigation cells fit exactly at the 144-DIP expanded minimum.
  const int row_inset = 0;
  const int row_width = width - 2 * row_inset;
  const int columns = std::max(1, row_width / cell_width);
  VerticalDockLayout result;
  auto* lower_new_tab = SidebarNewTab(dock);
  const bool tab_address = SidebarAddressInTabs(dock);
  for (views::View* child : dock.children()) {
    if (!OccupiesDockSpace(child)) {
      continue;
    }
    if (child->GetProperty(kNoventraDockKind) == 3) {
      continue;
    }
    if (child->GetID() == VIEW_ID_APP_MENU &&
        child->GetProperty(kNoventraDockSharedFooter)) {
      result.footer_menu = child;
    }
    if (child->GetProperty(kNoventraDockBookmarkPanel)) {
      result.footer_bookmarks = child;
    }
  }
  int y = dock.vertical_content_inset();
  int column = 0;
  const auto flush_row = [&] {
    if (column) {
      y += row_height;
      column = 0;
    }
  };
  for (views::View* child : dock.children()) {
    if (!OccupiesDockSpace(child)) {
      result.children.emplace_back(child, gfx::Rect());
      continue;
    }
    const int kind = child->GetProperty(kNoventraDockKind);
    if (kind == 5) {
      auto* library = views::AsViewClass<NoventraBookmarksView>(child);
      if (library->inline_folders()) {
        result.library = library;
        const bool compact_library =
            compact || width < 160 ||
            (!expand_library && child->GetProperty(kNoventraDockCompact));
        result.library_height = compact_library ? 40 : library->ExpandedHeight();
        result.library_gap = compact_library ? 0 : 8;
        continue;
      }
    }
    // The active tab opens its URL editor; a separate Search button would
    // duplicate that action, including when this dock is an icon rail.
    if (kind == 3) {
      result.spaces = child;
      result.footer_height =
          child->GetPreferredSize(views::SizeBounds(width, {})).height();
      continue;
    }
    if (child == result.footer_menu || child == lower_new_tab ||
        child == result.footer_bookmarks) {
      continue;
    }
    if (dock.compact_overflow() &&
        (kind == 1 || child->GetID() == VIEW_ID_DYNAMIC_NEW_TAB_BUTTON ||
         child->GetID() == VIEW_ID_APP_MENU)) {
      flush_row();
      result.children.emplace_back(child, gfx::Rect(0, y, width, 40));
      y += 40;
      continue;
    }
    if (kind == 0 || (kind == 4 && !compact)) {
      const int preferred_width =
          kind == 4 ? DockRegionPreferredSize(child).width() : cell_width;
      const int cells = std::min(
          columns, 1 + (std::max(1, preferred_width) - 1) / cell_width);
      if (column + cells > columns) {
        flush_row();
      }
      // A region wider than the available whole cells gets the complete row,
      // including its remainder. Its native layout handles overflow buttons.
      const int extent = preferred_width > columns * cell_width
                             ? row_width
                             : std::min(row_width, cells * cell_width);
      result.children.emplace_back(
          child,
          gfx::Rect(row_inset + column * cell_width, y, extent, row_height));
      column += cells;
      if (column == columns) {
        flush_row();
      }
      continue;
    }
    flush_row();
    int extent = kind == 1 && tab_address ? 38 : 40;
    if (kind == 2) {
      extent = tab_height;
      ++result.tabs;
    } else if (kind == 5) {
      extent = expand_library ? views::AsViewClass<NoventraBookmarksView>(child)
                                    ->ExpandedHeight()
                              : child->GetPreferredSize().height();
    } else if (kind == 4) {
      extent = std::max(40, DockRegionPreferredSize(child).height());
    }
    result.children.emplace_back(child, gfx::Rect(0, y, width, extent));
    y += extent;
    if (kind == 2 && lower_new_tab) {
      // Keep New tab with the actual tab list, before any following tools.
      result.children.emplace_back(
          lower_new_tab, gfx::Rect(row_inset, y, row_width, 30));
      y += 30;
    }
  }
  flush_row();
  result.horizontal_footer =
      width >= VerticalTabStripRegionView::kUncollapsedMinWidth;
  if (result.horizontal_footer) {
    const int tools_width = (result.footer_bookmarks ? 36 : 0) +
                            (result.footer_menu ? 36 : 0);
    result.footer_height = tools_width ? 36 : 0;
    if (result.spaces) {
      result.footer_height = std::max(
          result.footer_height,
          result.spaces->GetPreferredSize(views::SizeBounds(
              std::max(0, width - tools_width), {})).height());
    }
  } else {
    if (result.footer_menu) result.footer_height += 36;
    if (result.footer_bookmarks) result.footer_height += 36;
  }
  result.height = y + result.footer_height +
                  result.library_height + result.library_gap;
  return result;
}

int MinimumVerticalExtent(const NoventraDockView& dock, int width, bool compact,
                          bool expand_library = false) {
  // Keep several tab rows visible before widening a crowded side dock.
  return VerticalDockBounds(dock, width, compact, 160, expand_library).height;
}
// Use native control sizes and toolbar spacing. Share the top row with the
// native tab strip when its minimum width and the controls fit; otherwise
// retain the accessible wrapped layout.
struct HorizontalDockLayout {
  std::vector<std::pair<views::View*, gfx::Rect>> children;
  int height = 0;
};
HorizontalDockLayout HorizontalDockBounds(const NoventraDockView& dock,
                                          int width,
                                          bool top_edge) {
  const int control_height =
      std::max(GetLayoutConstant(LayoutConstant::kToolbarButtonHeight),
               GetLayoutConstant(LayoutConstant::kLocationBarHeight));
  const auto padding = GetLayoutInsets(TOOLBAR_INTERIOR_MARGIN);
  const int row_height = control_height + padding.height();
  const int tab_height = GetLayoutConstant(LayoutConstant::kTabStripHeight);
  const int spacing = GetLayoutConstant(LayoutConstant::kToolbarElementPadding);
  width = std::max(0, width);
  HorizontalDockLayout result;
  std::vector<std::vector<views::View*>> rows;
  views::View* tabs = nullptr;
  for (views::View* child : dock.children()) {
    if (OccupiesDockSpace(child) &&
        child->GetProperty(kNoventraDockKind) == 2) {
      tabs = child;
    }
  }
  views::View* first = nullptr;
  for (views::View* child : dock.children()) {
    if (OccupiesDockSpace(child)) { first = child; break; }
  }
  // A tabs-first top layout is the normal two-row browser. Navigation-first
  // top layouts retain the compact single row when their native controls fit.
  const bool tabs_first = top_edge && tabs && first == tabs;
  if (tabs && top_edge && !tabs_first) {
    const auto insets = dock.CaptionInsetsForRow(0) + padding;
    const int available = std::max(0, width - insets.width());
    // Respect the native tab strip's minimum rather than clipping its close,
    // overflow or drag targets to make a row appear to fit.
    const int tabs_minimum = std::max(160, tabs->GetMinimumSize().width());
    std::vector<views::View*> row;
    views::View* address = nullptr;
    int minimum = 0;
    for (views::View* child : dock.children()) {
      if (!OccupiesDockSpace(child)) {
        continue;
      }
      if (!row.empty()) {
        minimum += spacing;
      }
      row.push_back(child);
      minimum += child == tabs ? tabs_minimum : MinimumHorizontalExtent(child);
      if (child->GetProperty(kNoventraDockKind) == 1) {
        address = child;
      }
    }
    if (minimum <= available) {
      const int extra = available - minimum;
      // Keep an editable address capsule, giving most spare space to tabs.
      // Its native omnibox keeps this allocation when focus enters the field.
      const int address_extra = address ? std::min(extra / 3, 160) : 0;
      const int height = std::max(row_height, tab_height);
      int x = insets.left();
      for (auto* child : row) {
        int extent = child == tabs ? tabs_minimum : MinimumHorizontalExtent(child);
        extent += child == tabs ? extra - address_extra
                               : child == address ? address_extra : 0;
        const bool address_slot = child == address;
        const int child_height = child == tabs ? tab_height : control_height;
        result.children.emplace_back(
            child, gfx::Rect(x, address_slot ? 0 : (height - child_height) / 2,
                             extent, address_slot ? height : child_height));
        x += extent + spacing;
      }
      result.height = height;
      return result;
    }
  }
  const int first_navigation_y = tabs && tabs_first ? tab_height : 0;
  const auto row_insets = [&](int y) {
    // The native controls (including the real omnibox) start below the row's
    // top padding. A caption entirely above that content must not reserve an
    // empty column beside it; full-height window controls still exclude it.
    return dock.CaptionInsetsForRow(y + padding.top()) + padding;
  };
  int used = 0;
  for (views::View* child : dock.children()) {
    if (!OccupiesDockSpace(child) || child == tabs) {
      continue;
    }
    int row_y = first_navigation_y +
                std::max(0, static_cast<int>(rows.size()) - 1) * row_height;
    int row_width = std::max(0, width - row_insets(row_y).width());
    const int gap = rows.empty() ? 0 : spacing;
    if (rows.empty() ||
        used + gap + std::min(row_width, MinimumHorizontalExtent(child)) >
            row_width) {
      rows.emplace_back();
      used = 0;
      row_y = first_navigation_y + (rows.size() - 1) * row_height;
      row_width = std::max(0, width - row_insets(row_y).width());
    }
    if (!rows.back().empty()) {
      used += spacing;
    }
    rows.back().push_back(child);
    used += std::min(row_width, MinimumHorizontalExtent(child));
  }
  int y = first_navigation_y;
  if (tabs && tabs_first) {
    const auto insets = dock.CaptionInsetsForRow(0);
    result.children.emplace_back(
        tabs, gfx::Rect(insets.left(), 0, std::max(0, width - insets.width()),
                        tab_height));
  }
  for (const auto& row : rows) {
    const auto insets = row_insets(y);
    const int row_width = std::max(0, width - insets.width());
    int minimum = (static_cast<int>(row.size()) - 1) * spacing;
    for (auto* child : row) {
      minimum += std::min(row_width, MinimumHorizontalExtent(child));
    }
    int x = insets.left();
    for (auto* child : row) {
      int extent = std::min(row_width, MinimumHorizontalExtent(child));
      const bool address_slot = child->GetProperty(kNoventraDockKind) == 1;
      int address_margin = 0;
      if (address_slot) {
        extent += std::max(0, row_width - minimum);
        // Keep a readable, compact field centered between the leading and
        // trailing controls. Spare space surrounds the field, so the menu and
        // extension region still reach the end of the row.
        constexpr int kMaximumAddressWidth = 720;
        address_margin = std::max(0, extent - kMaximumAddressWidth);
        extent -= address_margin;
        x += address_margin / 2;
      }
      // The address slot anchors a separate native omnibox pane. Keep its
      // entire row available so that pane can center the actual field itself.
      result.children.emplace_back(
          child, gfx::Rect(x, y + (address_slot ? 0 : padding.top()), extent,
                           address_slot ? row_height : control_height));
      x += extent + spacing;
      x += address_margin - address_margin / 2;
    }
    y += row_height;
  }
  if (tabs && !tabs_first) {
    result.children.emplace_back(tabs, gfx::Rect(0, y, width, tab_height));
    y += tab_height;
  }
  // Include the lower padding, even though no child occupies those pixels.
  result.height = y;
  return result;
}
}  // namespace

NoventraDockView::NoventraDockView(noventra::DockEdge edge) : edge_(edge) {
  // WebContents owns a compositor surface. All dock chrome must share a
  // layer above it, including buttons and background without their own layers.
  SetPaintToLayer();
  layer()->SetFillsBoundsOpaquely(false);
  SetMirrored(false);
  SetProperty(views::kBubbleOpensInsideWindow, true);
  SetBackground(std::make_unique<DockMaterialBackground>());
  SetEventTargeter(std::make_unique<views::ViewTargeter>(this));
  GetViewAccessibility().SetRole(ax::mojom::Role::kToolbar);
  GetViewAccessibility().SetName(u"Browser controls");
  if (edge_ == noventra::DockEdge::kLeft ||
      edge_ == noventra::DockEdge::kRight) {
    resize_edge_layer_ = std::make_unique<ui::LayerSolidColor>();
    resize_edge_layer_->SetName("NoventraSidebarResizeEdge");
    layer()->Add(resize_edge_layer_.get());
  }
}

NoventraDockView::~NoventraDockView() = default;

void NoventraDockView::SetCommandPopup(bool enabled) {
  if (command_popup_ == enabled) {
    return;
  }
  command_popup_ = enabled;
  UpdateGlassBlur();
  InvalidateLayout();
  SchedulePaint();
}

void NoventraDockView::SetFloatingGlass(bool floating) {
  if (floating_glass_ == floating) {
    return;
  }
  floating_glass_ = floating;
  layer()->SetRoundedCornerRadius(gfx::RoundedCornersF(floating ? 8.f : 0.f));
  layer()->SetIsFastRoundedCorner(true);
  UpdateGlassBlur();
}

void NoventraDockView::SetPaintGlassMaterial(bool paint) {
  if (paint_glass_material_ == paint) {
    return;
  }
  paint_glass_material_ = paint;
  UpdateGlassBlur();
  SchedulePaint();
}

void NoventraDockView::OnThemeChanged() {
  views::AccessiblePaneView::OnThemeChanged();
  UpdateGlassBlur();
  UpdateResizeEdge();
  SchedulePaint();
}

std::vector<gfx::Rect> NoventraDockView::GetSharedNewTabGlassBounds() const {
  if (!floating_glass_ || command_popup_ || !paint_glass_material_ ||
      !GetWidget() || GetLocalBounds().IsEmpty()) {
    VLOG(1) << "NoventraGlass dock.share gated dock=" << this
            << " floating=" << floating_glass_ << " popup=" << command_popup_
            << " paint_material=" << paint_glass_material_
            << " widget=" << (GetWidget() != nullptr)
            << " local_empty=" << GetLocalBounds().IsEmpty();
    return {};
  }
  auto* browser = BrowserView::GetBrowserViewForNativeWindow(
      GetWidget()->GetNativeWindow());
  auto* contents = browser ? browser->multi_contents_view() : nullptr;
  if (!contents) {
    VLOG(1) << "NoventraGlass dock.share missing_contents dock=" << this;
    return {};
  }
  // Share only pixels actually covered by the trusted page. Frame insets and
  // other split panes keep their own readable material. Never extend page
  // transparency to a website.
  const gfx::Rect bounds = GetBoundsInScreen();
  std::vector<gfx::Rect> shared;
  bool foreground_overlap = false;
  contents->ExecuteOnEachVisibleContentsView(base::BindRepeating(
      [](const gfx::Rect& dock, std::vector<gfx::Rect>* regions,
         bool* foreground_overlap, ContentsWebView* view) {
        if (!view->HasNoventraGlassBackground()) {
          VLOG(1) << "NoventraGlass dock.share pane_material=0 view=" << view;
          return;
        }
        gfx::Rect overlap = view->GetBoundsInScreen();
        overlap.Intersect(dock);
        if (!overlap.IsEmpty()) {
          gfx::Rect local = overlap;
          const gfx::Point origin = view->GetBoundsInScreen().origin();
          local.Offset(-origin.x(), -origin.y());
          if (!view->CanShareNoventraGlassBackground(local)) {
            VLOG(1) << "NoventraGlass dock.share pane_safe=0 view=" << view
                    << " screen=" << view->GetBoundsInScreen().ToString()
                    << " local_overlap=" << local.ToString();
            *foreground_overlap = true;
            return;
          }
          overlap.Offset(-dock.x(), -dock.y());
          regions->push_back(overlap);
          VLOG(1) << "NoventraGlass dock.share pane_safe=1 view=" << view
                  << " screen=" << view->GetBoundsInScreen().ToString()
                  << " dock_local_overlap=" << overlap.ToString();
        }
      },
      bounds, &shared, &foreground_overlap));
  VLOG(1) << "NoventraGlass dock.share selected dock=" << this
          << " screen=" << bounds.ToString()
          << " foreground_or_unknown=" << foreground_overlap
          << " shared_count=" << (foreground_overlap ? 0 : shared.size());
  if (VLOG_IS_ON(1) && !foreground_overlap) {
    for (const gfx::Rect& rect : shared) {
      VLOG(1) << "NoventraGlass dock.share rect dock=" << this
              << " local=" << rect.ToString();
    }
  }
  // Keep a uniform surface when any trusted foreground intersects this dock.
  // Small per-control patches would draw dark bands around the page controls.
  return foreground_overlap ? std::vector<gfx::Rect>() : shared;
}

void NoventraDockView::UpdateResizeEdge() {
  if (!resize_edge_layer_) {
    return;
  }
  resize_edge_layer_->SetVisible(!GetLocalBounds().IsEmpty() &&
                                 GetColorProvider());
  if (!resize_edge_layer_->visible()) {
    return;
  }
  if (resize_edge_layer_->parent() != layer()) {
    layer()->Add(resize_edge_layer_.get());
  }
  // Tab rows paint in their own child layer. Keep the full-height boundary
  // above them, while inheriting the dock's visibility, opacity and clipping.
  bool resize_active = false;
  for (views::View* child : children()) {
    if (auto* region = views::AsViewClass<VerticalTabStripRegionView>(child)) {
      if (auto* handle = region->GetNoventraResizeTarget()) {
        resize_active = handle->GetProperty(kNoventraDockResizeActive) ||
                        handle->HasFocus();
      }
    }
  }
  const int thickness = std::min(resize_active ? 3 : 1, width());
  resize_edge_layer_->SetBounds(
      gfx::Rect(edge_ == noventra::DockEdge::kLeft ? width() - thickness : 0, 0,
                thickness, height()));
  resize_edge_layer_->SetColor(SkColor4f::FromColor(
      resize_active
          ? GetColorProvider()->GetColor(ui::kColorSysPrimary)
          : SkColorSetA(GetColorProvider()->GetColor(kColorToolbarText), 72)));
  layer()->StackAtTop(resize_edge_layer_.get());
}

void NoventraDockView::OnBoundsChanged(const gfx::Rect& previous_bounds) {
  views::AccessiblePaneView::OnBoundsChanged(previous_bounds);
  UpdateGlassBlur();
  if (previous_bounds.size() != size()) {
    UpdateResizeEdge();
  }
}

void NoventraDockView::UpdateGlassBlur() {
  const auto shared = GetSharedNewTabGlassBounds();
  SkRegion blur_region(gfx::RectToSkIRect(GetLocalBounds()));
  for (const gfx::Rect& bounds : shared) {
    blur_region.op(gfx::RectToSkIRect(bounds), SkRegion::kDifference_Op);
  }
  const bool enabled =
      !command_popup_ && paint_glass_material_ && floating_glass_ &&
      helium::ShouldUseNativeFrameMaterials(this) && !blur_region.isEmpty();
  const bool bounds_changed =
      background_blur_bounds_ != GetLocalBounds() ||
      shared_background_bounds_ != shared;
  VLOG(1) << "NoventraGlass dock.blur dock=" << this
          << " local=" << GetLocalBounds().ToString() << " enabled=" << enabled
          << " previous_enabled=" << background_blur_enabled_
          << " bounds_changed=" << bounds_changed
          << " shared_count=" << shared.size()
          << " region_empty=" << blur_region.isEmpty()
          << " region_rects=" << blur_region.computeRegionComplexity()
          << " path_points=" << blur_region.getBoundaryPath().countPoints()
          << " region_bounds="
          << gfx::SkIRectToRect(blur_region.getBounds()).ToString()
          << " previous_sigma=" << layer()->background_blur();
  if (bounds_changed) {
    background_blur_bounds_ = GetLocalBounds();
    shared_background_bounds_ = shared;
    layer()->SetBackdropFilterBounds(blur_region.getBoundaryPath());
    // A reveal can change shared material without resizing this layer. Repaint
    // its background too, otherwise the old full tint survives the new clip.
    SchedulePaint();
    // SetBackdropFilterBounds only updates cc::Layer's inputs; it does not
    // invalidate the effect tree. Reapply the filter in this same task so the
    // new path reaches the compositor even when the blur strength is unchanged.
    if (background_blur_enabled_) {
      layer()->SetBackgroundBlur(0.f);
    }
  }
  if (background_blur_enabled_ != enabled || bounds_changed) {
    background_blur_enabled_ = enabled;
    layer()->SetBackgroundBlur(enabled ? 12.f : 0.f);
  }
}
void NoventraDockView::SetCaptionExclusion(int leading_width, int trailing_width,
                                          int height) {
  if (leading_caption_width_ == leading_width &&
      trailing_caption_width_ == trailing_width && caption_height_ == height)
    return;
  leading_caption_width_ = leading_width;
  trailing_caption_width_ = trailing_width;
  caption_height_ = height;
  InvalidateLayout();
  SchedulePaint();
}
gfx::Insets NoventraDockView::CaptionInsetsForRow(int y) const {
  return y < caption_height_
      ? gfx::Insets::TLBR(0, leading_caption_width_, 0, trailing_caption_width_)
      : gfx::Insets();
}
void NoventraDockView::SetVerticalContentInset(int inset) {
  inset = std::max(0, inset);
  if (vertical_content_inset_ == inset) {
    return;
  }
  vertical_content_inset_ = inset;
  InvalidateLayout();
}
void NoventraDockView::SetCaptionClip(gfx::Rect leading, gfx::Rect trailing) {
  leading.Intersect(GetLocalBounds());
  trailing.Intersect(GetLocalBounds());
  leading_caption_clip_ = leading;
  trailing_caption_clip_ = trailing;
  if (leading.IsEmpty() && trailing.IsEmpty() && clip_path().isEmpty()) return;
  // The frame stays fixed while the dock slides. Mask the whole native layer,
  // including layered children, at the captions' current local coordinates.
  SkPathBuilder builder;
  builder.setFillType(SkPathFillType::kEvenOdd);
  builder.addRect(gfx::RectToSkRect(GetLocalBounds()));
  if (!leading.IsEmpty()) builder.addRect(gfx::RectToSkRect(leading));
  if (!trailing.IsEmpty()) builder.addRect(gfx::RectToSkRect(trailing));
  const auto path = builder.detach();
  if (path != clip_path()) SetClipPath(path);
}
bool NoventraDockView::DoesIntersectRect(const views::View* target,
                                        const gfx::Rect& rect) const {
  // The material extends into the frame's reserved top strip, but its input
  // still belongs to the frame for resizing and dragging the window.
  if (vertical_content_inset_ > 0 &&
      gfx::Rect(0, 0, width(), vertical_content_inset_).Contains(rect)) {
    return false;
  }
  if (leading_caption_clip_.Contains(rect) || trailing_caption_clip_.Contains(rect))
    return false;
  return views::ViewTargeterDelegate::DoesIntersectRect(target, rect);
}
views::View* NoventraDockView::TargetForRect(views::View* root,
                                             const gfx::Rect& rect) {
  DCHECK_EQ(root, this);
  if ((edge_ == noventra::DockEdge::kLeft ||
       edge_ == noventra::DockEdge::kRight) &&
      DoesIntersectRect(this, rect)) {
    // Tabs use their natural height so New Tab follows the rows. Extend hit
    // testing instead of stretching that region or adding a second splitter.
    for (views::View* child : children()) {
      auto* region = views::AsViewClass<VerticalTabStripRegionView>(child);
      if (!region || !region->IsDrawn()) {
        continue;
      }
      if (auto* target = region->GetNoventraResizeTarget()) {
        // Derive the physical side from the actual handle, including Views'
        // RTL transforms. Keep an easy-to-find grip in the collapsed rail.
        auto grip = views::View::ConvertRectToTarget(target, this,
                                                     target->GetLocalBounds());
        // Keep a generous target along the entire edge, including below the
        // natural tab list. Icon rails retain room for their small buttons.
        const int grip_width = std::min(
            width() >= VerticalTabStripRegionView::kUncollapsedMinWidth ? 12 : 8,
            width());
        grip.set_x(grip.CenterPoint().x() < width() / 2 ? 0
                                                        : width() - grip_width);
        grip.set_y(0);
        grip.set_size(gfx::Size(grip_width, height()));
        if (grip.Contains(rect.CenterPoint())) {
          return target;
        }
      }
    }
  }
  return views::ViewTargeterDelegate::TargetForRect(root, rect);
}

bool NoventraDockView::TraverseUsingUpDownKeys() {
  return edge_ == noventra::DockEdge::kLeft ||
         edge_ == noventra::DockEdge::kRight;
}
bool NoventraDockView::HasVisibleControls() const {
  for (views::View* child : children()) if (OccupiesDockSpace(child)) return true;
  return false;
}
views::View* NoventraDockView::CompactMenuAnchor() const {
  for (views::View* child : children()) {
    if (child->GetID() == VIEW_ID_APP_MENU &&
        child->GetProperty(kNoventraDockCompactOverflow) &&
        child->GetEnabled() && child->IsDrawn() &&
        !child->GetLocalBounds().IsEmpty()) {
      return child;
    }
  }
  return nullptr;
}

std::unique_ptr<ui::SimpleMenuModel> NoventraDockView::CompactSpacesMenu()
    const {
  for (views::View* child : children()) {
    if (child->GetProperty(kNoventraDockKind) == 3 && child->GetVisible() &&
        child->GetProperty(kNoventraDockCompactOverflow)) {
      return VerticalTabStripRegionView::CreateAccountSpacesMenu(child);
    }
  }
  return nullptr;
}

void NoventraDockView::RestoreCompactPresentation() {
  // Apply calls this before reparenting/removing any of these direct children.
  // Neither the saved layout nor its explicit visibility changes on resize.
  for (size_t i = 0; i < normal_order_.size(); ++i) {
    auto* child = normal_order_[i].get();
    if (child->parent() == this) {
      child->SetProperty(kNoventraDockCompactOverflow, false);
      child->SetProperty(kNoventraDockSharedFooter, false);
      if (auto* extensions =
              views::AsViewClass<ExtensionsToolbarDesktop>(child)) {
        // Re-evaluate native pin/install/popup state accumulated while hidden.
        extensions->UpdateContainerVisibility();
        extensions->GetAnimatingLayoutManager()->ResetLayout();
      } else if (suppressed_controls_.contains(child)) {
        child->SetVisible(true);
      }
      ReorderChildView(child, i);
    }
  }
  normal_order_.clear();
  suppressed_controls_.clear();
}

void NoventraDockView::PrepareForLayout(int available_height) {
  bool compact = false;
  if (edge_ == noventra::DockEdge::kLeft || edge_ == noventra::DockEdge::kRight)
    for (views::View* child : children())
      if (child->GetVisible() && child->GetProperty(kNoventraDockKind) == 2)
        compact = child->GetPreferredSize().width() < 100;
  compact_ = compact;
  views::View* menu = nullptr;
  for (views::View* child : children()) {
    if (child->GetID() == VIEW_ID_APP_MENU && child->GetVisible() &&
        child->GetEnabled()) {
      menu = child;
      break;
    }
  }
  views::View* spaces = nullptr;
  for (views::View* child : children()) {
    if (child->GetProperty(kNoventraDockKind) == 3 && child->GetVisible()) {
      spaces = child;
      break;
    }
  }
  const bool simplify = menu && compact;
  if (compact_overflow() && (!simplify || compact != compact_tools_)) {
    RestoreCompactPresentation();
  }
  if (simplify && !compact_overflow()) {
    compact_tools_ = compact;
    for (views::View* child : children()) {
      normal_order_.push_back(child);
    }
    auto* focused =
        GetFocusManager() ? GetFocusManager()->GetFocusedView() : nullptr;
    bool move_focus = false;
    for (views::View* child : children()) {
      const bool menu_tool = views::IsViewClass<NoventraBookmarksView>(child) &&
                             child->GetProperty(kNoventraDockKind) != 5 &&
                             !child->GetProperty(kNoventraDockBookmarkPanel);
      if (child->GetProperty(kNoventraDockOverflowable) &&
          (compact || menu_tool)) {
        // Keep saved visibility separate from the compact presentation.
        if (child->GetVisible()) {
          suppressed_controls_.insert(child);
        }
        move_focus |= focused && child->Contains(focused);
        child->SetProperty(kNoventraDockCompactOverflow, true);
        child->SetVisible(false);
      }
    }
    menu->SetProperty(kNoventraDockCompactOverflow, true);
    // Native child order is also keyboard order. Keep New Tab with the tab
    // list; the shared browser/workspace menu follows the footer
    // dots.
    std::vector<views::View*> ordered;
    for (views::View* child : normal_order_) {
      if (child->GetID() == VIEW_ID_DYNAMIC_NEW_TAB_BUTTON) {
        ordered.push_back(child);
      }
    }
    if (!spaces) {
      ordered.push_back(menu);
    }
    for (views::View* child : normal_order_) {
      // Preserve Address's saved side of the tab list. Its position determines
      // whether it belongs to a tab row or remains a standalone shortcut.
      if (child != menu &&
          child->GetID() != VIEW_ID_DYNAMIC_NEW_TAB_BUTTON) {
        ordered.push_back(child);
      }
    }
    if (spaces) {
      ordered.push_back(menu);
    }
    for (size_t i = 0; i < ordered.size(); ++i) {
      ReorderChildView(ordered[i], i);
    }
    if (move_focus) {
      menu->RequestFocus();
    }
  }
  const bool vertical =
      edge_ == noventra::DockEdge::kLeft || edge_ == noventra::DockEdge::kRight;
  for (views::View* child : children()) {
    if (child->GetID() == VIEW_ID_DYNAMIC_NEW_TAB_BUTTON) {
      child->SetProperty(kNoventraDockHorizontal, !vertical);
    }
  }
  if (menu && menu->GetProperty(kNoventraDockSharedFooter) != vertical) {
    menu->SetProperty(kNoventraDockSharedFooter, vertical);
  }
  if (menu) {
    menu->SetProperty(kNoventraDockCompactOverflow, vertical);
    if (vertical && children().back() != menu) {
      ReorderChildView(menu, children().size() - 1);
    }
  }
  const bool shared_menu = vertical && menu;
  if (spaces &&
      spaces->GetProperty(kNoventraDockCompactOverflow) != shared_menu) {
    spaces->SetProperty(kNoventraDockCompactOverflow, shared_menu);
  }
  for (views::View* child : children()) {
    if (auto* tabs = views::AsViewClass<VerticalTabStripRegionView>(child)) {
      tabs->SetNoventraUnifiedAddress(false);
    }
  }
  if (vertical) {
    if (auto* new_tab = SidebarNewTab(*this)) {
      size_t index = children().size();
      for (size_t i = 0; i < children().size(); ++i) {
        if (children()[i]->GetProperty(kNoventraDockKind) == 2) {
          index = i + 1;
          break;
        }
      }
      // ReorderChildView uses final indices; account for removal before the
      // target. The saved model order remains untouched.
      const size_t old = GetIndexOf(new_tab).value();
      if (old < index) {
        --index;
      }
      if (old != index) {
        ReorderChildView(new_tab, index);
      }
      if (auto* button = views::AsViewClass<ToolbarButton>(new_tab)) {
        button->SetHorizontalAlignment(button->GetText().empty()
                                           ? gfx::ALIGN_CENTER
                                           : gfx::ALIGN_LEFT);
      }
    }
  }
  // Keep the actual pinned extension buttons and their popup anchors. In a
  // narrow rail they stack vertically instead of clipping behind one another.
  for (views::View* child : children()) {
    // Extra columns in a short window do not expand the tab strip's state.
    // Tell its space switcher which presentation the dock actually allocated.
    if (child->GetProperty(kNoventraDockKind) == 3) {
      child->SetProperty(kNoventraDockCompact, compact);
      child->SetProperty(kNoventraDockHorizontal,
          edge_ == noventra::DockEdge::kTop || edge_ == noventra::DockEdge::kBottom);
    }
    if (auto* extensions = views::AsViewClass<ExtensionsToolbarDesktop>(child)) {
      auto* layout = extensions->GetTargetLayoutManager();
      const auto orientation = compact ? views::LayoutOrientation::kVertical
                                       : views::LayoutOrientation::kHorizontal;
      layout->SetOrientation(orientation);
      const auto alignment = views::LayoutAlignment::kCenter;
      // Unlike orientation, setting this property always invalidates layout.
      if (layout->cross_axis_alignment() != alignment)
        layout->SetCrossAxisAlignment(alignment);
      // The dock owns the row inset. Extra region padding makes Extensions
      // wider than the adjacent native buttons and forces an early wrap.
      const auto padding = gfx::Insets();
      if (layout->interior_margin() != padding)
        layout->SetInteriorMargin(padding);
      // The animation wrapper also uses its orientation to constrain bounds
      // and slide icons. Leaving it horizontal clips a vertical icon stack.
      auto* animation = extensions->GetAnimatingLayoutManager();
      animation->SetBoundsAnimationMode(
          views::AnimatingLayoutManager::BoundsAnimationMode::kAnimateMainAxis);
      animation->SetOrientation(orientation);
    }
  }
  int preferred_width = 232;
  for (views::View* child : children())
    if (child->GetVisible() && child->GetProperty(kNoventraDockKind) == 2)
      preferred_width = std::clamp(child->GetPreferredSize().width(),
          VerticalTabStripRegionView::kCollapsedWidth,
          VerticalTabStripRegionView::kUncollapsedMaxWidth);
  // Measure the expanded library without toggling its state during layout.
  // Its fixed rows cannot shrink with extra width. Reserve working tab space
  // using the actual section counts and collapse preferences.
  const bool compact_library = compact || (vertical &&
      MinimumVerticalExtent(*this, preferred_width, compact, true) > available_height);
  for (views::View* child : children()) {
    if (child->GetProperty(kNoventraDockKind) != 5) continue;
    // Library property notifications invalidate preferred size, even for an
    // unchanged value. Do not start another invalidation during every layout.
    if (child->GetProperty(kNoventraDockCompact) != compact_library)
      child->SetProperty(kNoventraDockCompact, compact_library);
    if (child->GetProperty(kNoventraDockHorizontal) != !vertical)
      child->SetProperty(kNoventraDockHorizontal, !vertical);
  }
}
int NoventraDockView::CrossAxisSize(int available_width,
                                    int available_height) const {
  if (edge_ == noventra::DockEdge::kTop ||
      edge_ == noventra::DockEdge::kBottom) {
    return HorizontalDockBounds(*this, available_width,
                                edge_ == noventra::DockEdge::kTop)
        .height;
  }
  int cross = 40;
  for (views::View* child : children()) {
    if (!OccupiesDockSpace(child)) {
      continue;
    }
    const int kind = child->GetProperty(kNoventraDockKind);
    if (kind == 2) {
      cross = std::clamp(child->GetPreferredSize().width(),
          VerticalTabStripRegionView::kCollapsedWidth,
          VerticalTabStripRegionView::kUncollapsedMaxWidth);
      break;
    }
    if (kind != 0) {
      cross = 232;
    }
  }
  // A sidebar containing tabs keeps the user's width in short windows; its
  // native tab view scrolls. Extra columns are only for standalone tool docks.
  for (views::View* child : children()) {
    if (OccupiesDockSpace(child) && child->GetProperty(kNoventraDockKind) == 2) {
      return std::min(cross, std::max(0, available_width - 160));
    }
  }
  // Leave room for the web page when a standalone tool dock needs more columns.
  const int limit = std::min(VerticalTabStripRegionView::kUncollapsedMaxWidth,
                             std::max(40, available_width - 160));
  cross = std::min(cross, limit);
  while (cross < limit &&
         MinimumVerticalExtent(*this, cross, compact_) > available_height) {
    cross = std::min(limit, ((cross / 40) + 1) * 40);
  }
  return cross;
}
void NoventraDockView::Layout(PassKey) {
  const bool vertical =
      edge_ == noventra::DockEdge::kLeft || edge_ == noventra::DockEdge::kRight;
  if (!vertical) {
    if (children().size() == 1 &&
        views::IsViewClass<LocationBarView>(children().front())) {
      // This is the separate address pane. Use the real omnibox's native
      // height in both compact top rows and the unchanged sidebar slots.
      auto* field = children().front().get();
      const int field_height =
          std::min(height(),
                   command_popup_ ? LocationBarView::kNoventraCommandPopupHeight
                   : GetProperty(kNoventraDockUnifiedAddress)
                       ? 38
                       : GetLayoutConstant(LayoutConstant::kLocationBarHeight));
      field->SetBounds(0, (height() - field_height) / 2, width(), field_height);
      return;
    }
    for (views::View* child : children())
      if (!OccupiesDockSpace(child)) child->SetBoundsRect(gfx::Rect());
    for (const auto& [child, bounds] : HorizontalDockBounds(
             *this, width(), edge_ == noventra::DockEdge::kTop).children)
      child->SetBoundsRect(bounds);
    return;
  }
  const auto fixed = VerticalDockBounds(*this, width(), compact_, 0);
  int tab_height =
      fixed.tabs ? std::max(0, height() - fixed.height) / fixed.tabs : 0;
  if (SidebarNewTab(*this)) {
    for (views::View* child : children()) {
      if (child->GetProperty(kNoventraDockKind) == 2 &&
          OccupiesDockSpace(child)) {
        // A short tab list takes its natural native height. Overflow still
        // receives the available viewport and uses the native scroll views.
        tab_height = std::min(
            tab_height,
            child->GetPreferredSize(views::SizeBounds(width(), {})).height());
      }
    }
  }
  const auto layout = VerticalDockBounds(*this, width(), compact_, tab_height);
  for (const auto& [child, bounds] : layout.children) {
    child->SetBoundsRect(bounds);
  }
  if (layout.library) {
    layout.library->SetBounds(
        0, std::max(0, height() - layout.footer_height - layout.library_gap -
                           layout.library_height),
        width(), layout.library_height);
  }
  // Spaces is a footer, independent of saved control order and tab count.
  // Reserve its height above so neither scrolling tabs nor later controls
  // overlap it, including a sidebar that contains no tab strip.
  if (layout.spaces) {
    const int leading = layout.horizontal_footer && layout.footer_bookmarks
                            ? 36 : 0;
    const int trailing = layout.horizontal_footer && layout.footer_menu
                             ? 36 : 0;
    const int space_width = std::max(0, width() - leading - trailing);
    const int space_height = layout.horizontal_footer
        ? std::min(layout.footer_height,
                   layout.spaces->GetPreferredSize(
                       views::SizeBounds(space_width, {})).height())
        : layout.footer_height - (layout.footer_menu ? 36 : 0) -
              (layout.footer_bookmarks ? 36 : 0);
    layout.spaces->SetBounds(
        leading, std::max(0, height() - layout.footer_height) +
                     (layout.horizontal_footer
                          ? (layout.footer_height - space_height) / 2
                          : (layout.footer_bookmarks ? 36 : 0)),
        space_width, space_height);
  }
  if (layout.footer_bookmarks) {
    layout.footer_bookmarks->SetBounds(
        layout.horizontal_footer ? 0 : std::max(0, (width() - 36) / 2),
        std::max(0, height() - layout.footer_height), std::min(36, width()),
        36);
  }
  if (layout.footer_menu) {
    const int extent = std::min(36, width());
    layout.footer_menu->SetBounds(
        layout.horizontal_footer ? width() - extent : (width() - extent) / 2,
        std::max(0, height() - 36), extent, 36);
  }
  UpdateResizeEdge();
}
BEGIN_METADATA(NoventraDockView)
END_METADATA
