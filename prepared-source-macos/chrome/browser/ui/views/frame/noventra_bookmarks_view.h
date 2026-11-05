// Copyright 2026 The Noventra Authors
// SPDX-License-Identifier: GPL-3.0-only
#ifndef CHROME_BROWSER_UI_VIEWS_FRAME_NOVENTRA_BOOKMARKS_VIEW_H_
#define CHROME_BROWSER_UI_VIEWS_FRAME_NOVENTRA_BOOKMARKS_VIEW_H_

#include <map>
#include <memory>
#include <optional>
#include <set>
#include <vector>

#include "base/callback_list.h"
#include "base/memory/raw_ptr.h"
#include "base/memory/weak_ptr.h"
#include "base/scoped_observation.h"
#include "components/bookmarks/browser/base_bookmark_model_observer.h"
#include "components/bookmarks/browser/bookmark_model.h"
#include "components/prefs/pref_change_registrar.h"
#include "ui/base/metadata/metadata_header_macros.h"
#include "ui/views/context_menu_controller.h"
#include "ui/views/controls/button/button.h"
#include "ui/views/view.h"

class BrowserView;
class BookmarkContextMenu;
namespace views {
class MdTextButton;
class MenuButtonController;
class Label;
class ScrollView;
class Widget;
}  // namespace views

// Presentations of the real bookmark library. Stable node IDs bind actions;
// moving the dock never moves tabs, bookmarks or their profile-owned models.
class NoventraBookmarksView : public views::View,
                              public bookmarks::BaseBookmarkModelObserver,
                              public views::ContextMenuController {
  METADATA_HEADER(NoventraBookmarksView, views::View)
 public:
  enum class Presentation { kSection, kButton, kPanelButton };
  NoventraBookmarksView(BrowserView& browser,
                        bool favorites,
                        Presentation presentation = Presentation::kSection);
  ~NoventraBookmarksView() override;
  gfx::Size CalculatePreferredSize(
      const views::SizeBounds& available) const override;
  void OnBoundsChanged(const gfx::Rect& previous_bounds) override;
  void Layout(PassKey) override;
  int ExpandedHeight() const;
  // Combined sidebars expand folders in place. Other library presentations
  // retain their native folder menus.
  void SetInlineFolders(bool enabled);
  bool inline_folders() const { return inline_folders_; }
  // Empty library/menu controls do not occupy a dock slot. Favorites retains
  // its creation affordance, and the searchable popup stays reachable.
  bool HasDockContent() const;
  bool HasOpenPopup() const;
  void AfterPropertyChange(const void* key, int64_t old_value) override;
  void BookmarkModelChanged() override;
  void BookmarkModelBeingDeleted() override;
  void BookmarkNodeFaviconChanged(const bookmarks::BookmarkNode* node) override;
  void ShowContextMenuForViewImpl(
      views::View* source,
      const gfx::Point& point,
      ui::mojom::MenuSourceType source_type) override;

 private:
  class FolderMenu;
  class BookmarkPopup;
  void CloseMenu();
  void ClosePopup();
  void OnOpeningStyleChanged();
  void OnPanelShown();
  void RunContextMenu(int64_t id,
                      const gfx::Point& point,
                      ui::mojom::MenuSourceType type,
                      bool can_paste);
  void ScheduleRefresh();
  void Refresh();
  void Toggle();
  void OpenNode(int64_t id, const ui::Event& event);
  void ShowLibrary(const ui::Event& event);
  void Manage();
  void NewFolder();
  bool CanCreateFolder() const;
  bool UsesPopupButton() const;
  bool Compact() const;
  bool Collapsed() const;
  const char* CollapsePref() const;
  std::vector<const bookmarks::BookmarkNode*> Nodes() const;

  const raw_ptr<BrowserView> browser_;
  const bool favorites_;
  const Presentation presentation_;
  raw_ptr<bookmarks::BookmarkModel> model_ = nullptr;
  raw_ptr<views::MdTextButton> heading_ = nullptr;
  raw_ptr<views::MdTextButton> compact_button_ = nullptr;
  raw_ptr<views::MenuButtonController> popup_button_controller_ = nullptr;
  raw_ptr<views::MdTextButton> manage_ = nullptr;
  raw_ptr<views::MdTextButton> new_folder_ = nullptr;
  raw_ptr<views::ScrollView> scroll_ = nullptr;
  raw_ptr<views::View> rows_ = nullptr;
  raw_ptr<views::Label> empty_hint_ = nullptr;
  std::map<int64_t, raw_ptr<views::MdTextButton>> buttons_;
  std::vector<int64_t> order_;
  base::ScopedObservation<bookmarks::BookmarkModel,
                          bookmarks::BookmarkModelObserver>
      observation_{this};
  PrefChangeRegistrar registrar_;
  std::unique_ptr<FolderMenu> menu_;
  std::optional<views::Button::ScopedAnchorHighlight> menu_highlight_;
  std::unique_ptr<BookmarkContextMenu> context_menu_;
  // The client-owned widget is destroyed before its delegate and contents.
  std::unique_ptr<BookmarkPopup> popup_;
  std::unique_ptr<views::Widget> popup_widget_;
  bool pending_ = false;
  bool opening_style_initialized_ = false;
  bool panel_request_pending_ = false;
  bool cancel_pending_panel_ = false;
  base::CallbackListSubscription panel_shown_subscription_;
  bool inline_folders_ = false;
  std::set<int64_t> expanded_folders_;
  base::WeakPtrFactory<NoventraBookmarksView> weak_factory_{this};
};
#endif
