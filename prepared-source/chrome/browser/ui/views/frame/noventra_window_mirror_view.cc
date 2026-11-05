// Copyright 2026 The Noventra Authors
#include "chrome/browser/ui/views/frame/noventra_window_mirror_view.h"

#include <algorithm>
#include <cstdlib>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "base/functional/bind.h"
#include "base/memory/raw_ptr.h"
#include "base/memory/weak_ptr.h"
#include "base/strings/utf_string_conversions.h"
#include "base/task/single_thread_task_runner.h"
#include "cc/paint/paint_flags.h"
#include "chrome/app/chrome_command_ids.h"
#include "chrome/app/vector_icons/vector_icons.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/ui/browser_commands.h"
#include "chrome/browser/ui/browser_window.h"
#include "chrome/browser/ui/browser_window/public/browser_window_interface.h"
#include "chrome/browser/ui/browser_window/public/create_browser_window.h"
#include "chrome/browser/ui/color/chrome_color_id.h"
#include "chrome/browser/ui/noventra/window_mirror_service.h"
#include "chrome/browser/ui/tabs/tab_enums.h"
#include "chrome/browser/ui/tabs/tab_strip_model.h"
#include "chrome/browser/ui/views/frame/browser_view.h"
#include "chrome/browser/ui/views/frame/multi_contents_view.h"
#include "chrome/browser/ui/views/frame/noventra_dock_controller.h"
#include "chrome/browser/ui/views/frame/tab_strip_region_view.h"
#include "chrome/browser/ui/views/frame/vertical_tab_strip_region_view.h"
#include "chrome/browser/ui/views/tabs/vertical/vertical_tab_strip_bottom_container.h"
#include "chrome/browser/ui/views/toolbar/toolbar_view.h"
#include "chrome/browser/ui/views/helium/native_frame_materials.h"
#include "components/prefs/pref_change_registrar.h"
#include "components/prefs/pref_service.h"
#include "components/vector_icons/vector_icons.h"
#include "ui/accessibility/ax_enums.mojom.h"
#include "ui/base/metadata/metadata_impl_macros.h"
#include "ui/base/models/image_model.h"
#include "ui/base/mojom/menu_source_type.mojom.h"
#include "ui/color/color_provider.h"
#include "ui/compositor/layer.h"
#include "ui/events/event.h"
#include "ui/events/keycodes/keyboard_codes.h"
#include "ui/gfx/canvas.h"
#include "ui/gfx/color_utils.h"
#include "ui/gfx/geometry/vector2d.h"
#include "ui/views/accessibility/view_accessibility.h"
#include "ui/views/background.h"
#include "ui/views/border.h"
#include "ui/views/context_menu_controller.h"
#include "ui/views/controls/button/image_button.h"
#include "ui/views/controls/button/image_button_factory.h"
#include "ui/views/controls/button/label_button.h"
#include "ui/views/controls/button/md_text_button.h"
#include "ui/views/controls/label.h"
#include "ui/views/controls/scroll_view.h"
#include "ui/views/layout/box_layout.h"
#include "ui/views/view.h"
#include "ui/views/view_class_properties.h"
#include "ui/views/view_tracker.h"
#include "ui/views/widget/widget.h"

namespace {
using Row = noventra::WindowMirrorService::Row;

class MirrorActions {
 public:
  virtual void Select(const std::string& id, bool edit_address = false) = 0;
  virtual void Close(const std::string& id) = 0;
  virtual void Context(const std::string& id, const gfx::Point& screen) = 0;
  virtual void Drop(const std::string& id, const gfx::Point& screen) = 0;
  virtual void CaptureEnded() = 0;

 protected:
  virtual ~MirrorActions() = default;
};

// These are presentation buttons, not TabModels or hidden page copies. Use
// browser color tokens so extension themes and native light/dark/glass choices
// apply to this strip just as they do to ordinary tabs.
class NoventraMirrorTabButton : public views::LabelButton,
                                public views::ContextMenuController {
  METADATA_HEADER(NoventraMirrorTabButton, views::LabelButton)
 public:
  NoventraMirrorTabButton(MirrorActions* actions, Row row)
      : views::LabelButton(
            base::BindRepeating(&NoventraMirrorTabButton::Pressed,
                                base::Unretained(this))),
        actions_(actions) {
    SetFocusBehavior(FocusBehavior::ALWAYS);
    SetBorder(views::CreateEmptyBorder(gfx::Insets::VH(0, 8)));
    SetImageLabelSpacing(6);
    set_context_menu_controller(this);
    GetViewAccessibility().SetRole(ax::mojom::Role::kTab);
    close_ = AddChildView(views::CreateVectorImageButtonWithNativeTheme(
        base::BindRepeating(&NoventraMirrorTabButton::Close,
                            base::Unretained(this)),
        kCloseIcon, 14));
    Update(std::move(row), false);
  }

  void Update(Row row, bool compact) {
    row_ = std::move(row);
    compact_ = compact || row_.pinned;
    SetText(compact_ ? std::u16string() : row_.title);
    SetHorizontalAlignment(compact_ ? gfx::ALIGN_CENTER : gfx::ALIGN_LEFT);
    SetImageCentered(compact_);
    SetImageModel(STATE_NORMAL,
                  row_.favicon.IsEmpty()
                      ? ui::ImageModel::FromVectorIcon(vector_icons::kGlobeIcon,
                                                       ui::kColorIcon, 16)
                      : row_.favicon);
    SetTooltipText(row_.title + u"\n" + base::UTF8ToUTF16(row_.url.spec()) +
                   (row_.local ? u"" : u"\nOpen in another window"));
    GetViewAccessibility().SetName(row_.title);
    GetViewAccessibility().SetIsSelected(row_.selected);
    GetViewAccessibility().SetDescription(
        row_.local ? u"Tab in this window" : u"Shared tab in another window");
    close_->SetTooltipText(u"Close " + row_.title);
    close_->SetVisible(!compact_);
    const auto* colors = GetColorProvider();
    if (colors) {
      const bool frame_active =
          !GetWidget() || GetWidget()->ShouldPaintAsActive();
      SetEnabledTextColors(colors->GetColor(
          row_.selected
              ? (frame_active ? kColorTabForegroundActiveFrameActive
                              : kColorTabForegroundActiveFrameInactive)
              : (frame_active ? kColorTabForegroundInactiveFrameActive
                              : kColorTabForegroundInactiveFrameInactive)));
    }
    InvalidateLayout();
    SchedulePaint();
  }

  const std::string& id() const { return row_.id; }
  bool held() const { return pressed_; }
  void SetCompact(bool compact) {
    if (compact_ != (compact || row_.pinned)) {
      Update(row_, compact);
    }
  }

  void Layout(PassKey) override {
    LayoutSuperclass<views::LabelButton>(this);
    if (close_->GetVisible()) {
      close_->SetBounds(std::max(0, width() - 27), (height() - 24) / 2, 24, 24);
      auto bounds = label()->bounds();
      bounds.set_width(std::max(0, close_->x() - bounds.x() - 4));
      label()->SetBoundsRect(bounds);
    }
  }

  void OnPaintBackground(gfx::Canvas* canvas) override {
    if (!row_.selected && GetState() == STATE_NORMAL) {
      return;
    }
    const auto* colors = GetColorProvider();
    if (!colors) {
      return;
    }
    const bool active = !GetWidget() || GetWidget()->ShouldPaintAsActive();
    SkColor color = colors->GetColor(
        row_.selected
            ? (active ? kColorTabBackgroundActiveFrameActive
                      : kColorTabBackgroundActiveFrameInactive)
            : (active ? kColorTabBackgroundInactiveHoverFrameActive
                      : kColorTabBackgroundInactiveHoverFrameInactive));
    if (helium::ShouldUseNativeFrameMaterials(this)) {
      color = color_utils::AlphaBlend(color, SK_ColorTRANSPARENT,
                                      row_.selected ? 0.24f : 0.14f);
    }
    cc::PaintFlags flags;
    flags.setAntiAlias(true);
    flags.setColor(color);
    gfx::RectF bounds(GetLocalBounds());
    bounds.Inset(1);
    canvas->DrawRoundRect(bounds, 10, flags);
  }

  void OnThemeChanged() override {
    views::LabelButton::OnThemeChanged();
    Update(row_, compact_);
  }

  bool OnMousePressed(const ui::MouseEvent& event) override {
    pressed_ = event.IsOnlyLeftMouseButton();
    edit_address_ = pressed_ && row_.selected && row_.local &&
                    !event.IsShiftDown() && !event.IsControlDown() &&
                    !event.IsCommandDown() && !event.IsAltDown();
    dragging_ = cancelled_ = false;
    start_ = views::View::ConvertPointToScreen(this, event.location());
    if (event.IsOnlyMiddleMouseButton()) {
      return true;
    }
    return views::LabelButton::OnMousePressed(event);
  }

  bool OnMouseDragged(const ui::MouseEvent& event) override {
    const auto now = views::View::ConvertPointToScreen(this, event.location());
    if (pressed_ && !cancelled_ &&
        (std::abs(now.x() - start_.x()) > 5 ||
         std::abs(now.y() - start_.y()) > 5)) {
      dragging_ = true;
    }
    return views::LabelButton::OnMouseDragged(event);
  }

  void OnMouseReleased(const ui::MouseEvent& event) override {
    const auto point =
        views::View::ConvertPointToScreen(this, event.location());
    const bool drop = dragging_ && !cancelled_;
    if (event.IsOnlyMiddleMouseButton()) {
      actions_->Close(row_.id);
    } else {
      views::LabelButton::OnMouseReleased(event);
      if (drop) {
        actions_->Drop(row_.id, point);
      }
    }
    pressed_ = dragging_ = cancelled_ = false;
    edit_address_ = false;
    actions_->CaptureEnded();
  }

  void OnMouseCaptureLost() override {
    pressed_ = dragging_ = cancelled_ = false;
    edit_address_ = false;
    views::LabelButton::OnMouseCaptureLost();
    actions_->CaptureEnded();
  }

  bool OnKeyPressed(const ui::KeyEvent& event) override {
    if (event.key_code() == ui::VKEY_RETURN && row_.selected && row_.local) {
      actions_->Select(row_.id, true);
      return true;
    }

    if (event.key_code() == ui::VKEY_ESCAPE && pressed_) {
      // Keep the input holder alive until release/capture loss. Otherwise an
      // orphan release could select a remote tab after Escape cancelled drag.
      cancelled_ = true;
      dragging_ = false;
      SetState(STATE_NORMAL);
      return true;
    }
    return views::LabelButton::OnKeyPressed(event);
  }

 private:
  void Pressed() {
    if (!dragging_ && !cancelled_) {
      actions_->Select(row_.id, edit_address_);
    }
  }
  void Close() { actions_->Close(row_.id); }
  void ShowContextMenuForViewImpl(views::View*,
                                  const gfx::Point& point,
                                  ui::mojom::MenuSourceType) override {
    actions_->Context(row_.id, point);
  }
  raw_ptr<MirrorActions> actions_;
  Row row_;
  raw_ptr<views::ImageButton> close_ = nullptr;
  bool compact_ = false;
  bool pressed_ = false;
  bool dragging_ = false;
  bool cancelled_ = false;
  bool edit_address_ = false;
  gfx::Point start_;
};
BEGIN_METADATA(NoventraMirrorTabButton)
END_METADATA

class NoventraMirrorTabStrip : public views::View {
  METADATA_HEADER(NoventraMirrorTabStrip, views::View)
 public:
  NoventraMirrorTabStrip(MirrorActions* actions, base::RepeatingClosure new_tab)
      : actions_(actions) {
    SetProperty(views::kViewIgnoredByLayoutKey, true);
    GetViewAccessibility().SetRole(ax::mojom::Role::kTabList);
    GetViewAccessibility().SetName(u"Shared tabs");
    scroll_ = AddChildView(std::make_unique<views::ScrollView>());
    scroll_->SetUseContentsPreferredSize(true);
    contents_ = scroll_->SetContents(std::make_unique<views::View>());
    new_tab_ = contents_->AddChildView(
        std::make_unique<views::LabelButton>(std::move(new_tab), u"New Tab"));
    new_tab_->SetImageModel(
        views::Button::STATE_NORMAL,
        ui::ImageModel::FromVectorIcon(kAddIcon, ui::kColorIcon, 16));
    new_tab_->SetTooltipText(u"New Tab");
  }

  views::View* DefaultFocusable() const {
    for (const auto& row : rows_) {
      if (row.selected && buttons_.at(row.id)->IsFocusable()) {
        return buttons_.at(row.id).get();
      }
    }
    return new_tab_;
  }

  bool held() const {
    return std::ranges::any_of(
        buttons_, [](const auto& item) { return item.second->held(); });
  }

  void Update(std::vector<Row> rows, bool vertical, int row_height) {
    const auto selected = std::ranges::find(rows, true, &Row::selected);
    const std::string next =
        selected == rows.end() ? std::string() : selected->id;
    reveal_selection_ |= next != selected_ || vertical_ != vertical;
    selected_ = next;
    rows_ = std::move(rows);
    vertical_ = vertical;
    views::View* button_parent = vertical_ ? contents_.get() : this;
    if (new_tab_->parent() != button_parent) {
      button_parent->AddChildView(
          new_tab_->parent()->RemoveChildViewT(new_tab_.get()));
    }
    scroll_->SetTreatAllScrollEventsAsHorizontal(!vertical_);
    row_height_ = row_height;
    std::map<std::string, bool> keep;
    for (const auto& row : rows_) {
      keep[row.id] = true;
    }
    for (auto it = buttons_.begin(); it != buttons_.end();) {
      if (!keep.contains(it->first)) {
        contents_->RemoveChildViewT(it->second);
        it = buttons_.erase(it);
      } else {
        ++it;
      }
    }
    for (const auto& row : rows_) {
      auto found = buttons_.find(row.id);
      if (found == buttons_.end()) {
        buttons_[row.id] = contents_->AddChildView(
            std::make_unique<NoventraMirrorTabButton>(actions_, row));
      } else {
        found->second->Update(row, vertical_ && width() < 100);
      }
    }
    scroll_->SetHorizontalScrollBarMode(
        vertical_ ? views::ScrollView::ScrollBarMode::kDisabled
                  : views::ScrollView::ScrollBarMode::kHiddenButEnabled);
    scroll_->SetVerticalScrollBarMode(
        vertical_ ? views::ScrollView::ScrollBarMode::kHiddenButEnabled
                  : views::ScrollView::ScrollBarMode::kDisabled);
    InvalidateLayout();
  }

  std::string RowAt(const gfx::Point& screen) const {
    for (const auto& [id, button] : buttons_) {
      if (button->GetBoundsInScreen().Contains(screen)) {
        return id;
      }
    }
    return {};
  }

  void Layout(PassKey) override {
    if (last_size_ != size()) {
      reveal_selection_ = true;
      last_size_ = size();
    }
    scroll_->SetBounds(0, 0, vertical_ ? width() : std::max(0, width() - 36),
                       height());
    const bool compact = vertical_ && width() < 100;
    const int ordinary_height = compact ? 30 : row_height_;
    const int columns = std::max(1, width() / 44);
    int x = 2;
    int y = 2;
    int pin_count = 0;
    for (const auto& row : rows_) {
      auto* button = buttons_.at(row.id).get();
      button->SetCompact(compact);
      if (vertical_) {
        if (row.pinned && !compact) {
          const int column = pin_count % columns;
          const int line = pin_count / columns;
          button->SetBounds(2 + column * 44, 2 + line * 44, 40, 40);
          ++pin_count;
          y = 2 + ((pin_count + columns - 1) / columns) * 44;
        } else {
          button->SetBounds(2, y, std::max(0, width() - 4), ordinary_height);
          y += ordinary_height + 2;
        }
      } else {
        const int tab_width = row.pinned ? 40 : 160;
        button->SetBounds(x, 0, tab_width, height());
        x += tab_width + 2;
      }
    }
    new_tab_->SetText(vertical_ && !compact ? u"New Tab" : u"");
    new_tab_->GetViewAccessibility().SetName(u"New Tab");
    new_tab_->SetHorizontalAlignment(compact ? gfx::ALIGN_CENTER
                                             : gfx::ALIGN_LEFT);
    new_tab_->SetImageCentered(compact);
    const gfx::Size extent = vertical_
                                 ? gfx::Size(width(), y + ordinary_height + 8)
                                 : gfx::Size(x + 2, height());
    if (vertical_) {
      new_tab_->SetBounds(2, y + 3, std::max(0, width() - 4), ordinary_height);
    } else {
      // Keep the add button reachable while the shared catalog scrolls.
      new_tab_->SetBounds(std::max(0, width() - 32), 0, 32, height());
    }
    contents_->SetPreferredSize(extent);
    // A plain manually laid-out View has no layout manager to apply its
    // preferred size. Its real bounds must exist for paint, input and scroll.
    gfx::Size painted_extent = extent;
    painted_extent.SetToMax(scroll_->GetContentsBounds().size());
    contents_->SetSize(painted_extent);
    scroll_->DeprecatedLayoutImmediately();
    if (reveal_selection_) {
      reveal_selection_ = false;
      if (auto selected = buttons_.find(selected_);
          selected != buttons_.end()) {
        selected->second->ScrollRectToVisible(
            selected->second->GetLocalBounds());
      }
    }
  }

 private:
  raw_ptr<MirrorActions> actions_;
  raw_ptr<views::ScrollView> scroll_ = nullptr;
  raw_ptr<views::View> contents_ = nullptr;
  raw_ptr<views::LabelButton> new_tab_ = nullptr;
  std::map<std::string, raw_ptr<NoventraMirrorTabButton>> buttons_;
  std::vector<Row> rows_;
  bool vertical_ = true;
  bool reveal_selection_ = false;
  std::string selected_;
  gfx::Size last_size_;
  int row_height_ = 36;
};
BEGIN_METADATA(NoventraMirrorTabStrip)
END_METADATA

// A remote preview must not leave a physically owned fallback page editable
// behind its opaque cover. Track Views rather than WebContents across moves.
class MirrorContentsInputGuard {
 public:
  explicit MirrorContentsInputGuard(views::View* view)
      : enabled_(view->GetEnabled()),
        events_(view->GetCanProcessEventsWithinSubtree()),
        leaf_(view->GetViewAccessibility().views::ViewAccessibility::IsLeaf()),
        ignored_(view->GetViewAccessibility().GetIsIgnored()) {
    view_.SetView(view);
    view->SetEnabled(false);
    view->SetCanProcessEventsWithinSubtree(false);
    view->GetViewAccessibility().SetIsLeaf(true);
    view->GetViewAccessibility().SetIsIgnored(true);
  }
  ~MirrorContentsInputGuard() {
    if (view_) {
      view_.view()->SetEnabled(enabled_);
      view_.view()->SetCanProcessEventsWithinSubtree(events_);
      view_.view()->GetViewAccessibility().SetIsLeaf(leaf_);
      view_.view()->GetViewAccessibility().SetIsIgnored(ignored_);
    }
  }
  const views::View* view() const { return view_.view(); }

 private:
  views::ViewTracker view_;
  bool enabled_, events_, leaf_, ignored_;
};

class NoventraMirrorSnapshotView : public views::View {
  METADATA_HEADER(NoventraMirrorSnapshotView, views::View)
 public:
  explicit NoventraMirrorSnapshotView(base::RepeatingClosure show_here) {
    SetProperty(views::kViewIgnoredByLayoutKey, true);
    SetPaintToLayer();
    layer()->SetFillsBoundsOpaquely(true);
    SetFocusBehavior(FocusBehavior::ALWAYS);
    GetViewAccessibility().SetName(u"Tab preview from another window");
    card_ = AddChildView(std::make_unique<views::View>());
    auto* layout = card_->SetLayoutManager(std::make_unique<views::BoxLayout>(
        views::BoxLayout::Orientation::kVertical, gfx::Insets(16), 10));
    layout->set_cross_axis_alignment(
        views::BoxLayout::CrossAxisAlignment::kCenter);
    card_->SetBackground(views::CreateRoundedRectBackground(kColorToolbar, 10));
    auto* message = card_->AddChildView(
        std::make_unique<views::Label>(u"This tab is open in another window"));
    message->SetMultiLine(true);
    card_->AddChildView(std::make_unique<views::MdTextButton>(
        std::move(show_here), u"Show here"));
  }
  bool OnMousePressed(const ui::MouseEvent&) override {
    RequestFocus();
    return true;
  }
  bool OnKeyPressed(const ui::KeyEvent&) override { return true; }
  void SetImage(gfx::ImageSkia image) {
    image_ = std::move(image);
    SchedulePaint();
  }
  void Layout(PassKey) override {
    const int card_width = std::min(380, std::max(0, width() - 24));
    const int card_height = card_->GetHeightForWidth(card_width);
    card_->SetBounds((width() - card_width) / 2,
                     std::max(0, (height() - card_height) / 2), card_width,
                     card_height);
  }
  void OnPaint(gfx::Canvas* canvas) override {
    canvas->FillRect(
        GetLocalBounds(),
        SkColorSetA(GetColorProvider()->GetColor(kColorToolbar), 255));
    if (!image_.isNull() && image_.width() > 0 && image_.height() > 0) {
      const float scale =
          std::min(static_cast<float>(width()) / image_.width(),
                   static_cast<float>(height()) / image_.height());
      canvas->Save();
      canvas->Translate(
          gfx::Vector2d((width() - image_.width() * scale) / 2,
                        (height() - image_.height() * scale) / 2));
      canvas->Scale(scale, scale);
      canvas->DrawImageInt(image_, 0, 0);
      canvas->Restore();
      canvas->FillRect(GetLocalBounds(), SkColorSetARGB(100, 0, 0, 0));
    }
    views::View::OnPaint(canvas);
  }

 private:
  raw_ptr<views::View> card_ = nullptr;
  gfx::ImageSkia image_;
};
BEGIN_METADATA(NoventraMirrorSnapshotView)
END_METADATA
}  // namespace

class NoventraWindowMirrorPresentation::Impl : public MirrorActions {
 public:
  explicit Impl(BrowserView* view) : view_(view) {
    prefs_.Init(view_->browser()->GetProfile()->GetPrefs());
    prefs_.Add(noventra::kWindowMirroring,
               base::BindRepeating(&Impl::QueueRefresh, weak_.GetWeakPtr()));
    if (auto* service = Service()) {
      changed_ = service->RegisterChanged(
          base::BindRepeating(&Impl::QueueRefresh, weak_.GetWeakPtr()));
    }
    QueueRefresh();
  }
  ~Impl() override {
    weak_.InvalidateWeakPtrs();
    input_guards_.clear();
    RestoreNativeStrip();
    RemoveProjection();
    if (snapshot_) {
      snapshot_.view()->parent()->RemoveChildViewT(snapshot_.view());
    }
  }

  noventra::WindowMirrorService* Service() const {
    return noventra::WindowMirrorService::GetForProfile(
        view_->browser()->GetProfile());
  }
  bool Enabled() const {
    auto* service = Service();
    return service && service->IsEnabled() &&
           service->IsEnrolled(view_->browser());
  }
  NoventraMirrorTabStrip* Strip() const {
    return static_cast<NoventraMirrorTabStrip*>(
        const_cast<views::View*>(strip_.view()));
  }
  void QueueRefresh() {
    if (refresh_pending_) {
      return;
    }
    refresh_pending_ = true;
    base::SingleThreadTaskRunner::GetCurrentDefault()->PostTask(
        FROM_HERE, base::BindOnce(&Impl::Refresh, weak_.GetWeakPtr()));
  }
  void Refresh() {
    refresh_pending_ = false;
    view_->RefreshNoventraSpaceTheme();
    auto* service = Service();
    if (!service || !service->ObserveWindow(view_->browser()) || !Enabled()) {
      RestoreNativeStrip();
      RemoveProjection();
      input_guards_.clear();
      snapshot_showing_ = false;
      if (snapshot_) {
        static_cast<NoventraMirrorSnapshotView*>(snapshot_.view())
            ->SetImage({});
        snapshot_.view()->SetVisible(false);
      }
      return;
    }
    if (Strip() && Strip()->held()) {
      // Do not remove/rebuild the view holding native mouse capture. Logical
      // IDs are resolved again by the eventual action, after input cleanup.
      return;
    }
    Layout();
    if (auto* strip = Strip()) {
      const int density =
          view_->browser()->GetProfile()->GetPrefs()->GetInteger(
              "noventra.browser.tab_density");
      strip->Update(service->Rows(view_->browser()),
                    view_->ShouldDrawVerticalTabStrip(), density ? 44 : 36);
    }
    UpdateSnapshot();
  }

  void Layout() {
    if (!Enabled() || !view_->tab_strip_view()) {
      return;
    }
    auto* native = view_->tab_strip_view()->GetTabStripView();
    if (!native || !native->parent()) {
      return;
    }
    if (native_.view() != native || !strip_ ||
        strip_.view()->parent() != native->parent()) {
      RestoreNativeStrip();
      RemoveProjection();
      native_.SetView(native);
      auto* strip = native->parent()->AddChildView(
          std::make_unique<NoventraMirrorTabStrip>(
              this, base::BindRepeating(&Impl::NewTab, weak_.GetWeakPtr())));
      strip_.SetView(strip);
      native_enabled_ = native->GetEnabled();
      native_events_ = native->GetCanProcessEventsWithinSubtree();
      native_ax_leaf_ =
          native->GetViewAccessibility().views::ViewAccessibility::IsLeaf();
      native_ax_ignored_ = native->GetViewAccessibility().GetIsIgnored();
      native_had_layer_ = native->layer() != nullptr;
      native_opacity_ = native_had_layer_ ? native->layer()->opacity() : 1.0f;
      if (!native_had_layer_) {
        native->SetPaintToLayer();
        native->layer()->SetFillsBoundsOpaquely(false);
      }
      native->layer()->SetOpacity(0);
      native->SetCanProcessEventsWithinSubtree(false);
      // Disabled-in-subtree suppresses every native descendant keyboard stop.
      // Pruning also prevents duplicate invisible tabs in accessibility trees.
      native->SetEnabled(false);
      native->GetViewAccessibility().SetIsLeaf(true);
      native->GetViewAccessibility().SetIsIgnored(true);
      QueueRefresh();
    }
    if (view_->ShouldDrawVerticalTabStrip()) {
      auto* region = views::AsViewClass<VerticalTabStripRegionView>(
          view_->tab_strip_view());
      if (region && region->GetBottomContainer()) {
        auto* button = region->GetBottomContainer();
        native_new_tab_.SetView(button);
        button->SetVisible(false);
      }
    }
    gfx::Rect bounds = native->bounds();
    auto* parent = native->parent();
    if (view_->ShouldDrawVerticalTabStrip()) {
      int bottom = parent->height();
      for (const auto& child : parent->children()) {
        if (child != native && child != strip_.view() && child->GetVisible() &&
            child->height() > 0 && child->y() >= bounds.y()) {
          bottom = std::min(bottom, child->y());
        }
      }
      bounds.set_height(std::max(bounds.height(), bottom - bounds.y() - 4));
    } else {
      bounds.set_width(std::max(bounds.width(), parent->width() - bounds.x()));
      bounds.set_height(std::max(bounds.height(), parent->height()));
    }
    strip_.view()->SetBoundsRect(bounds);
    parent->ReorderChildView(strip_.view(), parent->children().size() - 1);
    if (snapshot_) {
      snapshot_.view()->SetBoundsRect(
          view_->contents_container()->GetLocalBounds());
    }
  }

  bool Contains(const views::View* source, const gfx::Point& point) const {
    if (!Enabled() || !strip_ || !strip_.view()->IsDrawn()) {
      return false;
    }
    auto local = point;
    views::View::ConvertPointToTarget(source, strip_.view(), &local);
    return strip_.view()->GetLocalBounds().Contains(local);
  }

  void Select(const std::string& id, bool edit_address = false) override {
    Post(base::BindOnce(
        [](Impl* self, std::string id, bool edit_address) {
          if (auto* service = self->Service()) {
            if (service->Select(self->view_->browser(), id) && edit_address &&
                self->view_->toolbar() &&
                self->view_->toolbar()->UsesNoventraInlineAddress()) {
              chrome::FocusLocationBar(self->view_->browser());
            }
          }
          self->QueueRefresh();
        },
        base::Unretained(this), id, edit_address));
  }
  void Close(const std::string& id) override {
    Post(base::BindOnce(
        [](Impl* self, std::string id) {
          if (auto* service = self->Service()) {
            service->Close(self->view_->browser(), id);
          }
        },
        base::Unretained(this), id));
  }
  void Context(const std::string& id, const gfx::Point& screen) override {
    Post(base::BindOnce(
        [](Impl* self, std::string id, gfx::Point screen) {
          auto* service = self->Service();
          if (!service || !service->Select(self->view_->browser(), id)) {
            return;
          }
          const auto rows = service->Rows(self->view_->browser());
          auto it = std::ranges::find(rows, id, &Row::id);
          if (it == rows.end() || !it->handle.Get()) {
            return;
          }
          auto* region = self->view_->tab_strip_view();
          auto* anchor =
              region ? region->GetTabAnchorView(it->handle) : nullptr;
          if (anchor && anchor->context_menu_controller()) {
            anchor->context_menu_controller()->ShowContextMenuForView(
                anchor, screen, ui::mojom::MenuSourceType::kMouse);
          }
        },
        base::Unretained(this), id, screen));
  }
  void Drop(const std::string& id, const gfx::Point& screen) override {
    std::string target = Strip() ? Strip()->RowAt(screen) : std::string();
    const auto content = view_->contents_container()->GetBoundsInScreen();
    const bool in_content = content.Contains(screen);
    auto layout = split_tabs::SplitTabLayout::kSideBySide;
    if (in_content && (screen.y() < content.y() + content.height() / 4 ||
                       screen.y() > content.bottom() - content.height() / 4)) {
      layout = split_tabs::SplitTabLayout::kStacked;
    }
    Post(base::BindOnce(
        [](Impl* self, std::string id, std::string target, bool content,
           split_tabs::SplitTabLayout layout, gfx::Point screen) {
          auto* service = self->Service();
          if (!service) {
            return;
          }
          if (content) {
            target = service->Selected(self->view_->browser());
            if (target != id) {
              service->DropToSplit(self->view_->browser(), id, target, layout);
            }
          } else if (!target.empty() && target != id) {
            service->MoveLogical(self->view_->browser(), id, target);
          } else if (!self->view_->GetWidget()
                          ->GetWindowBoundsInScreen()
                          .Contains(screen)) {
            self->NewWindowWithTab(id);
          } else {
            service->MoveLogical(self->view_->browser(), id, {});
          }
          self->QueueRefresh();
        },
        base::Unretained(this), id, target, in_content, layout, screen));
  }
  void CaptureEnded() override { QueueRefresh(); }
  void OnActivated(bool active) {
    if (auto* service = Service()) {
      service->ObserveWindow(view_->browser());
      service->OnActivated(view_->browser(), active);
    }
    QueueRefresh();
  }

 private:
  void Post(base::OnceClosure action) {
    // The closure's raw self is dereferenced only behind this weak guard.
    base::SingleThreadTaskRunner::GetCurrentDefault()->PostTask(
        FROM_HERE, base::BindOnce(
                       [](base::WeakPtr<Impl> self, base::OnceClosure action) {
                         if (self && self->Enabled()) {
                           std::move(action).Run();
                         }
                       },
                       weak_.GetWeakPtr(), std::move(action)));
  }
  void NewTab() {
    Post(base::BindOnce(
        [](Impl* self) {
          auto* service = self->Service();
          if (!service) {
            return;
          }
          const auto selected = service->Selected(self->view_->browser());
          if (!selected.empty() &&
              !service->Select(self->view_->browser(), selected)) {
            return;
          }
          if (!NoventraDockController::ShowNewTabPopup(self->view_)) {
            chrome::NewTab(self->view_->browser(), NewTabTypes::kNewTabButton);
          }
        },
        base::Unretained(this)));
  }
  void NewWindowWithTab(const std::string& id) {
    auto* service = Service();
    if (!service || view_->browser()->GetProfile()->ShutdownStarted()) {
      return;
    }
    const auto rows = service->Rows(view_->browser());
    auto row = std::ranges::find(rows, id, &Row::id);
    if (row == rows.end() || !row->handle.Get()) {
      return;
    }
    BrowserWindowCreateParams params(BrowserWindowInterface::TYPE_NORMAL,
                                     view_->browser()->GetProfile(), true);
    params.should_trigger_session_restore = false;
    auto* browser = CreateBrowserWindow(std::move(params));
    if (!browser) {
      return;
    }
    if (!service->ObserveWindow(browser) || !service->Select(browser, id)) {
      browser->GetWindow()->Close();
      return;
    }
    browser->GetWindow()->Show();
  }
  void UpdateSnapshot() {
    auto* service = Service();
    const bool show = service && service->IsSelectedRemote(view_->browser());
    if (!show) {
      input_guards_.clear();
      snapshot_showing_ = false;
      if (snapshot_) {
        static_cast<NoventraMirrorSnapshotView*>(snapshot_.view())
            ->SetImage({});
        snapshot_.view()->SetVisible(false);
      }
      return;
    }
    if (!snapshot_) {
      snapshot_.SetView(view_->contents_container()->AddChildView(
          std::make_unique<NoventraMirrorSnapshotView>(
              base::BindRepeating(&Impl::ShowSelected, weak_.GetWeakPtr()))));
    }
    auto* snapshot = static_cast<NoventraMirrorSnapshotView*>(snapshot_.view());
    const auto image = service->GetSnapshot(view_->browser());
    snapshot->SetImage(
        image && image->id == service->Selected(view_->browser()) &&
                image->space == service->SelectedSpace(view_->browser())
            ? image->image
            : gfx::ImageSkia());
    snapshot->SetBoundsRect(view_->contents_container()->GetLocalBounds());
    snapshot->SetVisible(true);
    for (const auto& child : snapshot->parent()->children()) {
      if (child == snapshot ||
          std::ranges::any_of(input_guards_, [&](const auto& guard) {
            return guard->view() == child;
          })) {
        continue;
      }
      input_guards_.push_back(
          std::make_unique<MirrorContentsInputGuard>(child));
    }
    if (!snapshot_showing_ && snapshot->GetWidget()->IsActive()) {
      snapshot->RequestFocus();
    }
    snapshot_showing_ = true;
    snapshot->parent()->ReorderChildView(
        snapshot, snapshot->parent()->children().size() - 1);
  }
  void ShowSelected() {
    if (auto* service = Service()) {
      Select(service->Selected(view_->browser()));
    }
  }
  void RestoreNativeStrip() {
    if (native_new_tab_) {
      native_new_tab_.view()->SetVisible(
          !view_->noventra_dock_controller() ||
          !view_->noventra_dock_controller()->active());
      native_new_tab_.SetView(nullptr);
    }
    if (!native_) {
      return;
    }
    auto* native = native_.view();
    native->SetCanProcessEventsWithinSubtree(native_events_);
    native->SetEnabled(native_enabled_);
    native->GetViewAccessibility().SetIsLeaf(native_ax_leaf_);
    native->GetViewAccessibility().SetIsIgnored(native_ax_ignored_);
    if (native->layer()) {
      native->layer()->SetOpacity(native_opacity_);
      if (!native_had_layer_) {
        native->DestroyLayer();
      }
    }
    native_.SetView(nullptr);
  }
  void RemoveProjection() {
    if (strip_) {
      strip_.view()->parent()->RemoveChildViewT(strip_.view());
    }
  }

  raw_ptr<BrowserView> view_;
  PrefChangeRegistrar prefs_;
  base::CallbackListSubscription changed_;
  views::ViewTracker native_;
  views::ViewTracker native_new_tab_;
  views::ViewTracker strip_;
  views::ViewTracker snapshot_;
  std::vector<std::unique_ptr<MirrorContentsInputGuard>> input_guards_;
  bool snapshot_showing_ = false;
  bool native_had_layer_ = false;
  bool native_enabled_ = true;
  bool native_events_ = true;
  bool native_ax_leaf_ = false;
  bool native_ax_ignored_ = false;
  float native_opacity_ = 1.0f;
  bool refresh_pending_ = false;
  base::WeakPtrFactory<Impl> weak_{this};
};

NoventraWindowMirrorPresentation::NoventraWindowMirrorPresentation(
    BrowserView* view)
    : impl_(std::make_unique<Impl>(view)) {}
NoventraWindowMirrorPresentation::~NoventraWindowMirrorPresentation() = default;
void NoventraWindowMirrorPresentation::Layout() {
  impl_->Layout();
}
void NoventraWindowMirrorPresentation::Refresh() {
  impl_->Refresh();
}
views::View* NoventraWindowMirrorPresentation::GetDefaultFocusableChild()
    const {
  auto* strip = impl_->Enabled() ? impl_->Strip() : nullptr;
  return strip && strip->IsDrawn() ? strip->DefaultFocusable() : nullptr;
}
void NoventraWindowMirrorPresentation::OnActivationChanged(bool active) {
  impl_->OnActivated(active);
}
bool NoventraWindowMirrorPresentation::ContainsTabStripPoint(
    const views::View* source,
    const gfx::Point& point) const {
  return impl_->Contains(source, point);
}

bool NoventraWindowMirrorPresentation::HandleTabCommand(
    BrowserWindowInterface* browser,
    int command) {
  auto* service =
      noventra::WindowMirrorService::GetForProfile(browser->GetProfile());
  if (!service || !service->IsEnrolled(browser) || !service->IsEnabled()) {
    return false;
  }
  const auto rows = service->Rows(browser);
  if (rows.empty()) {
    return false;
  }
  const auto selected = service->Selected(browser);
  auto it = std::ranges::find(rows, selected, &Row::id);
  const int index = it == rows.end() ? 0 : it - rows.begin();
  int next = -1;
  if (command == IDC_SELECT_NEXT_TAB) {
    next = (index + 1) % rows.size();
  } else if (command == IDC_SELECT_PREVIOUS_TAB) {
    next = (index + rows.size() - 1) % rows.size();
  } else if (command >= IDC_SELECT_TAB_0 && command <= IDC_SELECT_TAB_7) {
    next = command - IDC_SELECT_TAB_0;
  } else if (command == IDC_SELECT_LAST_TAB) {
    next = rows.size() - 1;
  } else if (command == IDC_CLOSE_TAB) {
    service->Close(browser, selected);
    return true;
  }
  if (next >= 0) {
    if (next < static_cast<int>(rows.size())) {
      service->Select(browser, rows[next].id);
    }
    return true;
  }
  // A remote shell has no physical active tab. Claim its selected real page
  // before ordinary address/navigation/browser commands reach model guards.
  if (service->IsSelectedRemote(browser) && command != IDC_CLOSE_WINDOW &&
      command != IDC_EXIT && !service->Select(browser, selected)) {
    return true;
  }
  return false;
}

bool NoventraWindowMirrorPresentation::CreateMirroredWindow(
    BrowserWindowInterface* source) {
  auto* service =
      noventra::WindowMirrorService::GetForProfile(source->GetProfile());
  if (!service || !service->IsEnabled() || !service->IsEnrolled(source)) {
    return false;
  }
  const auto id = service->Selected(source);
  const auto rows = service->Rows(source);
  auto selected = std::ranges::find(rows, id, &Row::id);
  if (selected == rows.end() || !selected->handle.Get()) {
    return false;
  }
  BrowserWindowCreateParams params(BrowserWindowInterface::TYPE_NORMAL,
                                   source->GetProfile(), true);
  params.should_trigger_session_restore = false;
  auto* browser = CreateBrowserWindow(std::move(params));
  if (!browser) {
    return true;
  }
  if (!service->ObserveWindow(browser) || !service->Select(browser, id)) {
    browser->GetWindow()->Close();
    return true;
  }
  browser->GetWindow()->Show();
  return true;
}
