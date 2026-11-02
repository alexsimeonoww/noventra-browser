// Copyright 2026 The Noventra Authors
// SPDX-License-Identifier: GPL-3.0-only
#include "chrome/browser/ui/views/frame/noventra_bookmarks_view.h"

#include <algorithm>
#include <optional>
#include <set>
#include <string>

#include "base/functional/bind.h"
#include "base/i18n/rtl.h"
#include "base/memory/ptr_util.h"
#include "base/strings/string_util.h"
#include "base/strings/utf_string_conversions.h"
#include "base/task/single_thread_task_runner.h"
#include "chrome/app/chrome_command_ids.h"
#include "chrome/app/vector_icons/vector_icons.h"
#include "chrome/browser/bookmarks/bookmark_merged_surface_service_factory.h"
#include "chrome/browser/bookmarks/bookmark_model_factory.h"
#include "chrome/browser/bookmarks/bookmark_parent_folder.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/ui/bookmarks/bookmark_context_menu_controller.h"
#include "chrome/browser/ui/bookmarks/bookmark_editor.h"
#include "chrome/browser/ui/bookmarks/bookmark_ui_operations_helper.h"
#include "chrome/browser/ui/bookmarks/bookmark_utils.h"
#include "chrome/browser/ui/browser_commands.h"
#include "chrome/browser/ui/color/chrome_color_id.h"
#include "chrome/browser/ui/navigator/browser_navigator.h"
#include "chrome/browser/ui/navigator/browser_navigator_params.h"
#include "chrome/browser/ui/side_panel/side_panel_ui.h"
#include "chrome/browser/ui/views/bookmarks/bookmark_context_menu.h"
#include "chrome/browser/ui/views/bookmarks/bookmark_menu_delegate.h"
#include "chrome/browser/ui/views/frame/browser_view.h"
#include "chrome/browser/ui/views/frame/noventra_dock_view.h"
#include "chrome/common/pref_names.h"
#include "components/bookmarks/browser/bookmark_utils.h"
#include "components/bookmarks/common/bookmark_pref_names.h"
#include "components/prefs/pref_service.h"
#include "components/prefs/scoped_user_pref_update.h"
#include "components/vector_icons/vector_icons.h"
#include "ui/base/dragdrop/mojom/drag_drop_types.mojom.h"
#include "ui/base/menu_source_utils.h"
#include "ui/base/metadata/metadata_impl_macros.h"
#include "ui/base/models/image_model.h"
#include "ui/base/models/tree_node_iterator.h"
#include "ui/base/mojom/dialog_button.mojom.h"
#include "ui/base/mojom/menu_source_type.mojom.h"
#include "ui/base/window_open_disposition_utils.h"
#include "ui/color/color_provider.h"
#include "ui/events/keycodes/keyboard_codes.h"
#include "ui/gfx/canvas.h"
#include "ui/gfx/geometry/vector2d.h"
#include "ui/gfx/paint_vector_icon.h"
#include "ui/views/accessibility/view_accessibility.h"
#include "ui/views/bubble/bubble_dialog_delegate_view.h"
#include "ui/views/controls/button/md_text_button.h"
#include "ui/views/controls/button/menu_button_controller.h"
#include "ui/views/controls/label.h"
#include "ui/views/controls/menu/menu_item_view.h"
#include "ui/views/controls/menu/menu_runner.h"
#include "ui/views/controls/scroll_view.h"
#include "ui/views/controls/textfield/textfield.h"
#include "ui/views/controls/textfield/textfield_controller.h"
#include "ui/views/focus/focus_manager.h"
#include "ui/views/layout/box_layout.h"
#include "ui/views/style/typography.h"
#include "ui/views/widget/widget.h"

namespace {
constexpr int kBookmarkRowHeight = 32;
constexpr int kBookmarkViewportHeight = 6 * kBookmarkRowHeight;
constexpr int kPopupBookmarkRowHeight = 36;
constexpr int kPopupBookmarkViewportHeight = 9 * kPopupBookmarkRowHeight;
constexpr int kMinimumLibraryWidth = 200;
constexpr size_t kMaximumPopupRows = 100;
constexpr int kInlineHeadingHeight = 24;

// Keep the native button, focus ring and context-menu target. A separate
// disclosure icon leaves the saved page/folder title free of text glyphs.
class BookmarkRowButton : public views::MdTextButton {
  METADATA_HEADER(BookmarkRowButton, views::MdTextButton)

 public:
  using views::MdTextButton::MdTextButton;

  void SetDisclosure(bool visible, bool expanded) {
    if (disclosure_visible_ == visible && expanded_ == expanded) {
      return;
    }
    disclosure_visible_ = visible;
    expanded_ = expanded;
    SchedulePaint();
  }

  void PaintButtonContents(gfx::Canvas* canvas) override {
    views::MdTextButton::PaintButtonContents(canvas);
    if (!disclosure_visible_) {
      return;
    }
    constexpr int kIconSize = 12;
    canvas->Save();
    canvas->Translate(
        gfx::Vector2d(GetMirroredXWithWidthInView(width() - 22, kIconSize),
                      (height() - kIconSize) / 2));
    if (!expanded_ && base::i18n::IsRTL()) {
      canvas->Translate(gfx::Vector2d(kIconSize, 0));
      canvas->Scale(-1, 1);
    }
    gfx::PaintVectorIcon(
        canvas,
        expanded_ ? vector_icons::kExpandMoreOldIcon : kChevronRightIcon,
        kIconSize,
        GetColorProvider()->GetColor(ui::kColorLabelForegroundSecondary));
    canvas->Restore();
  }

 private:
  bool disclosure_visible_ = false;
  bool expanded_ = false;
};

BEGIN_METADATA(BookmarkRowButton)
END_METADATA

void SetFlatBackground(views::MdTextButton* button, bool flat) {
  // MdTextButton's two override forms are mutually exclusive.
  if (flat) {
    if (button->GetBgColorIdOverride()) {
      button->SetBgColorIdOverride(std::nullopt);
    }
    button->SetBgColorOverrideDeprecated(SK_ColorTRANSPARENT);
  } else {
    if (button->GetBgColorOverrideDeprecated()) {
      button->SetBgColorOverrideDeprecated(std::nullopt);
    }
    button->SetBgColorIdOverride(kColorToolbar);
  }
}

bool IsLibraryCommand(int id) {
  return id == IDC_BOOKMARK_THIS_TAB || id == IDC_SHOW_BOOKMARK_MANAGER ||
         id == IDC_SHOW_BOOKMARK_SIDE_PANEL;
}

// Imported titles sometimes contain the complete URL. Keep those model
// values intact, but show a quiet hostname instead of a long address in a row.
std::u16string BookmarkDisplayTitle(const bookmarks::BookmarkNode& node) {
  const auto& title = node.GetTitle();
  if (node.is_folder()) {
    return title.empty() ? u"Bookmark folder" : title;
  }
  const std::string text = base::UTF16ToUTF8(title);
  const bool address_title =
      title.empty() ||
      base::StartsWith(text, "https://",
                       base::CompareCase::INSENSITIVE_ASCII) ||
      base::StartsWith(text, "http://", base::CompareCase::INSENSITIVE_ASCII);
  if (!address_title || node.url().host().empty()) {
    return title.empty() ? base::UTF8ToUTF16(node.url().spec()) : title;
  }
  std::string host(node.url().host());
  if (base::StartsWith(host, "www.")) {
    host.erase(0, 4);
  }
  return base::UTF8ToUTF16(host);
}

ui::ImageModel BookmarkIcon(bookmarks::BookmarkModel& model,
                            const bookmarks::BookmarkNode& node) {
  if (node.is_url()) {
    const auto& favicon = model.GetFavicon(&node);
    if (!favicon.IsEmpty()) {
      return ui::ImageModel::FromImageSkia(favicon.AsImageSkia());
    }
  }
  return ui::ImageModel::FromVectorIcon(
      node.is_folder() ? vector_icons::kFolderFlippableIcon
                       : vector_icons::kGlobeIcon,
      kColorToolbarButtonIcon, 16);
}
}  // namespace

// Forward to Chromium's bookmark delegate: folders, middle-click, native
// context menus and changes while a menu is open retain the existing browser
// behavior.
class NoventraBookmarksView::FolderMenu : public views::MenuDelegate {
 public:
  FolderMenu(BrowserView& browser,
             std::optional<BookmarkParentFolder> folder,
             base::RepeatingClosure on_closed)
      : browser_(&browser),
        on_closed_(std::move(on_closed)),
        delegate_(browser.browser(),
                  browser.GetWidget(),
                  this,
                  BookmarkLaunchLocation::kSidePanelFolder) {
    std::unique_ptr<views::MenuItemView> root;
    if (folder) {
      delegate_.SetActiveMenu(*folder, 0);
      root = base::WrapUnique(delegate_.menu());
    } else {
      root = std::make_unique<views::MenuItemView>(this);
      root->CreateSubmenu();
      delegate_.BuildFullMenu(root.get(), /*show_bookmarks_title=*/false);
    }
    runner_ = std::make_unique<views::MenuRunner>(std::move(root), 0);
  }
  void Run(views::View& anchor, ui::mojom::MenuSourceType source_type) {
    const auto bounds = anchor.GetBoundsInScreen();
    // Align toward the window interior on either side. MenuRunner interprets
    // anchors logically and mirrors them in RTL; this decision is physical.
    const bool right_half =
        bounds.CenterPoint().x() >
        anchor.GetWidget()->GetWindowBoundsInScreen().CenterPoint().x();
    const auto position = right_half != base::i18n::IsRTL()
                              ? views::MenuAnchorPosition::kTopRight
                              : views::MenuAnchorPosition::kTopLeft;
    runner_->RunMenuAt(anchor.GetWidget(), nullptr, bounds, position,
                       source_type);
  }
  void CancelForModelChange() {
    // Native context-menu deletion updates the delegate's mappings itself.
    // External edits must cancel before those mappings can be used again.
    if (!delegate_.is_mutating_model()) {
      runner_->Cancel();
    }
  }
  void OnMenuClosed(views::MenuItemView*) override {
    // The callback destroys this wrapper, including the delegate's node maps.
    auto on_closed = on_closed_;
    on_closed.Run();
  }
  bool GetDropFormats(views::MenuItemView* menu,
                      int* formats,
                      std::set<ui::ClipboardFormatType>* types) override {
    if (IsLibraryCommand(menu->GetCommand())) {
      return false;
    }
    return delegate_.GetDropFormats(menu, formats, types);
  }
  bool AreDropTypesRequired(views::MenuItemView* menu) override {
    if (IsLibraryCommand(menu->GetCommand())) {
      return false;
    }
    return delegate_.AreDropTypesRequired(menu);
  }
  bool CanDrop(views::MenuItemView* menu,
               const ui::OSExchangeData& data) override {
    if (IsLibraryCommand(menu->GetCommand())) {
      return false;
    }
    return delegate_.CanDrop(menu, data);
  }
  ui::mojom::DragOperation GetDropOperation(views::MenuItemView* menu,
                                            const ui::DropTargetEvent& event,
                                            DropPosition* position) override {
    if (IsLibraryCommand(menu->GetCommand())) {
      *position = DropPosition::kNone;
      return ui::mojom::DragOperation::kNone;
    }
    return delegate_.GetDropOperation(menu, event, position);
  }
  views::View::DropCallback GetDropCallback(
      views::MenuItemView* menu,
      DropPosition position,
      const ui::DropTargetEvent& event) override {
    if (IsLibraryCommand(menu->GetCommand())) {
      return {};
    }
    return delegate_.GetDropCallback(menu, position, event);
  }
  bool CanDrag(views::MenuItemView* menu) override {
    if (IsLibraryCommand(menu->GetCommand())) {
      return false;
    }
    return delegate_.CanDrag(menu);
  }
  void WriteDragData(views::MenuItemView* menu,
                     ui::OSExchangeData* data) override {
    if (IsLibraryCommand(menu->GetCommand())) {
      return;
    }
    delegate_.WriteDragData(menu, data);
  }
  int GetDragOperations(views::MenuItemView* menu) override {
    if (IsLibraryCommand(menu->GetCommand())) {
      return 0;
    }
    return delegate_.GetDragOperations(menu);
  }
  bool ShouldCloseOnDragDropCompleted() override { return false; }
  int GetMaxWidthForMenu(views::MenuItemView* menu) override {
    return delegate_.GetMaxWidthForMenu(menu);
  }
  std::u16string GetTooltipText(int id,
                                const gfx::Point& point) const override {
    return delegate_.GetTooltipText(id, point);
  }
  bool IsTriggerableEvent(views::MenuItemView* item,
                          const ui::Event& event) override {
    return delegate_.IsTriggerableEvent(item, event);
  }
  void ExecuteCommand(int id, int flags) override {
    if (IsLibraryCommand(id)) {
      chrome::ExecuteCommand(browser_->browser(), id);
      return;
    }
    delegate_.ExecuteCommand(id, flags);
  }
  bool IsCommandEnabled(int id) const override {
    return !IsLibraryCommand(id) ||
           chrome::IsCommandEnabled(browser_->browser(), id);
  }
  bool ShouldExecuteCommandWithoutClosingMenu(int id,
                                              const ui::Event& event) override {
    if (IsLibraryCommand(id)) {
      return false;
    }
    return delegate_.ShouldExecuteCommandWithoutClosingMenu(id, event);
  }
  bool ShowContextMenu(views::MenuItemView* item,
                       int id,
                       const gfx::Point& point,
                       ui::mojom::MenuSourceType type) override {
    return delegate_.ShowContextMenu(item, id, point, type);
  }
  void WillShowMenu(views::MenuItemView* item) override {
    delegate_.WillShowMenu(item);
  }

 private:
  const raw_ptr<BrowserView> browser_;
  const base::RepeatingClosure on_closed_;
  BookmarkMenuDelegate delegate_;
  std::unique_ptr<views::MenuRunner> runner_;
};

// A small native overlay, not a dock slot or WebContents. Only stable IDs are
// retained across model notifications; actions resolve the current node again.
class NoventraBookmarksView::BookmarkPopup : public views::BubbleDialogDelegate,
                                             public views::TextfieldController {
 public:
  explicit BookmarkPopup(NoventraBookmarksView& owner)
      : views::BubbleDialogDelegate(owner.compact_button_,
                                    views::BubbleBorder::BOTTOM_LEFT),
        owner_(&owner),
        pressed_lock_(owner.popup_button_controller_->TakeLock()) {
    SetButtons(static_cast<int>(ui::mojom::DialogButton::kNone));
    SetTitle(u"Bookmarks");
    SetShowTitle(true);
    SetShowCloseButton(true);
    set_margins(gfx::Insets::TLBR(8, 10, 10, 10));
    set_corner_radius(12);
    set_close_on_deactivate(true);
    SetUseAnchorWindowBounds(true);

    auto contents = std::make_unique<views::View>();
    contents->GetViewAccessibility().SetName(u"Searchable bookmarks");
    contents->SetLayoutManager(std::make_unique<views::BoxLayout>(
        views::BoxLayout::Orientation::kVertical, gfx::Insets(), 6));
    search_ = contents->AddChildView(std::make_unique<views::Textfield>());
    search_->SetAccessibleName(u"Search bookmarks");
    search_->SetPlaceholderText(u"Search bookmarks");
    search_->SetDefaultWidthInChars(20);
    search_->SetCornerRadius(8);
    search_->SetTextColorId(kColorToolbarText);
    search_->SetBackgroundColor(kColorLocationBarBackground);
    search_->set_controller(this);
    SetInitiallyFocusedView(search_);

    back_ = contents->AddChildView(std::make_unique<views::MdTextButton>(
        base::BindRepeating(&BookmarkPopup::Back, weak_factory_.GetWeakPtr()),
        u"All bookmarks", views::style::CONTEXT_LABEL));
    back_->SetStyle(ui::ButtonStyle::kText);
    back_->SetHorizontalAlignment(gfx::ALIGN_TO_HEAD);
    back_->SetCustomPadding(gfx::Insets::VH(4, 4));
    back_->SetMinSize(gfx::Size(0, 28));
    scroll_ = contents->AddChildView(std::make_unique<views::ScrollView>());
    scroll_->SetBackgroundColor(std::nullopt);
    scroll_->SetHorizontalScrollBarMode(
        views::ScrollView::ScrollBarMode::kDisabled);
    rows_ = scroll_->SetContents(std::make_unique<views::View>());
    rows_->SetLayoutManager(std::make_unique<views::BoxLayout>(
        views::BoxLayout::Orientation::kVertical));
    empty_ = rows_->AddChildView(std::make_unique<views::Label>());
    empty_->SetMultiLine(true);
    empty_->SetHorizontalAlignment(gfx::ALIGN_TO_HEAD);
    empty_->SetPreferredSize(gfx::Size(0, 48));
    more_ = contents->AddChildView(std::make_unique<views::MdTextButton>(
        base::BindRepeating(&BookmarkPopup::Manage, weak_factory_.GetWeakPtr()),
        u"Open bookmark manager", views::style::CONTEXT_LABEL));
    more_->SetStyle(ui::ButtonStyle::kText);
    more_->SetHorizontalAlignment(gfx::ALIGN_TO_HEAD);
    more_->SetAccessibleName(u"Open bookmark manager");
    more_->SetTooltipText(
        u"First 100 results shown. Refine your search or open the bookmark "
        u"manager to see all bookmarks.");
    more_->SetCustomPadding(gfx::Insets::VH(6, 8));
    more_->SetCornerRadius(6);
    SetContentsView(std::move(contents));
    UpdateSizeLimits();
    Refresh();
  }

  void ScheduleRefresh() {
    if (!owner_ || pending_) {
      return;
    }
    pending_ = true;
    base::SingleThreadTaskRunner::GetCurrentDefault()->PostTask(
        FROM_HERE,
        base::BindOnce(&BookmarkPopup::Refresh, weak_factory_.GetWeakPtr()));
  }

  void PrepareToClose() {
    // Native deactivation observers continue running after the close callback.
    // Detach from the owner now, but keep this delegate and its widget alive
    // until that stack has unwound.
    weak_factory_.InvalidateWeakPtrs();
    set_close_on_deactivate(false);
    owner_ = nullptr;
    search_->set_controller(nullptr);
    pressed_lock_.reset();
    SetAnchorView(nullptr);
  }

  // Keep the whole popup inside the browser on either dock edge, including
  // short windows. The browser's page viewport never participates in layout.
  gfx::Rect GetBubbleBounds() override {
    UpdateSizeLimits();
    gfx::Rect bounds = views::BubbleDialogDelegate::GetBubbleBounds();
    if (auto* anchor = GetAnchorView(); anchor && anchor->GetWidget()) {
      auto window = anchor->GetWidget()->GetWindowBoundsInScreen();
      window.Inset(8);
      bounds.AdjustToFit(window);
    }
    return bounds;
  }

  void ContentsChanged(views::Textfield*, const std::u16string&) override {
    ScheduleRefresh();
  }

  bool HandleKeyEvent(views::Textfield*, const ui::KeyEvent& event) override {
    if (pending_) {
      Refresh();
    }
    if (!owner_) {
      return false;
    }
    if (event.type() == ui::EventType::kKeyPressed && !order_.empty() &&
        (event.key_code() == ui::VKEY_DOWN ||
         event.key_code() == ui::VKEY_RETURN)) {
      if (event.key_code() == ui::VKEY_DOWN) {
        buttons_.at(order_.front())->RequestFocus();
      } else {
        Open(order_.front(), event);
      }
      return true;
    }
    return false;
  }

 private:
  void UpdateSizeLimits() {
    if (!owner_ || !owner_->GetWidget()) {
      return;
    }
    const auto window = owner_->GetWidget()->GetWindowBoundsInScreen();
    set_fixed_width(std::max(1, std::min(400, window.width() - 40)));
    scroll_->ClipHeightTo(0, std::max(0, std::min(kPopupBookmarkViewportHeight,
                                                  window.height() - 204)));
  }

  void Back() {
    if (!owner_) {
      return;
    }
    const auto* model = owner_->model_.get();
    const auto* folder =
        model && folder_id_ ? bookmarks::GetBookmarkNodeByID(model, *folder_id_)
                            : nullptr;
    const auto* parent = folder ? folder->parent() : nullptr;
    folder_id_ = parent && !parent->is_permanent_node()
                     ? std::make_optional(parent->id())
                     : std::nullopt;
    search_->SetText(u"");
    ScheduleRefresh();
    search_->RequestFocus();
  }

  void Open(int64_t id, const ui::Event& event) {
    if (!owner_) {
      return;
    }
    auto* model = owner_->model_.get();
    const auto* node =
        model ? bookmarks::GetBookmarkNodeByID(model, id) : nullptr;
    if (!node) {
      ScheduleRefresh();
      return;
    }
    if (node->is_folder()) {
      folder_id_ = id;
      search_->SetText(u"");
      ScheduleRefresh();
      search_->RequestFocus();
      return;
    }
    auto owner = owner_->weak_factory_.GetWeakPtr();
    owner_->OpenNode(id, event);
    if (owner) {
      owner->ClosePopup();
    }
  }

  void Manage() {
    if (!owner_) {
      return;
    }
    auto owner = owner_->weak_factory_.GetWeakPtr();
    owner_->Manage();
    if (owner) {
      owner->ClosePopup();
    }
  }

  void Refresh() {
    pending_ = false;
    if (!owner_) {
      return;
    }
    auto* model = owner_->model_.get();
    const auto* folder =
        model && folder_id_ ? bookmarks::GetBookmarkNodeByID(model, *folder_id_)
                            : nullptr;
    if (!folder || !folder->is_folder()) {
      folder_id_.reset();
      folder = nullptr;
    }
    const std::u16string query(search_->GetText());
    std::vector<const bookmarks::BookmarkNode*> nodes;
    if (model && model->loaded()) {
      if (!query.empty()) {
        bookmarks::QueryFields fields;
        fields.word_phrase_query = std::make_unique<std::u16string>(query);
        const auto words = bookmarks::ParseBookmarkQuery(fields);
        ui::TreeNodeIterator<const bookmarks::BookmarkNode> iterator(
            model->root_node());
        while (!words.empty() && iterator.has_next() &&
               nodes.size() <= kMaximumPopupRows) {
          const auto* node = iterator.Next();
          bool visible = true;
          for (auto* ancestor = node; ancestor; ancestor = ancestor->parent()) {
            if (!ancestor->IsVisible()) {
              visible = false;
              break;
            }
          }
          if (visible && !node->is_permanent_node() &&
              bookmarks::DoesBookmarkContainWords(node->GetTitle(), node->url(),
                                                  words)) {
            nodes.push_back(node);
          }
        }
      } else if (folder) {
        for (const auto& child : folder->children()) {
          nodes.push_back(child.get());
          if (nodes.size() > kMaximumPopupRows) {
            break;
          }
        }
      } else {
        // Account and local permanent folders share the profile model. Flatten
        // their visible children just as the existing native library does.
        for (const auto& root : model->root_node()->children()) {
          if (root->IsVisible()) {
            for (const auto& child : root->children()) {
              nodes.push_back(child.get());
              if (nodes.size() > kMaximumPopupRows) {
                break;
              }
            }
          }
          if (nodes.size() > kMaximumPopupRows) {
            break;
          }
        }
      }
    }
    more_->SetText(nodes.size() > kMaximumPopupRows
                       ? u"More bookmarks — open bookmark manager"
                       : u"Open bookmark manager");
    more_->SetTooltipText(
        nodes.size() > kMaximumPopupRows
            ? u"First 100 results shown. Refine your search or "
              u"open the bookmark manager to see all bookmarks."
            : u"Organize, edit or remove saved bookmarks");
    if (nodes.size() > kMaximumPopupRows) {
      nodes.resize(kMaximumPopupRows);
    }
    back_->SetVisible(folder != nullptr || !query.empty());
    back_->SetText(folder ? BookmarkDisplayTitle(*folder) : u"Search results");
    back_->SetAccessibleName(folder ? u"Back to parent bookmark folder"
                                    : u"All bookmarks");
    back_->SetTooltipText(!query.empty() ? u"Search results from all bookmarks"
                          : folder       ? u"Back to parent bookmark folder"
                                         : u"Your saved pages and folders");
    back_->SetEnabled(folder != nullptr);
    back_->SetImageModel(
        views::Button::STATE_NORMAL,
        folder ? ui::ImageModel::FromVectorIcon(vector_icons::kArrowBackIcon,
                                                ui::kColorLabelForeground, 16)
               : ui::ImageModel());

    std::set<int64_t> present;
    order_.clear();
    for (const auto* node : nodes) {
      const auto id = node->id();
      present.insert(id);
      order_.push_back(id);
      if (!buttons_.contains(id)) {
        auto button = std::make_unique<BookmarkRowButton>(
            base::BindRepeating(&BookmarkPopup::Open,
                                weak_factory_.GetWeakPtr(), id),
            u"", views::style::CONTEXT_LABEL);
        button->SetStyle(ui::ButtonStyle::kText);
        button->SetBgColorOverrideDeprecated(SK_ColorTRANSPARENT);
        button->SetHorizontalAlignment(gfx::ALIGN_TO_HEAD);
        button->SetEnabledTextColors(kColorToolbarText);
        button->SetCustomPadding(gfx::Insets::VH(6, 8));
        button->SetMinSize(gfx::Size(0, 0));
        button->SetCornerRadius(6);
        button->SetPreferredSize(gfx::Size(0, kPopupBookmarkRowHeight));
        button->SetTriggerableEventFlags(ui::EF_LEFT_MOUSE_BUTTON |
                                         ui::EF_MIDDLE_MOUSE_BUTTON);
        buttons_[id] = rows_->AddChildView(std::move(button));
      }
      auto* button = buttons_.at(id).get();
      const auto title = BookmarkDisplayTitle(*node);
      static_cast<BookmarkRowButton*>(button)->SetDisclosure(
          node->is_folder(), false);
      button->SetCustomPadding(
          gfx::Insets::TLBR(6, 8, 6, node->is_folder() ? 28 : 8));
      button->SetText(title);
      button->SetAccessibleName(title);
      button->SetTooltipText(
          title + (node->is_url()
                       ? u"\n" + base::UTF8ToUTF16(node->url().spec())
                       : u"\nOpen bookmark folder"));
      button->SetImageModel(views::Button::STATE_NORMAL,
                            BookmarkIcon(*model, *node));
      rows_->ReorderChildView(button, order_.size() - 1);
    }
    for (auto it = buttons_.begin(); it != buttons_.end();) {
      if (present.contains(it->first)) {
        ++it;
      } else {
        if (it->second->HasFocus()) {
          search_->RequestFocus();
        }
        rows_->RemoveChildViewT(it->second.get());
        it = buttons_.erase(it);
      }
    }
    empty_->SetText(!model || !model->loaded() ? u"Loading bookmarks…"
                    : !query.empty()
                        ? u"No matching bookmarks"
                        : u"No bookmarks yet. Save a page with Ctrl+D.");
    empty_->SetVisible(nodes.empty());
    rows_->InvalidateLayout();
    if (GetWidget()) {
      SizeToContents();
    }
  }

  raw_ptr<NoventraBookmarksView> owner_;
  std::unique_ptr<views::MenuButtonController::PressedLock> pressed_lock_;
  raw_ptr<views::Textfield> search_ = nullptr;
  raw_ptr<views::MdTextButton> back_ = nullptr;
  raw_ptr<views::ScrollView> scroll_ = nullptr;
  raw_ptr<views::View> rows_ = nullptr;
  raw_ptr<views::Label> empty_ = nullptr;
  raw_ptr<views::MdTextButton> more_ = nullptr;
  std::map<int64_t, raw_ptr<views::MdTextButton>> buttons_;
  std::vector<int64_t> order_;
  std::optional<int64_t> folder_id_;
  bool pending_ = false;
  base::WeakPtrFactory<BookmarkPopup> weak_factory_{this};
};

NoventraBookmarksView::NoventraBookmarksView(BrowserView& browser,
                                             bool favorites,
                                             Presentation presentation)
    : browser_(&browser),
      favorites_(favorites),
      presentation_(presentation),
      model_(BookmarkModelFactory::GetForBrowserContext(browser.GetProfile())) {
  const auto title = favorites_ ? u"Favorites" : u"Bookmarks";
  GetViewAccessibility().SetRole(ax::mojom::Role::kGroup);
  GetViewAccessibility().SetName(
      presentation_ == Presentation::kPanelButton ? u"Bookmark popup"
      : presentation_ == Presentation::kButton    ? u"Bookmark menu"
      : favorites_ ? u"Favorite sites section"
                   : u"Bookmark library section");
  heading_ = AddChildView(std::make_unique<views::MdTextButton>(
      base::BindRepeating(&NoventraBookmarksView::Toggle,
                          base::Unretained(this)),
      title));
  heading_->SetStyle(ui::ButtonStyle::kText);
  heading_->SetBgColorIdOverride(kColorToolbar);
  heading_->SetMinSize(gfx::Size(0, 28));
  heading_->SetCustomPadding(gfx::Insets::VH(4, 6));
  heading_->SetHorizontalAlignment(gfx::ALIGN_TO_HEAD);
  heading_->SetEnabledTextColors(kColorToolbarText);
  heading_->SetAccessibleName(title);
  heading_->SetTooltipText(
      favorites_ ? u"Favorites from your bookmarks bar — collapse or expand"
                 : u"Your bookmark library — collapse or expand");
  if (!favorites_ && presentation_ == Presentation::kSection) {
    new_folder_ = AddChildView(std::make_unique<views::MdTextButton>(
        base::BindRepeating(&NoventraBookmarksView::NewFolder,
                            base::Unretained(this)),
        u""));
    new_folder_->SetStyle(ui::ButtonStyle::kText);
    new_folder_->SetBgColorOverrideDeprecated(SK_ColorTRANSPARENT);
    new_folder_->SetMinSize(gfx::Size(0, 0));
    new_folder_->SetCustomPadding(gfx::Insets(6));
    new_folder_->SetAccessibleName(u"New bookmark folder");
    new_folder_->SetTooltipText(u"New bookmark folder");
    new_folder_->SetImageModel(
        views::Button::STATE_NORMAL,
        ui::ImageModel::FromVectorIcon(kAddIcon, kColorToolbarButtonIcon, 16));
    scroll_ = AddChildView(std::make_unique<views::ScrollView>());
    scroll_->SetBackgroundColor(std::nullopt);
    scroll_->SetHorizontalScrollBarMode(
        views::ScrollView::ScrollBarMode::kDisabled);
    scroll_->ClipHeightTo(0, kBookmarkViewportHeight);
    rows_ = scroll_->SetContents(std::make_unique<views::View>());
    rows_->SetLayoutManager(std::make_unique<views::BoxLayout>(
        views::BoxLayout::Orientation::kVertical));
  }
  compact_button_ = AddChildView(std::make_unique<views::MdTextButton>(
      base::BindRepeating(&NoventraBookmarksView::ShowLibrary,
                          base::Unretained(this)),
      u""));
  compact_button_->SetStyle(ui::ButtonStyle::kText);
  // MdTextButton also derives its vector-icon color from its text color.
  // Match native chrome instead of inheriting the default accent color.
  compact_button_->SetEnabledTextColors(kColorToolbarText);
  if (presentation_ != Presentation::kSection || !favorites_) {
    // Match native toolbar buttons over glass. The ink drop and focus ring
    // continue to provide the themed hover, press and menu-anchor feedback.
    compact_button_->SetBgColorOverrideDeprecated(SK_ColorTRANSPARENT);
  } else {
    compact_button_->SetBgColorIdOverride(kColorToolbar);
  }
  compact_button_->SetMinSize(gfx::Size(0, 0));
  compact_button_->SetCustomPadding(
      gfx::Insets(presentation_ != Presentation::kSection ? 5 : 6));
  compact_button_->SetHorizontalAlignment(gfx::ALIGN_CENTER);
  compact_button_->SetAccessibleName(title);
  compact_button_->SetFocusBehavior(FocusBehavior::ALWAYS);
  if (UsesPopupButton()) {
    // Use Chromium's native popup-button click suppression so clicking the
    // anchor dismisses the bubble instead of immediately reopening it.
    compact_button_->SetCallback(views::Button::PressedCallback());
    auto controller = std::make_unique<views::MenuButtonController>(
        compact_button_,
        base::BindRepeating(&NoventraBookmarksView::ShowLibrary,
                            base::Unretained(this)),
        std::make_unique<views::Button::DefaultButtonControllerDelegate>(
            compact_button_));
    popup_button_controller_ = controller.get();
    compact_button_->SetButtonController(std::move(controller));
  }
  compact_button_->GetViewAccessibility().SetHasPopup(
      UsesPopupButton() ? ax::mojom::HasPopup::kDialog
                        : ax::mojom::HasPopup::kMenu);
  compact_button_->GetViewAccessibility().SetIsCollapsed();
  compact_button_->SetTooltipText(presentation_ == Presentation::kPanelButton
                                      ? u"Show or hide bookmarks"
                                  : favorites_ ? u"Favorites — bookmarks bar"
                                               : u"Bookmarks");
  compact_button_->SetImageModel(
      views::Button::STATE_NORMAL,
      ui::ImageModel::FromVectorIcon(
          UsesPopupButton() ? kBookmarksSidePanelRefreshOldIcon
          : favorites_      ? kStarIcon
                            : kKeyboardDoubleArrowRightIcon,
          kColorToolbarButtonIcon, favorites_ || UsesPopupButton() ? 18 : 16));
  manage_ = AddChildView(std::make_unique<views::MdTextButton>(
      base::BindRepeating(&NoventraBookmarksView::Manage,
                          base::Unretained(this)),
      favorites_ ? u"Manage favorites" : u"All bookmarks"));
  manage_->SetStyle(ui::ButtonStyle::kText);
  manage_->SetBgColorIdOverride(kColorToolbar);
  manage_->SetMinSize(gfx::Size(0, 28));
  manage_->SetCustomPadding(gfx::Insets::VH(4, 6));
  manage_->SetHorizontalAlignment(gfx::ALIGN_TO_HEAD);
  manage_->SetEnabledTextColors(kColorToolbarText);
  empty_hint_ = AddChildView(std::make_unique<views::Label>(
      favorites_ ? u"Add sites to your bookmarks bar"
                 : u"Save a page with Ctrl+D"));
  empty_hint_->SetEnabledColor(kColorToolbarText);
  empty_hint_->SetFontList(empty_hint_->font_list().DeriveWithSizeDelta(-2));
  empty_hint_->SetHorizontalAlignment(gfx::ALIGN_TO_HEAD);
  registrar_.Init(browser.GetProfile()->GetPrefs());
  registrar_.Add(CollapsePref(),
                 base::BindRepeating(&NoventraBookmarksView::ScheduleRefresh,
                                     base::Unretained(this)));
  if (UsesPopupButton()) {
    registrar_.Add(
        "noventra.browser.bookmark_opening",
        base::BindRepeating(&NoventraBookmarksView::OnOpeningStyleChanged,
                            base::Unretained(this)));
    OnOpeningStyleChanged();
  }
  if (new_folder_) {
    registrar_.Add(bookmarks::prefs::kEditBookmarksEnabled,
                   base::BindRepeating(&NoventraBookmarksView::ScheduleRefresh,
                                       base::Unretained(this)));
  }
  if (model_) {
    observation_.Observe(model_);
  }
  ScheduleRefresh();
}
NoventraBookmarksView::~NoventraBookmarksView() {
  ClosePopup();
}
const char* NoventraBookmarksView::CollapsePref() const {
  return favorites_ ? "noventra.browser.favorites_collapsed"
                    : "noventra.browser.bookmarks_collapsed";
}
bool NoventraBookmarksView::UsesPopupButton() const {
  return !favorites_ && presentation_ != Presentation::kButton;
}
bool NoventraBookmarksView::Compact() const {
  return presentation_ != Presentation::kSection ||
         GetProperty(kNoventraDockHorizontal) ||
         GetProperty(kNoventraDockCompact) ||
         (!favorites_ && width() > 0 &&
          width() < (inline_folders_ ? 160 : kMinimumLibraryWidth));
}
void NoventraBookmarksView::SetInlineFolders(bool enabled) {
  enabled = enabled && !favorites_ && presentation_ == Presentation::kSection;
  if (inline_folders_ == enabled) {
    return;
  }
  CloseMenu();
  context_menu_.reset();
  inline_folders_ = enabled;
  heading_->SetText(u"Bookmarks");
  heading_->SetAccessibleName(u"Bookmarks");
  heading_->SetLabelStyle(enabled ? views::style::STYLE_CAPTION
                                  : views::style::STYLE_PRIMARY);
  const ui::ColorId caption_color =
      enabled ? static_cast<ui::ColorId>(ui::kColorLabelForegroundSecondary)
              : static_cast<ui::ColorId>(kColorToolbarText);
  heading_->SetEnabledTextColors(caption_color);
  heading_->SetCustomPadding(gfx::Insets::VH(enabled ? 2 : 4, 6));
  heading_->SetMinSize(gfx::Size(0, enabled ? kInlineHeadingHeight : 28));
  SetFlatBackground(heading_, enabled);
  SetFlatBackground(compact_button_, enabled || !favorites_);
  new_folder_->SetEnabledTextColors(
      enabled ? std::make_optional<ui::ColorVariant>(
                    ui::kColorLabelForegroundSecondary)
              : std::nullopt);
  new_folder_->SetCustomPadding(gfx::Insets(enabled ? 4 : 6));
  empty_hint_->SetEnabledColor(caption_color);
  ScheduleRefresh();
  PreferredSizeChanged();
  InvalidateLayout();
}
bool NoventraBookmarksView::Collapsed() const {
  return browser_->GetProfile()->GetPrefs()->GetBoolean(CollapsePref());
}
bool NoventraBookmarksView::HasOpenPopup() const {
  // Native Show() can activate the bubble (and deactivate the browser) before
  // its visibility flag changes. Include that opening interval. ClosePopup()
  // synchronously clears the delegate before hiding or destroying the widget.
  return popup_ && (!popup_widget_ || !popup_widget_->IsClosed());
}

bool NoventraBookmarksView::HasDockContent() const {
  if (favorites_ || presentation_ == Presentation::kPanelButton) {
    return true;
  }
  return model_ && model_->loaded() &&
         std::ranges::any_of(model_->root_node()->children(),
                             [](const auto& node) {
                               return node->IsVisible() &&
                                      !node->children().empty();
                             });
}
void NoventraBookmarksView::AfterPropertyChange(const void* key, int64_t old) {
  views::View::AfterPropertyChange(key, old);
  if (key == kNoventraDockHorizontal || key == kNoventraDockCompact) {
    InvalidateLayout();
    PreferredSizeChanged();
  }
}
void NoventraBookmarksView::Toggle() {
  auto* prefs = browser_->GetProfile()->GetPrefs();
  if (!prefs->IsManagedPreference(CollapsePref())) {
    prefs->SetBoolean(CollapsePref(), !Collapsed());
  }
}
void NoventraBookmarksView::OnOpeningStyleChanged() {
  ClosePopup();
  const bool panel = browser_->GetProfile()->GetPrefs()->GetInteger(
                         "noventra.browser.bookmark_opening") == 1;
  if (opening_style_initialized_ && !panel) {
    cancel_pending_panel_ = panel_request_pending_;
    if (auto* side_panel = SidePanelUI::From(browser_->browser());
        side_panel &&
        side_panel->GetCurrentEntryId() == SidePanelEntryId::kBookmarks) {
      side_panel->Close();
    }
  }
  auto& accessibility = compact_button_->GetViewAccessibility();
  accessibility.SetHasPopup(panel ? ax::mojom::HasPopup::kFalse
                                  : ax::mojom::HasPopup::kDialog);
  if (panel) {
    accessibility.RemoveExpandCollapseState();
  } else {
    accessibility.SetIsCollapsed();
  }
  compact_button_->SetTooltipText(panel ? u"Show or hide bookmarks panel"
                                        : u"Show or hide bookmarks popup");
  opening_style_initialized_ = true;
}
void NoventraBookmarksView::OnPanelShown() {
  auto* side_panel = SidePanelUI::From(browser_->browser());
  const bool cancel = cancel_pending_panel_;
  panel_request_pending_ = cancel_pending_panel_ = false;
  if (cancel && side_panel &&
      browser_->GetProfile()->GetPrefs()->GetInteger(
          "noventra.browser.bookmark_opening") != 1 &&
      side_panel->GetCurrentEntryId() == SidePanelEntryId::kBookmarks) {
    // The entry can finish loading after another Settings window chose popup.
    // Only cancel a pending request made by this button, not unrelated panels.
    side_panel->Close();
  }
}
void NoventraBookmarksView::CloseMenu() {
  menu_highlight_.reset();
  if (!UsesPopupButton()) {
    compact_button_->GetViewAccessibility().SetIsCollapsed();
  }
  menu_.reset();
}
void NoventraBookmarksView::ClosePopup() {
  if (!popup_ && !popup_widget_) {
    return;
  }
  // Keep both alive until native activation/visibility observers return.
  // Member destruction order also preserves CLIENT_OWNS_WIDGET's requirement
  // that the delegate outlive the widget if the deletion task is discarded.
  struct ClosingPopup {
    std::unique_ptr<BookmarkPopup> popup;
    std::unique_ptr<views::Widget> widget;
  };
  auto closing = std::make_unique<ClosingPopup>();
  closing->popup = std::move(popup_);
  closing->widget = std::move(popup_widget_);
  if (closing->popup) {
    closing->popup->PrepareToClose();
  }
  if (closing->widget) {
    closing->widget->Hide();
  }
  base::SingleThreadTaskRunner::GetCurrentDefault()->DeleteSoon(
      FROM_HERE, std::move(closing));
  if (UsesPopupButton()) {
    compact_button_->GetViewAccessibility().SetIsCollapsed();
  }
}
void NoventraBookmarksView::BookmarkModelChanged() {
  if (menu_) {
    menu_->CancelForModelChange();
  }
  ScheduleRefresh();
}
void NoventraBookmarksView::BookmarkNodeFaviconChanged(
    const bookmarks::BookmarkNode* node) {
  if (popup_) {
    popup_->ScheduleRefresh();
  }
  if (!model_ || !node) {
    return;
  }
  const auto row = buttons_.find(node->id());
  if (row == buttons_.end()) {
    return;
  }
  // Favicons do not change section membership, order or labels. Preserve
  // native focus and update only the row that displays this bookmark.
  row->second->SetImageModel(views::Button::STATE_NORMAL,
                            BookmarkIcon(*model_, *node));
}
void NoventraBookmarksView::BookmarkModelBeingDeleted() {
  observation_.Reset();
  model_ = nullptr;
  ClosePopup();
  expanded_folders_.clear();
  CloseMenu();
  context_menu_.reset();
  ScheduleRefresh();
}
void NoventraBookmarksView::ScheduleRefresh() {
  if (pending_) {
    return;
  }
  pending_ = true;
  base::SingleThreadTaskRunner::GetCurrentDefault()->PostTask(
      FROM_HERE, base::BindOnce(&NoventraBookmarksView::Refresh,
                                weak_factory_.GetWeakPtr()));
}
std::vector<const bookmarks::BookmarkNode*> NoventraBookmarksView::Nodes()
    const {
  std::vector<const bookmarks::BookmarkNode*> result;
  if (presentation_ != Presentation::kSection || !model_ || !model_->loaded()) {
    return result;
  }
  if (favorites_) {
    result.reserve(6);
  }
  const auto append = [&](const auto& self,
                          const bookmarks::BookmarkNode* parent) -> void {
    if (!parent) {
      return;
    }
    for (const auto& child : parent->children()) {
      if (favorites_ && result.size() == 6) {
        break;
      }
      if (!favorites_ || child->is_url()) {
        result.push_back(child.get());
        if (inline_folders_ && child->is_folder() &&
            expanded_folders_.contains(child->id())) {
          self(self, child.get());
        }
      }
    }
  };
  append(append, model_->account_bookmark_bar_node());
  append(append, model_->bookmark_bar_node());
  if (!favorites_) {
    append(append, model_->account_other_node());
    append(append, model_->other_node());
    append(append, model_->account_mobile_node());
    append(append, model_->mobile_node());
  }
  return result;
}
void NoventraBookmarksView::Refresh() {
  pending_ = false;
  if (popup_) {
    popup_->ScheduleRefresh();
  }
  if (!HasDockContent()) {
    CloseMenu();
  }
  compact_button_->SetEnabled(model_ &&
                             (presentation_ == Presentation::kPanelButton ||
                              model_->loaded()));
  if (new_folder_) {
    new_folder_->SetEnabled(CanCreateFolder());
  }
  const auto nodes = Nodes();
  std::set<int64_t> present;
  order_.clear();
  for (const auto* node : nodes) {
    const auto id = node->id();
    present.insert(id);
    order_.push_back(id);
    if (!buttons_.contains(id)) {
      auto button = std::make_unique<BookmarkRowButton>(
          base::BindRepeating(&NoventraBookmarksView::OpenNode,
                              base::Unretained(this), id),
          u"", views::style::CONTEXT_LABEL);
      button->SetStyle(ui::ButtonStyle::kText);
      button->SetBgColorIdOverride(kColorToolbar);
      button->SetMinSize(gfx::Size(0, 0));
      button->SetCustomPadding(gfx::Insets::VH(6, 8));
      button->SetTriggerableEventFlags(ui::EF_LEFT_MOUSE_BUTTON |
                                       ui::EF_MIDDLE_MOUSE_BUTTON);
      button->SetHorizontalAlignment(favorites_ ? gfx::ALIGN_CENTER
                                                : gfx::ALIGN_TO_HEAD);
      button->SetEnabledTextColors(kColorToolbarText);
      button->set_context_menu_controller(this);
      if (favorites_) {
        button->SetBgColorIdOverride(kColorToolbarBackgroundSubtleEmphasis);
        button->SetCornerRadius(8);
      }
      if (rows_) {
        button->SetPreferredSize(gfx::Size(0, kBookmarkRowHeight));
        buttons_[id] = rows_->AddChildView(std::move(button));
      } else {
        buttons_[id] = AddChildView(std::move(button));
      }
    }
    auto* button = buttons_.at(id).get();
    const auto title = BookmarkDisplayTitle(*node);
    const bool inline_folder = inline_folders_ && node->is_folder();
    const bool expanded = expanded_folders_.contains(id);
    static_cast<BookmarkRowButton*>(button)->SetDisclosure(inline_folder,
                                                           expanded);
    if (!favorites_) {
      SetFlatBackground(button, inline_folders_);
    }
    button->SetText(favorites_ ? u"" : title);
    button->SetAccessibleName(title);
    auto& accessibility = button->GetViewAccessibility();
    accessibility.SetHasPopup(node->is_folder() && !inline_folder
                                  ? ax::mojom::HasPopup::kMenu
                                  : ax::mojom::HasPopup::kFalse);
    if (inline_folder) {
      if (expanded) {
        accessibility.SetIsExpanded();
      } else {
        accessibility.SetIsCollapsed();
      }
    } else {
      accessibility.RemoveExpandCollapseState();
    }
    int depth = 0;
    if (inline_folders_) {
      for (const auto* parent = node->parent();
           parent && !parent->is_permanent_node(); parent = parent->parent()) {
        ++depth;
      }
    }
    // Deep folder trees remain readable within the capped sidebar width.
    button->SetCustomPadding(gfx::Insets::TLBR(6, 8 + std::min(depth, 6) * 12,
                                               6, inline_folder ? 28 : 8));
    button->SetTooltipText(
        title +
        (node->is_url() ? u"\n" + base::UTF8ToUTF16(node->url().spec()) : u""));
    button->SetImageModel(views::Button::STATE_NORMAL,
                          BookmarkIcon(*model_, *node));
  }
  for (auto it = buttons_.begin(); it != buttons_.end();) {
    if (present.contains(it->first)) {
      ++it;
      continue;
    }
    const bool focused =
        GetFocusManager() && GetFocusManager()->GetFocusedView() == it->second;
    it->second->parent()->RemoveChildViewT(it->second.get());
    it = buttons_.erase(it);
    if (focused) {
      heading_->RequestFocus();
    }
  }
  // Native focus traversal must follow the displayed rows, including after
  // bookmark reorders. Keep button identities so favicon updates retain focus.
  if (rows_) {
    // Scroll content owns every row, so large libraries remain reachable
    // without growing the sidebar or replacing the tab list.
    for (size_t index = 0; index < order_.size(); ++index) {
      rows_->ReorderChildView(buttons_.at(order_[index]), index);
    }
    ReorderChildView(new_folder_, 1);
    ReorderChildView(scroll_, 2);
    ReorderChildView(manage_, children().size() - 1);
  } else {
    ReorderChildView(empty_hint_, 2);
    for (size_t index = 0; index < order_.size(); ++index) {
      ReorderChildView(buttons_.at(order_[index]), 3 + index);
    }
    ReorderChildView(manage_, 3 + order_.size());
  }
  if (Collapsed()) {
    heading_->GetViewAccessibility().SetIsCollapsed();
  } else {
    heading_->GetViewAccessibility().SetIsExpanded();
  }
  heading_->SetImageModel(
      views::Button::STATE_NORMAL,
      ui::ImageModel::FromVectorIcon(
          Collapsed() ? kChevronRightIcon : vector_icons::kExpandMoreOldIcon,
          kColorToolbarButtonIcon, 12));
  heading_->SetEnabled(
      !browser_->GetProfile()->GetPrefs()->IsManagedPreference(CollapsePref()));
  manage_->SetTooltipText(favorites_ ? u"Organize sites in your bookmarks bar"
                                     : u"Open the complete bookmark library");
  PreferredSizeChanged();
  InvalidateLayout();
}
gfx::Size NoventraBookmarksView::CalculatePreferredSize(
    const views::SizeBounds& available) const {
  if (!HasDockContent()) {
    return gfx::Size();
  }
  const bool compact =
      available.width().is_bounded()
          ? presentation_ != Presentation::kSection ||
                GetProperty(kNoventraDockHorizontal) ||
                GetProperty(kNoventraDockCompact) ||
                (!favorites_ &&
                 available.width().value() <
                     (inline_folders_ ? 160 : kMinimumLibraryWidth))
          : Compact();
  if (compact) {
    return gfx::Size(40, 40);
  }
  return gfx::Size(232, ExpandedHeight());
}
void NoventraBookmarksView::OnBoundsChanged(const gfx::Rect& previous_bounds) {
  views::View::OnBoundsChanged(previous_bounds);
  if (!favorites_ && presentation_ == Presentation::kSection) {
    const int minimum_width = inline_folders_ ? 160 : kMinimumLibraryWidth;
    const bool was_narrow =
        previous_bounds.width() > 0 && previous_bounds.width() < minimum_width;
    const bool narrow = width() > 0 && width() < minimum_width;
    if (was_narrow != narrow) {
      // The dock's next measurement must reserve a single shortcut row when
      // shrinking, and restore the saved library height when widening again.
      PreferredSizeChanged();
    }
  }
}
int NoventraBookmarksView::ExpandedHeight() const {
  const int count = static_cast<int>(order_.size());
  if (inline_folders_) {
    if (Collapsed()) {
      return kInlineHeadingHeight;
    }
    const int body =
        count
            ? std::min(count * kBookmarkRowHeight, kBookmarkViewportHeight) + 4
            : 30;
    return kInlineHeadingHeight + body;
  }
  const int body = Collapsed()
                       ? 0
                       : (favorites_ ? ((count + 2) / 3) * 40
                                     : std::min(count * kBookmarkRowHeight,
                                                kBookmarkViewportHeight)) +
                             (count ? 32 : 60);
  return 36 + body;
}
void NoventraBookmarksView::Layout(PassKey) {
  if (!HasDockContent()) {
    for (views::View* child : children()) {
      child->SetVisible(false);
    }
    return;
  }
  const bool compact = Compact();
  if (!compact && popup_) {
    ClosePopup();
  }
  // A narrow saved library layout gets one readable entry point instead of
  // squeezing a disclosure, an ellipsized title and a New Folder button into
  // the sidebar. The expanded folder/collapse state remains intact.
  const bool labeled_shortcut =
      compact && UsesPopupButton() && presentation_ == Presentation::kSection &&
      width() >= 120 && !GetProperty(kNoventraDockHorizontal) &&
      !GetProperty(kNoventraDockCompact);
  compact_button_->SetText(labeled_shortcut ? u"Bookmarks" : u"");
  // LabelButton derives accessibility from its text, including an empty icon
  // label. Preserve the control's stable name when its presentation changes.
  compact_button_->SetAccessibleName(favorites_ ? u"Favorites" : u"Bookmarks");
  compact_button_->SetHorizontalAlignment(labeled_shortcut ? gfx::ALIGN_TO_HEAD
                                                           : gfx::ALIGN_CENTER);
  compact_button_->SetImageLabelSpacing(8);
  heading_->SetVisible(!compact);
  compact_button_->SetVisible(compact);
  compact_button_->SetBounds(0, 0, width(), height());
  if (inline_folders_) {
    // The sidebar library uses flat folder rows above the space controls.
    // Retain the collapse preference and native creation action in a quiet
    // caption row; the complete library is still in the browser menu.
    heading_->SetBounds(8, 0, std::max(0, width() - 42),
                        std::min(kInlineHeadingHeight, height()));
    new_folder_->SetVisible(!compact);
    new_folder_->SetBounds(std::max(0, width() - 30), 0, std::min(24, width()),
                           std::min(24, height()));
    const bool body = !compact && !Collapsed();
    manage_->SetVisible(false);
    empty_hint_->SetVisible(body && order_.empty() &&
                            height() >= kInlineHeadingHeight + 24);
    empty_hint_->SetBounds(14, kInlineHeadingHeight, std::max(0, width() - 28),
                           28);
    scroll_->SetVisible(body && !order_.empty());
    scroll_->SetBounds(6, kInlineHeadingHeight, std::max(0, width() - 12),
                       std::max(0, height() - kInlineHeadingHeight - 4));
    for (const auto& [id, button] : buttons_) {
      button->SetVisible(true);
    }
    return;
  }
  heading_->SetBounds(6, 2, std::max(0, width() - (new_folder_ ? 48 : 12)),
                      std::min(32, height()));
  if (new_folder_) {
    new_folder_->SetVisible(!compact);
    new_folder_->SetBounds(std::max(0, width() - 38), 2, std::min(32, width()),
                           std::min(32, height()));
  }
  const bool body = !compact && !Collapsed();
  const int footer_y = std::max(36, height() - 30);
  empty_hint_->SetVisible(body && order_.empty() && height() >= 92);
  empty_hint_->SetBounds(14, 36, std::max(0, width() - 28), 24);
  manage_->SetVisible(body && height() >= 64);
  manage_->SetBounds(8, footer_y, std::max(0, width() - 16), 28);
  if (scroll_) {
    scroll_->SetVisible(body && !order_.empty());
    scroll_->SetBounds(8, 36, std::max(0, width() - 16),
                       std::max(0, footer_y - 36));
    // Visibility belongs to the section; ScrollView clips and scrolls the
    // rows, including bringing keyboard-focused bookmarks into view.
    for (const auto& [id, button] : buttons_) {
      button->SetVisible(true);
    }
    return;
  }
  int index = 0;
  for (const auto id : order_) {
    auto* button = buttons_.at(id).get();
    const int y = 36 + (favorites_ ? index / 3 * 40 : index * 32);
    const int row_height = favorites_ ? 36 : 32;
    const int cell = std::max(0, (width() - 24) / 3);
    button->SetVisible(body && y + row_height <= footer_y);
    button->SetBounds(favorites_ ? 8 + index % 3 * (cell + 4) : 8, y,
                      favorites_ ? cell : std::max(0, width() - 16),
                      row_height);
    ++index;
  }
}
void NoventraBookmarksView::OpenNode(int64_t id, const ui::Event& event) {
  const auto* node =
      model_ ? bookmarks::GetBookmarkNodeByID(model_, id) : nullptr;
  if (!node) {
    return;
  }
  if (node->is_folder()) {
    if (inline_folders_ &&
        !(event.flags() & (ui::EF_MIDDLE_MOUSE_BUTTON | ui::EF_CONTROL_DOWN |
                           ui::EF_SHIFT_DOWN | ui::EF_ALT_DOWN))) {
      if (!expanded_folders_.erase(id)) {
        expanded_folders_.insert(id);
      }
      ScheduleRefresh();
      return;
    }
    menu_ = std::make_unique<FolderMenu>(
        *browser_, BookmarkParentFolder::FromFolderNode(node),
        base::BindRepeating(&NoventraBookmarksView::CloseMenu,
                            weak_factory_.GetWeakPtr()));
    menu_->Run(*buttons_.at(id), ui::GetMenuSourceTypeForEvent(event));
    return;
  }
  NavigateParams params(browser_->browser(), node->url(),
                        ui::PAGE_TRANSITION_AUTO_BOOKMARK);
  params.disposition = ui::DispositionFromEventFlags(event.flags());
  Navigate(&params);
}
void NoventraBookmarksView::ShowLibrary(const ui::Event& event) {
  if (UsesPopupButton()) {
    auto* prefs = browser_->GetProfile()->GetPrefs();
    if (prefs->GetInteger("noventra.browser.bookmark_opening") == 1) {
      ClosePopup();
      if (!GetWidget() || !IsDrawn()) {
        return;
      }
      auto* side_panel = SidePanelUI::From(browser_->browser());
      if (!side_panel) {
        return;
      }
      // Apply the chosen sidebar side to this entry only. Other panels and
      // policy-controlled alignment keep their existing preferences.
      if (!prefs->IsManagedPreference(prefs::kSidePanelHorizontalAlignment) &&
          !prefs->IsManagedPreference(prefs::kSidePanelAlignmentOverrides)) {
        for (views::View* ancestor = parent(); ancestor;
             ancestor = ancestor->parent()) {
          if (const auto* dock = views::AsViewClass<NoventraDockView>(ancestor)) {
            if (dock->edge() == noventra::DockEdge::kLeft ||
                dock->edge() == noventra::DockEdge::kRight) {
              ScopedDictPrefUpdate overrides(
                  prefs, prefs::kSidePanelAlignmentOverrides);
              overrides->Set(
                  SidePanelEntryIdToString(SidePanelEntryId::kBookmarks),
                  dock->edge() == noventra::DockEdge::kRight);
            }
            break;
          }
        }
      }
      compact_button_->RequestFocus();
      panel_request_pending_ =
          side_panel->GetCurrentEntryId() != SidePanelEntryId::kBookmarks;
      cancel_pending_panel_ = false;
      panel_shown_subscription_ = side_panel->RegisterSidePanelShown(
          base::BindRepeating(&NoventraBookmarksView::OnPanelShown,
                              weak_factory_.GetWeakPtr()));
      side_panel->Toggle(SidePanelEntryKey(SidePanelEntryId::kBookmarks),
                         SidePanelOpenTrigger::kToolbarButton);
      return;
    }
    if (popup_) {
      ClosePopup();
    } else if (model_ && GetWidget() && IsDrawn()) {
      compact_button_->RequestFocus();
      compact_button_->GetViewAccessibility().SetIsExpanded();
      popup_ = std::make_unique<BookmarkPopup>(*this);
      popup_widget_ = views::BubbleDialogDelegate::CreateBubble(
          popup_.get(),
          base::BindOnce(
              [](base::WeakPtr<NoventraBookmarksView> owner,
                 views::Widget::ClosedReason) {
                if (owner) {
                  owner->ClosePopup();
                }
              },
              weak_factory_.GetWeakPtr()));
      popup_widget_->Show();
    }
    return;
  }
  if (!model_ || !model_->loaded() || !HasDockContent() || !GetWidget() ||
      !IsDrawn()) {
    return;
  }
  if (menu_) {
    CloseMenu();
    return;
  }
  // Preserve a visible anchor during edge-reveal mode and let native menu
  // handling restore keyboard focus here on Escape.
  compact_button_->RequestFocus();
  menu_highlight_ = compact_button_->AddAnchorHighlight();
  compact_button_->GetViewAccessibility().SetIsExpanded();
  menu_ = std::make_unique<FolderMenu>(
      *browser_,
      favorites_ ? std::make_optional(BookmarkParentFolder::BookmarkBarFolder())
                 : std::nullopt,
      base::BindRepeating(&NoventraBookmarksView::CloseMenu,
                          weak_factory_.GetWeakPtr()));
  menu_->Run(*compact_button_, ui::GetMenuSourceTypeForEvent(event));
}
void NoventraBookmarksView::Manage() {
  NavigateParams params(
      browser_->browser(),
      GURL(favorites_ ? "chrome://bookmarks/?id=1" : "chrome://bookmarks/"),
      ui::PAGE_TRANSITION_AUTO_BOOKMARK);
  params.disposition = WindowOpenDisposition::NEW_FOREGROUND_TAB;
  Navigate(&params);
}
bool NoventraBookmarksView::CanCreateFolder() const {
  return model_ && model_->loaded() &&
         browser_->GetProfile()->GetPrefs()->GetBoolean(
             bookmarks::prefs::kEditBookmarksEnabled);
}
void NoventraBookmarksView::NewFolder() {
  if (!CanCreateFolder() || !GetWidget()) {
    return;
  }
  // Use the same merged-surface parent selection and native editor as the
  // bookmark bar. Cancel creates nothing; Save notifies the live model.
  const auto folder = BookmarkParentFolder::BookmarkBarFolder();
  const auto* parent = BookmarkUIOperationsHelperMergedSurfaces(
                           BookmarkMergedSurfaceServiceFactory::GetForProfile(
                               browser_->GetProfile()),
                           &folder)
                           .GetDefaultParentForNonMergedSurfaces();
  if (!parent) {
    return;
  }
  BookmarkEditor::Show(
      GetWidget()->GetNativeWindow(), browser_->GetProfile(),
      BookmarkEditor::EditDetails::AddFolder(parent, parent->children().size()),
      BookmarkEditor::SHOW_TREE);
}
void NoventraBookmarksView::ShowContextMenuForViewImpl(
    views::View* source,
    const gfx::Point& point,
    ui::mojom::MenuSourceType type) {
  for (const auto& [id, button] : buttons_) {
    if (source != button) {
      continue;
    }
    const auto* node =
        model_ ? bookmarks::GetBookmarkNodeByID(model_, id) : nullptr;
    if (!node) {
      return;
    }
    auto parent = BookmarkContextMenuController::GetParentForNewNodes(
        std::vector<raw_ptr<const bookmarks::BookmarkNode, VectorExperimental>>{
            node});
    BookmarkUIOperationsHelperMergedSurfaces(
        BookmarkMergedSurfaceServiceFactory::GetForProfile(
            browser_->GetProfile()),
        parent.get())
        .CanPasteFromClipboard(
            base::BindOnce(&NoventraBookmarksView::RunContextMenu,
                           weak_factory_.GetWeakPtr(), id, point, type));
    return;
  }
}
void NoventraBookmarksView::RunContextMenu(int64_t id,
                                           const gfx::Point& point,
                                           ui::mojom::MenuSourceType type,
                                           bool can_paste) {
  const auto* node =
      model_ ? bookmarks::GetBookmarkNodeByID(model_, id) : nullptr;
  if (!node || !GetWidget() || !IsDrawn() || !buttons_.contains(id) ||
      !buttons_.at(id)->IsDrawn()) {
    return;
  }
  context_menu_ = std::make_unique<BookmarkContextMenu>(
      GetWidget(), browser_->browser(), browser_->GetProfile(),
      BookmarkLaunchLocation::kSidePanelFolder,
      std::vector<raw_ptr<const bookmarks::BookmarkNode, VectorExperimental>>{
          node},
      true, can_paste);
  context_menu_->RunMenuAt(point, type);
}
BEGIN_METADATA(NoventraBookmarksView)
END_METADATA
