#include "components/tabs/public/split_tab_data.h"
// Copyright 2012 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/ui/tabs/tab_menu_model.h"

#include <memory>
#include <optional>

#include "base/command_line.h"
#include "base/containers/flat_set.h"
#include "base/feature_list.h"
#include "base/functional/bind.h"
#include "base/functional/callback_helpers.h"
#include "base/i18n/rtl.h"
#include "base/metrics/user_metrics.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/utf_string_conversions.h"
#include "base/task/single_thread_task_runner.h"
#include "build/build_config.h"
#include "chrome/app/chrome_command_ids.h"
#include "chrome/app/vector_icons/vector_icons.h"
#include "chrome/browser/glic/browser_ui/glic_vector_icon_manager.h"
#include "chrome/browser/glic/public/glic_enabling.h"
#include "chrome/browser/glic/public/glic_keyed_service.h"
#include "chrome/browser/glic/public/glic_keyed_service_factory.h"
#include "chrome/browser/glic/public/service/glic_instance_coordinator.h"
#include "chrome/browser/glic/resources/grit/glic_browser_resources.h"
#include "chrome/browser/profiles/noventra_account_spaces.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/send_tab_to_self/send_tab_to_self_util.h"
#include "chrome/browser/ui/browser_element_identifiers.h"
#include "chrome/browser/ui/browser_window/public/browser_window_features.h"
#include "chrome/browser/ui/browser_window/public/browser_window_interface.h"
#include "chrome/browser/ui/color/chrome_color_id.h"
#include "chrome/browser/ui/immersive/immersive_mode_controller.h"
#include "chrome/browser/ui/noventra/pinned_tab_state.h"
#include "chrome/browser/ui/send_tab_to_self/send_tab_to_self_context_menu_delegate.h"
#include "chrome/browser/ui/tabs/existing_tab_group_sub_menu_model.h"
#include "chrome/browser/ui/tabs/existing_window_sub_menu_model.h"
#include "chrome/browser/ui/tabs/features.h"
#include "chrome/browser/ui/tabs/glic_tab_sub_menu_model.h"
#include "chrome/browser/ui/tabs/split_tab_menu_model.h"
#include "chrome/browser/ui/tabs/split_tab_swap_menu_model.h"
#include "chrome/browser/ui/tabs/tab_group_model.h"
#include "chrome/browser/ui/tabs/tab_menu_model_delegate.h"
#include "chrome/browser/ui/tabs/tab_strip_model.h"
#include "chrome/browser/ui/tabs/tab_strip_model_delegate.h"
#include "chrome/browser/ui/tabs/tab_utils.h"
#include "chrome/browser/ui/tabs/vertical_tab_strip_state_controller.h"
#include "chrome/browser/ui/ui_features.h"
#include "chrome/browser/ui/web_applications/app_browser_controller.h"
#include "chrome/browser/ui/web_applications/web_app_tabbed_utils.h"
#include "chrome/browser/user_education/user_education_service.h"
#include "chrome/common/chrome_features.h"
#include "chrome/common/webui_url_constants.h"
#include "chrome/grit/generated_resources.h"
#include "components/commerce/core/commerce_constants.h"
#include "components/commerce/core/commerce_feature_list.h"
#include "components/constrained_window/constrained_window_views.h"
#include "components/feed/feed_feature_list.h"
#include "components/send_tab_to_self/features.h"
#include "components/split_tabs/split_tab_visual_data.h"
#include "components/tabs/public/tab_context_menu_command.h"
#include "components/tabs/public/tab_group.h"
#include "components/tabs/public/tab_interface.h"
#include "components/vector_icons/vector_icons.h"
#include "content/public/browser/context_menu_params.h"
#include "content/public/browser/web_contents.h"
#include "extensions/buildflags/buildflags.h"
#include "ui/base/base_window.h"
#include "ui/base/l10n/l10n_util.h"
#include "ui/base/models/dialog_model.h"
#include "ui/base/ui_base_features.h"
#include "ui/base/window_open_disposition.h"
#include "ui/views/vector_icons.h"

#if BUILDFLAG(ENABLE_EXTENSIONS)
#include "chrome/browser/extensions/context_menu_matcher.h"
#include "chrome/browser/extensions/menu_manager.h"
#include "extensions/common/extension_features.h"
#endif

using base::UserMetricsAction;

namespace {
constexpr int kTabMenuIconSize = 16;

// Local menu IDs: existing split/layout menus occupy 1701-1902 (see
// ExistingBaseSubMenuModel). Reserve 2001, 2100-2300 and 2401-2403 here.
constexpr int kSeparateTabsCommand = 2001;
constexpr int kSplitWithNewTabCommand = 2100;
constexpr int kFirstSplitCandidateCommand = 2101;
constexpr size_t kMaxSplitCandidates = 200;
constexpr int kEditPinnedTabCommand = 2401;
constexpr int kResetPinnedTabCommand = 2402;
constexpr int kUseCurrentPinnedHomeCommand = 2403;

DEFINE_LOCAL_ELEMENT_IDENTIFIER_VALUE(kPinnedNameField);
DEFINE_LOCAL_ELEMENT_IDENTIFIER_VALUE(kPinnedHomeField);
DEFINE_LOCAL_ELEMENT_IDENTIFIER_VALUE(kPinnedValidationError);

bool IsPinnedTabCommand(int command) {
  return command == kEditPinnedTabCommand ||
         command == kResetPinnedTabCommand ||
         command == kUseCurrentPinnedHomeCommand;
}

class PinnedTabEditorDelegate : public ui::DialogModelDelegate {
 public:
  PinnedTabEditorDelegate(base::WeakPtr<tabs::TabInterface> tab,
                          base::WeakPtr<BrowserWindowInterface> browser)
      : tab_(std::move(tab)), browser_(std::move(browser)) {}

  bool Save() {
    // A menu/dialog can outlive its tab, or an extension can move/unpin it.
    if (!tab_ || !browser_ || !tab_->IsPinned() ||
        tab_->GetBrowserWindowInterface() != browser_.get()) {
      return true;
    }
    const auto* home_field =
        dialog_model()->GetTextfieldByUniqueId(kPinnedHomeField);
    // Home is a complete URL, not omnibox input. FixupURL treats script-like
    // values such as javascript:1 as an HTTP hostname and port.
    const GURL home(base::UTF16ToUTF8(home_field->text()));
    const auto* name_field =
        dialog_model()->GetTextfieldByUniqueId(kPinnedNameField);
    if (!noventra::IsValidPinnedHomeUrl(home) ||
        !noventra::SetPinnedTabData(tab_->GetContents(), home,
                                    name_field->text())) {
      dialog_model()->SetVisible(kPinnedValidationError, true);
      return false;
    }
    return true;
  }

 private:
  base::WeakPtr<tabs::TabInterface> tab_;
  base::WeakPtr<BrowserWindowInterface> browser_;
};

void ShowPinnedTabEditor(base::WeakPtr<tabs::TabInterface> tab,
                         base::WeakPtr<BrowserWindowInterface> browser) {
  if (!tab || !tab->IsPinned() || !browser || !browser->GetWindow() ||
      tab->GetBrowserWindowInterface() != browser.get()) {
    return;
  }
  // Copy the metadata; a later edit, navigation or discard may replace it.
  noventra::PinnedTabData data;
  if (const auto* stored = noventra::GetPinnedTabData(tab->GetContents())) {
    data = *stored;
  }
  if (data.home_url.is_empty() &&
      noventra::IsValidPinnedHomeUrl(tab->GetContents()->GetVisibleURL())) {
    data.home_url = tab->GetContents()->GetVisibleURL();
  }
  auto delegate = std::make_unique<PinnedTabEditorDelegate>(tab, browser);
  auto* delegate_ptr = delegate.get();
  auto model =
      ui::DialogModel::Builder(std::move(delegate))
          .SetTitle(u"Edit pinned tab")
          .AddParagraph(ui::DialogModelLabel(
              u"Leave Name blank to use the page title. Reset to home opens "
              u"the saved URL in this pin."))
          .AddTextfield(kPinnedNameField, u"Name", data.custom_name)
          .AddTextfield(kPinnedHomeField, u"Home URL",
                        base::UTF8ToUTF16(data.home_url.spec()))
          .AddParagraph(ui::DialogModelLabel(
                            u"Use a full URL such as https://example.com "
                            u"without a username or password, "
                            u"and a single-line name of up to 256 characters."),
                        std::u16string(), kPinnedValidationError)
          .AddOkButton(base::BindRepeating(&PinnedTabEditorDelegate::Save,
                                           base::Unretained(delegate_ptr)),
                       ui::DialogModel::Button::Params().SetLabel(u"Save"))
          .AddCancelButton(base::DoNothing())
          .SetInitiallyFocusedField(kPinnedNameField)
          .Build();
  model->SetVisible(kPinnedValidationError, false);
  constrained_window::ShowBrowserModal(std::move(model),
                                       browser->GetWindow()->GetNativeWindow());
}

// Keep identities, not indices: extensions may move or close a tab while its
// menu is open. Every action revalidates both tabs against the original window.
class SplitWithTabMenuModel : public ui::SimpleMenuModel,
                              public ui::SimpleMenuModel::Delegate {
 public:
  SplitWithTabMenuModel(TabStripModel* model, int context_index)
      : ui::SimpleMenuModel(this),
        model_(model),
        source_(model->GetTabAtIndex(context_index)->GetWeakPtr()) {
    AddItem(kSplitWithNewTabCommand, u"New tab");
    for (int i = 0; i < model->count(); ++i) {
      auto* tab = model->GetTabAtIndex(i);
      if (tab == source_.get() || tab->IsSplit() || noventra::SpaceId(tab->GetContents()) != noventra::SpaceId(source_->GetContents())) {
        continue;
      }
      if (candidates_.size() == kMaxSplitCandidates) {
        break;
      }
      if (candidates_.empty()) {
        AddSeparator(ui::NORMAL_SEPARATOR);
      }
      auto title = noventra::PinnedTabDisplayTitle(
          tab->GetContents(), tab->GetContents()->GetTitle());
      if (title.empty()) {
        title = base::UTF8ToUTF16(
            tab->GetContents()->GetVisibleURL().spec());
      }
      if (title.size() > 80) {
        title = title.substr(0, 80) + u"…";
      }
      // Tab numbers distinguish pages with identical titles.
      title += u" (" + base::NumberToString16(i + 1) + u")";
      AddItem(kFirstSplitCandidateCommand + candidates_.size(), title);
      SetMayHaveMnemonicsAt(GetItemCount() - 1, false);
      candidates_.push_back(tab->GetWeakPtr());
    }
  }

  bool IsCommandIdEnabled(int command_id) const override {
    if (!IsAvailable(source_)) {
      return false;
    }
    if (command_id == kSplitWithNewTabCommand) {
      return !source_->IsSplit();
    }
    const int candidate = command_id - kFirstSplitCandidateCommand;
    return candidate >= 0 &&
           static_cast<size_t>(candidate) < candidates_.size() &&
           IsAvailable(candidates_[candidate]);
  }

  void ExecuteCommand(int command_id, int event_flags) override {
    if (!IsCommandIdEnabled(command_id)) {
      return;
    }
    // Activation can close the menu. Keep everything used after it in locals.
    TabStripModel* model = model_;
    auto source = source_;
    auto target = command_id == kSplitWithNewTabCommand
                      ? source
                      : candidates_[command_id - kFirstSplitCandidateCommand];
    if (command_id != kSplitWithNewTabCommand) {
      if (source->IsSplit()) {
        model->JoinNoventraSplit(model->GetIndexOfTab(target.get()),
                                 model->GetIndexOfTab(source.get()));
      } else {
        model->JoinNoventraSplit(model->GetIndexOfTab(source.get()),
                                 model->GetIndexOfTab(target.get()));
      }
      return;
    }
    model->ActivateTab(source.get());
    if (!source || !target || source->IsSplit() || target->IsSplit() ||
        model->GetIndexOfTab(source.get()) != model->active_index()) {
      return;
    }
    const int target_index = model->GetIndexOfTab(target.get());
    if (!model->ContainsIndex(target_index)) {
      return;
    }
    // Use the normal command to retain tab-group removal confirmation. The
    // source is now the sole selection, so an unrelated selection is ignored.
    model->ExecuteAddToNewSplitCommand(
        target_index, split_tabs::SplitTabLayout::kSideBySide);
  }

 private:
  bool IsAvailable(const base::WeakPtr<tabs::TabInterface>& tab) const {
    return tab && (!tab->IsSplit() || model_->GetSplitData(tab->GetSplit().value())->ListTabs().size() < 4) &&
           model_->ContainsIndex(model_->GetIndexOfTab(tab.get()));
  }

  const raw_ptr<TabStripModel> model_;
  const base::WeakPtr<tabs::TabInterface> source_;
  std::vector<base::WeakPtr<tabs::TabInterface>> candidates_;
};
}

DEFINE_CLASS_ELEMENT_IDENTIFIER_VALUE(TabMenuModel, kAddANoteTabMenuItem);
DEFINE_CLASS_ELEMENT_IDENTIFIER_VALUE(TabMenuModel, kSplitTabsMenuItem);
DEFINE_CLASS_ELEMENT_IDENTIFIER_VALUE(TabMenuModel, kArrangeSplitTabsMenuItem);
DEFINE_CLASS_ELEMENT_IDENTIFIER_VALUE(TabMenuModel, kSwapSplitTabsMenuItem);
DEFINE_CLASS_ELEMENT_IDENTIFIER_VALUE(TabMenuModel, kAddNewTabAdjacentMenuItem);
DEFINE_CLASS_ELEMENT_IDENTIFIER_VALUE(TabMenuModel, kDuplicateMenuItem);
DEFINE_CLASS_ELEMENT_IDENTIFIER_VALUE(TabMenuModel, kFocusSelectionMenuItem);

TabMenuModel::TabMenuModel(ui::SimpleMenuModel::Delegate* delegate,
                           TabMenuModelDelegate* tab_menu_model_delegate,
                           TabStripModel* tab_strip,
                           int index)
    : ui::SimpleMenuModel(delegate),
      tab_strip_(tab_strip),
      tab_menu_model_delegate_(tab_menu_model_delegate),
      tab_interface_(tab_strip_->GetTabAtIndex(index)->GetWeakPtr()) {
  CHECK(tab_strip_);

  if (tab_strip_->delegate()->IsForWebApp()) {
    BuildForWebApp(index);
  } else {
    Build(index);
  }
}

TabMenuModel::~TabMenuModel() = default;

void TabMenuModel::BuildForWebApp(int index) {
  auto* browser = tab_strip_->delegate()->GetBrowserWindowInterface();
  auto* app_controller =
      browser ? web_app::AppBrowserController::From(browser) : nullptr;
  const std::optional<base::flat_set<tabs::TabContextMenuCommand>> allowed =
      app_controller ? app_controller->GetAllowedTabMenuCommands()
                     : std::nullopt;

  // 1. Resolve the set of commands upfront.
  base::flat_set<tabs::TabContextMenuCommand> commands;
  if (allowed.has_value()) {
    commands = *allowed;
  } else {
    commands = {
        tabs::CommandCopyURL,
        tabs::CommandReload,
        tabs::CommandGoBack,
        tabs::CommandMoveToExistingWindow,
        tabs::CommandMoveTabsToNewWindow,
        tabs::CommandCloseTab,
        tabs::CommandCloseOtherTabs,
        tabs::CommandCloseAllTabs,
    };
  }

  // 2. Add common creation & navigation actions (available to all tabs).
  if (commands.contains(tabs::CommandNewTabToRight)) {
    AddItemWithStringId(TabStripModel::CommandNewTabToRight,
                        base::i18n::IsRTL() ? IDS_TAB_CXMENU_NEWTABTOLEFT
                                            : IDS_TAB_CXMENU_NEWTABTORIGHT);
  }
  if (commands.contains(tabs::CommandCopyURL)) {
    AddItemWithStringId(TabStripModel::CommandCopyURL, IDS_TAB_CXMENU_COPY_URL);
  }
  if (commands.contains(tabs::CommandReload)) {
    AddItemWithStringId(TabStripModel::CommandReload, IDS_TAB_CXMENU_RELOAD);
  }
  if (commands.contains(tabs::CommandGoBack)) {
    AddItemWithStringId(TabStripModel::CommandGoBack, IDS_CONTENT_CONTEXT_BACK);
  }

  // 3. Add "Move" actions (not available to pinned home tab).
  const bool has_pinned_home = web_app::HasPinnedHomeTab(tab_strip_);
  const bool pinned_home_is_part_of_selection =
      has_pinned_home &&
      tab_strip_->selection_model().IsSelected(*tab_strip_->begin());
  const bool is_pinned_home = web_app::IsPinnedHomeTab(tab_strip_, index);
  if (!is_pinned_home && !pinned_home_is_part_of_selection) {
    int num_tabs = tab_strip_->selection_model().size();
    if (commands.contains(tabs::CommandMoveToExistingWindow) &&
        ExistingWindowSubMenuModel::ShouldShowSubmenuForApp(
            tab_menu_model_delegate_)) {
      add_to_existing_window_submenu_ = ExistingWindowSubMenuModel::Create(
          delegate(), tab_menu_model_delegate_, tab_strip_, index);
      AddSubMenu(TabStripModel::CommandMoveToExistingWindow,
                 l10n_util::GetPluralStringFUTF16(
                     IDS_TAB_CXMENU_MOVETOANOTHERWINDOW, num_tabs),
                 add_to_existing_window_submenu_.get());
    } else if (commands.contains(tabs::CommandMoveTabsToNewWindow)) {
      AddItem(TabStripModel::CommandMoveTabsToNewWindow,
              l10n_util::GetPluralStringFUTF16(
                  IDS_TAB_CXMENU_MOVE_TABS_TO_NEW_WINDOW, num_tabs));
    }
  }

  // 4. Add "Close" actions.
  const bool add_close_tab =
      !is_pinned_home && commands.contains(tabs::CommandCloseTab);
  const bool add_close_other_tabs =
      !is_pinned_home && commands.contains(tabs::CommandCloseOtherTabs);
  const bool add_close_all_tabs =
      has_pinned_home && commands.contains(tabs::CommandCloseAllTabs);
  const bool add_close_tabs_to_right =
      commands.contains(tabs::CommandCloseTabsToRight);
  if (GetItemCount() > 0 && (add_close_tab || add_close_other_tabs ||
                             add_close_all_tabs || add_close_tabs_to_right)) {
    AddSeparator(ui::NORMAL_SEPARATOR);
  }
  if (add_close_tab) {
    AddItemWithStringId(TabStripModel::CommandCloseTab,
                        IDS_TAB_CXMENU_CLOSETAB);
  }
  if (add_close_other_tabs) {
    AddItemWithStringId(TabStripModel::CommandCloseOtherTabs,
                        IDS_TAB_CXMENU_CLOSEOTHERTABS);
  }
  if (add_close_all_tabs) {
    AddItemWithStringId(TabStripModel::CommandCloseAllTabs,
                        IDS_TAB_CXMENU_CLOSEALLTABS);
  }
  if (add_close_tabs_to_right) {
    AddItemWithStringId(TabStripModel::CommandCloseTabsToRight,
                        base::i18n::IsRTL() ? IDS_TAB_CXMENU_CLOSETABSTOLEFT
                                            : IDS_TAB_CXMENU_CLOSETABSTORIGHT);
  }
}

void TabMenuModel::BuildSendTabToSelfSubmenu(int index,
                                             const std::vector<int>& indices) {
  std::vector<content::WebContents*> web_contents_list;
  web_contents_list.reserve(indices.size());
  for (int i : indices) {
    web_contents_list.push_back(tab_strip_->GetWebContentsAt(i));
  }

  send_tab_to_self_submenu_delegate_ =
      std::make_unique<send_tab_to_self::SendTabToSelfContextMenuDelegate>(
          tab_strip_->GetWebContentsAt(index), web_contents_list,
          send_tab_to_self::ShareEntryPoint::kTabMenu);
  send_tab_to_self_submenu_ = std::make_unique<ui::SimpleMenuModel>(
      send_tab_to_self_submenu_delegate_.get());

  send_tab_to_self_submenu_delegate_->PopulateSubmenu(
      send_tab_to_self_submenu_.get());

#if BUILDFLAG(IS_MAC)
  if (features::IsMenuSimplificationEnabled()) {
    AddSubMenuWithStringIdAndIcon(
        TabStripModel::CommandSendTabToSelf, IDS_CONTEXT_MENU_SEND_TAB_TO_SELF,
        send_tab_to_self_submenu_.get(),
        ui::ImageModel::FromVectorIcon(
            features::IsRoundedIconsEnabled() ? kDevicesIcon : kDevicesOldIcon,
            ui::kColorMenuIcon, kTabMenuIconSize));
  } else {
    AddSubMenuWithStringId(TabStripModel::CommandSendTabToSelf,
                           IDS_CONTEXT_MENU_SEND_TAB_TO_SELF,
                           send_tab_to_self_submenu_.get());
  }
#else
  AddSubMenuWithStringIdAndIcon(
      TabStripModel::CommandSendTabToSelf, IDS_CONTEXT_MENU_SEND_TAB_TO_SELF,
      send_tab_to_self_submenu_.get(),
      ui::ImageModel::FromVectorIcon(
          features::IsRoundedIconsEnabled() ? kDevicesIcon : kDevicesOldIcon,
          ui::kColorMenuIcon, kTabMenuIconSize));
#endif

  SetElementIdentifierAt(GetItemCount() - 1, kTabSendTabToSelfMenuItem);

  // TODO(crbug.com/516708776): Remove new feature tag when no longer new.
  SetIsNewFeatureAt(GetItemCount() - 1,
                    UserEducationService::MaybeShowNewBadge(
                        tab_strip_->profile(),
                        send_tab_to_self::kSendTabToSelfEnhancedDesktopUI));
}

void TabMenuModel::BuildLegacySendTabToSelfItem() {
#if BUILDFLAG(IS_MAC)
  if (features::IsMenuSimplificationEnabled()) {
    AddItemWithIcon(
        TabStripModel::CommandSendTabToSelf,
        l10n_util::GetStringUTF16(IDS_CONTEXT_MENU_SEND_TAB_TO_SELF),
        ui::ImageModel::FromVectorIcon(features::IsRoundedIconsEnabled()
                                           ? kDevicesIcon
                                           : kDevicesOldIcon));
  } else {
    AddItem(TabStripModel::CommandSendTabToSelf,
            l10n_util::GetStringUTF16(IDS_CONTEXT_MENU_SEND_TAB_TO_SELF));
  }
#else
  AddItemWithIcon(
      TabStripModel::CommandSendTabToSelf,
      l10n_util::GetStringUTF16(IDS_CONTEXT_MENU_SEND_TAB_TO_SELF),
      ui::ImageModel::FromVectorIcon(
          features::IsRoundedIconsEnabled() ? kDevicesIcon : kDevicesOldIcon));
#endif
}

void TabMenuModel::AppendGlicItems(int index,
                                   int num_tabs,
                                   const std::vector<int>& indices) {
  glic_tab_sub_menu_model_ =
      std::make_unique<glic::GlicTabSubMenuModel>(tab_strip_, index);

  if (features::IsMenuSimplificationEnabled()) {
    AddSubMenuWithIcon(TabStripModel::CommandGlicShare,
                       l10n_util::GetPluralStringFUTF16(
                           IDS_TAB_CXMENU_GLIC_START_SHARE, num_tabs),
                       glic_tab_sub_menu_model_.get(),
                       ui::ImageModel::FromVectorIcon(
                           glic::GlicVectorIconManager::GetVectorIcon(
                               IDR_GLIC_BUTTON_VECTOR_ICON),
                           ui::kColorMenuIcon, kTabMenuIconSize));
  } else {
    AddSubMenu(TabStripModel::CommandGlicShare,
               l10n_util::GetPluralStringFUTF16(IDS_TAB_CXMENU_GLIC_START_SHARE,
                                                num_tabs),
               glic_tab_sub_menu_model_.get());
  }

  auto* service =
      glic::GlicKeyedServiceFactory::GetGlicKeyedService(tab_strip_->profile());
  CHECK(service);
  if (std::ranges::any_of(indices, [&](int index) {
        return service->instance_coordinator().IsTabPinnedToAnyInstance(
            tab_strip_->GetTabAtIndex(index)->GetHandle());
      })) {
    AddItem(TabStripModel::CommandGlicUnshare,
            l10n_util::GetStringUTF16(IDS_TAB_CXMENU_GLIC_UNSHARE));
  }
}

// Capitalization Policy (go/chrome-capitalization):
// Native right-click tab context menus on macOS use Title Case (`_MAC` string
// variants) to follow Apple Human Interface Guidelines (HIG), while other
// platforms use sentence case.
void TabMenuModel::Build(int index) {
  std::vector<int> indices;
  if (tab_strip_->IsTabSelected(index)) {
    const ui::ListSelectionModel::SelectedIndices sel =
        tab_strip_->selection_model()
            .GetListSelectionModel()
            .selected_indices();
    indices = std::vector<int>(sel.begin(), sel.end());
  } else {
    indices = {index};
  }

  int num_tabs = indices.size();

  auto* controller = tabs::VerticalTabStripStateController::From(
      tab_strip_->delegate()->GetBrowserWindowInterface());
  bool showing_vertical_tabs =
      controller && controller->ShouldDisplayVerticalTabs();

  if (showing_vertical_tabs) {
    AddItemWithStringId(TabStripModel::CommandNewTabToRight,
                        IDS_TAB_CXMENU_NEWTABBELOW);
  } else {
    AddItemWithStringId(TabStripModel::CommandNewTabToRight,
                        base::i18n::IsRTL() ? IDS_TAB_CXMENU_NEWTABTOLEFT
                                            : IDS_TAB_CXMENU_NEWTABTORIGHT);
  }
  SetElementIdentifierAt(GetItemCount() - 1, kAddNewTabAdjacentMenuItem);

  const auto split_icon = ui::ImageModel::FromVectorIcon(
      features::IsRoundedIconsEnabled() ? kSplitSceneIcon : kSplitSceneOldIcon,
      ui::kColorMenuIcon, kTabMenuIconSize);
  if (!tab_strip_->GetSplitForTab(index).has_value()) {
    split_layout_submenu_ =
        std::make_unique<SplitWithTabMenuModel>(tab_strip_, index);
    AddSubMenuWithIcon(TabStripModel::CommandAddToSplit, u"Split with tab",
                       split_layout_submenu_.get(), split_icon);
    SetElementIdentifierAt(GetItemCount() - 1, kSplitTabsMenuItem);
    if (tab_strip_->GetActiveTab()->IsSplit()) {
      swap_with_split_submenu_ =
          std::make_unique<SplitTabSwapMenuModel>(tab_strip_, index);
      AddSubMenuWithStringIdAndIcon(
          TabStripModel::CommandSwapWithActiveSplit,
          IDS_TAB_CXMENU_SWAP_WITH_ACTIVE_SPLIT, swap_with_split_submenu_.get(),
          split_icon);
      SetEnabledAt(GetItemCount() - 1, num_tabs == 1);
      SetElementIdentifierAt(GetItemCount() - 1, kSwapSplitTabsMenuItem);
    }
  } else {
    split_layout_submenu_ = std::make_unique<SplitWithTabMenuModel>(tab_strip_, index);
    AddSubMenuWithIcon(TabStripModel::CommandAddToSplit, u"Add tab to split",
                       split_layout_submenu_.get(), split_icon);
    AddItemWithIcon(kSeparateTabsCommand, u"Separate tabs", split_icon);
    arrange_split_view_submenu_ = std::make_unique<SplitTabMenuModel>(
        tab_strip_, SplitTabMenuModel::MenuSource::kTabContextMenu, index);
    AddSubMenuWithStringIdAndIcon(
        TabStripModel::CommandArrangeSplit, IDS_TAB_CXMENU_ARRANGE_SPLIT,
        arrange_split_view_submenu_.get(), split_icon);
    SetElementIdentifierAt(GetItemCount() - 1, kArrangeSplitTabsMenuItem);
  }

  if (tab_strip_->SupportsTabGroups() &&
      base::FeatureList::IsEnabled(features::kTabGroupsFocusing)) {
    if (std::optional<tab_groups::TabGroupId> group =
            tab_strip_->GetCommonGroupForIndices(indices)) {
      const bool is_focused = tab_strip_->GetFocusedGroup() == group;
      AddItemWithStringId(TabStripModel::CommandToggleFocusGroup,
                          is_focused ? IDS_TAB_CXMENU_UNFOCUS_TAB_GROUP
                                     : IDS_TAB_CXMENU_FOCUS_TAB_GROUP);
      SetElementIdentifierAt(GetItemCount() - 1, kFocusSelectionMenuItem);
    } else if (base::FeatureList::IsEnabled(features::kNonGroupFocus) &&
               tab_strip_->AreAllUngrouped(indices)) {
      AddItem(TabStripModel::CommandToggleFocusGroup,
              l10n_util::GetPluralStringFUTF16(IDS_TAB_CXMENU_FOCUS_TABS,
                                               num_tabs));
      SetElementIdentifierAt(GetItemCount() - 1, kFocusSelectionMenuItem);
    }
  }

  if (ExistingTabGroupSubMenuModel::ShouldShowSubmenu(
          tab_strip_, index, tab_menu_model_delegate_)) {
    // Create submenu with existing groups
    add_to_existing_group_submenu_ =
        std::make_unique<ExistingTabGroupSubMenuModel>(
            delegate(), tab_menu_model_delegate_, tab_strip_, index);
    AddSubMenu(TabStripModel::CommandAddToExistingGroup,
               l10n_util::GetPluralStringFUTF16(IDS_TAB_CXMENU_ADD_TAB_TO_GROUP,
                                                num_tabs),
               add_to_existing_group_submenu_.get());
    if (features::IsMenuSimplificationEnabled()) {
      SetIconForCommandId(
          TabStripModel::CommandAddToExistingGroup,
          ui::ImageModel::FromVectorIcon(
              features::IsRoundedIconsEnabled()
                  ? kGridViewIcon
                  : kSavedTabGroupBarEverythingOldIcon,
              ui::kColorMenuIcon, ui::SimpleMenuModel::kDefaultIconSize));
    }
  } else {
    AddItem(TabStripModel::CommandAddToNewGroup,
            l10n_util::GetPluralStringFUTF16(
                IDS_TAB_CXMENU_ADD_TAB_TO_NEW_GROUP, num_tabs));
    SetElementIdentifierAt(GetItemCount() - 1, kAddToNewGroupItemIdentifier);
    if (features::IsMenuSimplificationEnabled()) {
      SetIconForCommandId(
          TabStripModel::CommandAddToNewGroup,
          ui::ImageModel::FromVectorIcon(
              features::IsRoundedIconsEnabled()
                  ? kGridViewIcon
                  : kSavedTabGroupBarEverythingOldIcon,
              ui::kColorMenuIcon, ui::SimpleMenuModel::kDefaultIconSize));
    }
  }

  for (const auto& selection : indices) {
    std::optional<tab_groups::TabGroupId> group =
        tab_strip_->GetTabGroupForTab(selection);
    if (group.has_value() && !tab_strip_->IsEphemeralTabGroup(group.value())) {
      AddItemWithStringId(TabStripModel::CommandRemoveFromGroup,
                          IDS_TAB_CXMENU_REMOVE_TAB_FROM_GROUP);
      break;
    }
  }

  if (ExistingWindowSubMenuModel::ShouldShowSubmenu(tab_strip_->profile())) {
    // Create submenu with existing windows
    add_to_existing_window_submenu_ = ExistingWindowSubMenuModel::Create(
        delegate(), tab_menu_model_delegate_, tab_strip_, index);
    AddSubMenu(TabStripModel::CommandMoveToExistingWindow,
               l10n_util::GetPluralStringFUTF16(
                   IDS_TAB_CXMENU_MOVETOANOTHERWINDOW, num_tabs),
               add_to_existing_window_submenu_.get());
    if (features::IsMenuSimplificationEnabled()) {
      SetIconForCommandId(
          TabStripModel::CommandMoveToExistingWindow,
          ui::ImageModel::FromVectorIcon(
              features::IsRoundedIconsEnabled() ? kOpenInNewIcon
                                                : kOpenInNewOldIcon,
              ui::kColorMenuIcon, ui::SimpleMenuModel::kDefaultIconSize));
    }
  } else {
    AddItem(TabStripModel::CommandMoveTabsToNewWindow,
            l10n_util::GetPluralStringFUTF16(
                IDS_TAB_CXMENU_MOVE_TABS_TO_NEW_WINDOW, num_tabs));
    if (features::IsMenuSimplificationEnabled()) {
      SetIconForCommandId(
          TabStripModel::CommandMoveTabsToNewWindow,
          ui::ImageModel::FromVectorIcon(
              features::IsRoundedIconsEnabled() ? kOpenInNewIcon
                                                : kOpenInNewOldIcon,
              ui::kColorMenuIcon, ui::SimpleMenuModel::kDefaultIconSize));
    }
  }

  AddSeparator(ui::NORMAL_SEPARATOR);
  AddItemWithStringId(TabStripModel::CommandCopyURL,
                      num_tabs == 1 ? IDS_TAB_CXMENU_COPY_URL
                                    : IDS_TAB_CXMENU_COPY_URLS);

  AddSeparator(ui::NORMAL_SEPARATOR);
  AddItemWithStringId(TabStripModel::CommandReload, IDS_TAB_CXMENU_RELOAD);

  AddItemWithStringId(TabStripModel::CommandDuplicate,
                      IDS_TAB_CXMENU_DUPLICATE);
  SetElementIdentifierAt(GetItemCount() - 1, kDuplicateMenuItem);

  bool will_pin = tab_strip_->WillContextMenuPin(index);
  AddItemWithStringId(
      TabStripModel::CommandTogglePinned,
      will_pin ? IDS_TAB_CXMENU_PIN_TAB : IDS_TAB_CXMENU_UNPIN_TAB);

  if (features::IsMenuSimplificationEnabled()) {
    SetIconForCommandId(
        TabStripModel::CommandTogglePinned,
        ui::ImageModel::FromVectorIcon(
            will_pin ? features::IsRoundedIconsEnabled() ? views::kKeepIcon
                                                         : views::kPinOldIcon
            : features::IsRoundedIconsEnabled() ? views::kKeepFilledIcon
                                                : views::kUnpinOldIcon,
            ui::kColorMenuIcon, ui::SimpleMenuModel::kDefaultIconSize));
  }

  if (tab_interface_ && tab_interface_->IsPinned()) {
    AddItem(kEditPinnedTabCommand, u"Edit pinned tab…");
    AddItem(kResetPinnedTabCommand, u"Reset to home");
    AddItem(kUseCurrentPinnedHomeCommand, u"Use current page as home");
  }

  const bool will_mute_tab = tab_strip_->WillContextMenuMuteTabs(index);
  AddItem(TabStripModel::CommandToggleTabMuted,
          l10n_util::GetPluralStringFUTF16(
              will_mute_tab ? IDS_TAB_CXMENU_SOUND_MUTE_TAB
                            : IDS_TAB_CXMENU_SOUND_UNMUTE_TAB,
              num_tabs));

  const bool will_mute = !AreAllSitesMuted(*tab_strip_, indices);
  AddItem(TabStripModel::CommandToggleSiteMuted,
          will_mute ? l10n_util::GetPluralStringFUTF16(
                          IDS_TAB_CXMENU_SOUND_MUTE_SITE, num_tabs)
                    : l10n_util::GetPluralStringFUTF16(
                          IDS_TAB_CXMENU_SOUND_UNMUTE_SITE, num_tabs));

  if (features::IsMenuSimplificationEnabled()) {
    SetIconForCommandId(
        TabStripModel::CommandToggleSiteMuted,
        ui::ImageModel::FromVectorIcon(
            will_mute ? features::IsRoundedIconsEnabled()
                            ? vector_icons::kVolumeOffIcon
                            : vector_icons::kVolumeOffChromeRefreshOldIcon
            : features::IsRoundedIconsEnabled()
                ? vector_icons::kVolumeUpIcon
                : vector_icons::kVolumeUpChromeRefreshOldIcon,
            ui::kColorMenuIcon, ui::SimpleMenuModel::kDefaultIconSize));
  }

  const bool show_glic_items =
      glic::GlicEnabling::IsReadyForProfile(tab_strip_->profile());
  bool glic_displayed = false;
  if (features::IsMenuSimplificationEnabled() && show_glic_items) {
    AddSeparator(ui::NORMAL_SEPARATOR);
    AppendGlicItems(index, num_tabs, indices);
    AddSeparator(ui::NORMAL_SEPARATOR);
    glic_displayed = true;
  }

  const bool display_read_later = tab_strip_->delegate()->SupportsReadLater();
  const std::optional<send_tab_to_self::EntryPointDisplayReason>
      send_tab_to_self_reason = send_tab_to_self::GetEntryPointDisplayReason(
          tab_strip_->GetWebContentsAt(index));
  const bool display_send_to_self = send_tab_to_self_reason.has_value();

  if ((display_read_later || display_send_to_self) && !glic_displayed) {
    AddSeparator(ui::NORMAL_SEPARATOR);
  }

  if (display_read_later) {
    AddItemWithIcon(
        TabStripModel::CommandAddToReadLater,
        l10n_util::GetPluralStringFUTF16(IDS_TAB_CXMENU_READ_LATER, num_tabs),
        features::IsRoundedIconsEnabled()
            ? ui::ImageModel::FromVectorIcon(kListAltIcon)
            : ui::ImageModel::FromVectorIcon(kMenuBookChromeRefreshOldIcon,
                                             ui::kColorMenuIcon,
                                             kTabMenuIconSize));
    SetEnabledAt(GetItemCount() - 1,
                 tab_strip_->IsReadLaterSupportedForAny(indices));
  }

  if (show_glic_items && !glic_displayed) {
    AppendGlicItems(index, num_tabs, indices);
  }

  if (display_send_to_self) {
    if (base::FeatureList::IsEnabled(
            send_tab_to_self::kSendTabToSelfEnhancedDesktopUI) &&
        send_tab_to_self_reason ==
            send_tab_to_self::EntryPointDisplayReason::kOfferFeature) {
      BuildSendTabToSelfSubmenu(index, indices);
    } else {
      BuildLegacySendTabToSelfItem();
    }
  }

  // Append extension items for the 'tab' context if the feature is enabled.
#if BUILDFLAG(ENABLE_EXTENSIONS)
  if (base::FeatureList::IsEnabled(
          extensions_features::kExtensionTabContextMenu)) {
    extension_items_ = std::make_unique<extensions::ContextMenuMatcher>(
        tab_strip_->profile(), delegate(), this,
        base::BindRepeating([](const extensions::MenuItem* item) {
          return item->contexts().Contains(extensions::MenuItem::TAB);
        }));

    int extension_index = 0;
    // Iterate over all extensions that have menu items loaded in the
    // MenuManager.
    for (const auto& key :
         extensions::MenuManager::Get(tab_strip_->profile())->ExtensionIds()) {
      extension_items_->AppendExtensionItems(key, std::u16string(),
                                             &extension_index,
                                             /*is_action_menu=*/false);
    }
  }
#endif

  AddSeparator(ui::NORMAL_SEPARATOR);
  AddItemWithStringId(TabStripModel::CommandDiscard,
                      tab_strip_->IsTabInForeground(index)
                          ? IDS_TAB_CXMENU_HIBERNATE_OTHER_TABS
                          : IDS_TAB_CXMENU_HIBERNATE);

  // Separator Close Tab items
  AddSeparator(ui::NORMAL_SEPARATOR);
  AddItemWithStringId(TabStripModel::CommandCloseTab, IDS_TAB_CXMENU_CLOSETAB);
  AddItemWithStringId(TabStripModel::CommandCloseOtherTabs,
                      IDS_TAB_CXMENU_CLOSEOTHERTABS);

  if (showing_vertical_tabs) {
    AddItemWithStringId(TabStripModel::CommandCloseTabsToLeft,
                        IDS_TAB_CXMENU_CLOSETABSABOVE);
  } else {
    AddItemWithStringId(TabStripModel::CommandCloseTabsToLeft,
                        base::i18n::IsRTL() ? IDS_TAB_CXMENU_CLOSETABSTORIGHT
                                            : IDS_TAB_CXMENU_CLOSETABSTOLEFT);
  }

  SetEnabledAt(GetItemCount() - 1,
               tab_strip_->IsContextMenuCommandEnabled(
                   index, TabStripModel::CommandCloseTabsToLeft));

  if (showing_vertical_tabs) {
    AddItemWithStringId(TabStripModel::CommandCloseTabsToRight,
                        IDS_TAB_CXMENU_CLOSETABSBELOW);
  } else {
    AddItemWithStringId(TabStripModel::CommandCloseTabsToRight,
                        base::i18n::IsRTL() ? IDS_TAB_CXMENU_CLOSETABSTOLEFT
                                            : IDS_TAB_CXMENU_CLOSETABSTORIGHT);
  }
  SetEnabledAt(GetItemCount() - 1,
               tab_strip_->IsContextMenuCommandEnabled(
                   index,
                   TabStripModel::CommandCloseTabsToRight));
}

DEFINE_CLASS_ELEMENT_IDENTIFIER_VALUE(TabMenuModel,
                                      kAddToNewGroupItemIdentifier);

#if BUILDFLAG(ENABLE_EXTENSIONS)
// Helper to retrieve the extension matcher if item at 'index' is an extension.
extensions::ContextMenuMatcher* TabMenuModel::GetMatcherIfExtension(
    size_t index) const {
  int command_id = GetCommandIdAt(index);
  if (extensions::ContextMenuMatcher::IsExtensionsCustomCommandId(command_id)) {
    return extension_items_.get();
  }
  return nullptr;
}
#endif

// Overridden to check if the checked status of the item
// should be evaluated by the extension matcher instead of the simple model.
bool TabMenuModel::IsItemCheckedAt(size_t index) const {
  if (IsPinnedTabCommand(GetCommandIdAt(index))) {
    return false;
  }
#if BUILDFLAG(ENABLE_EXTENSIONS)
  if (auto* matcher = GetMatcherIfExtension(index)) {
    return matcher->IsCommandIdChecked(GetCommandIdAt(index));
  }
#endif
  return ui::SimpleMenuModel::IsItemCheckedAt(index);
}

// Overridden to check if the enabled status of the item
// should be evaluated by the extension matcher instead of the simple model.
bool TabMenuModel::IsEnabledAt(size_t index) const {
  const int command_id = GetCommandIdAt(index);
  if (IsPinnedTabCommand(command_id)) {
    if (!tab_interface_ || !tab_interface_->IsPinned() ||
        !tab_strip_->ContainsIndex(
            tab_strip_->GetIndexOfTab(tab_interface_.get()))) {
      return false;
    }
    auto* contents = tab_interface_->GetContents();
    if (command_id == kResetPinnedTabCommand) {
      const auto* data = noventra::GetPinnedTabData(contents);
      return data && noventra::IsValidPinnedHomeUrl(data->home_url);
    }
    return command_id != kUseCurrentPinnedHomeCommand ||
           noventra::IsValidPinnedHomeUrl(contents->GetVisibleURL());
  }
  if (command_id == kSeparateTabsCommand ||
      command_id == TabStripModel::CommandAddToSplit) {
    if (!tab_interface_ || !tab_strip_->ContainsIndex(
                              tab_strip_->GetIndexOfTab(tab_interface_.get()))) {
      return false;
    }
    if (command_id == kSeparateTabsCommand) {
      return tab_interface_->IsSplit();
    }
    return !tab_interface_->IsSplit() ||
           tab_strip_->GetSplitData(tab_interface_->GetSplit().value())
                   ->ListTabs()
                   .size() < 4;
  }
#if BUILDFLAG(ENABLE_EXTENSIONS)
  if (auto* matcher = GetMatcherIfExtension(index)) {
    return matcher->IsCommandIdEnabled(GetCommandIdAt(index));
  }
#endif
  return ui::SimpleMenuModel::IsEnabledAt(index);
}

// Overridden to check if the visible status of the item
// should be evaluated by the extension matcher instead of the simple model.
bool TabMenuModel::IsVisibleAt(size_t index) const {
#if BUILDFLAG(ENABLE_EXTENSIONS)
  if (auto* matcher = GetMatcherIfExtension(index)) {
    return matcher->IsCommandIdVisible(GetCommandIdAt(index));
  }
#endif
  return ui::SimpleMenuModel::IsVisibleAt(index);
}

void TabMenuModel::ActivatedAt(size_t index) {
  ActivatedAt(index, 0);
}

// Overridden to execute core context menu command logic on activation for
// extension items.
void TabMenuModel::ActivatedAt(size_t index, int event_flags) {
  const int command_id = GetCommandIdAt(index);
  if (IsPinnedTabCommand(command_id)) {
    if (!IsEnabledAt(index)) {
      return;
    }
    auto* contents = tab_interface_->GetContents();
    if (command_id == kEditPinnedTabCommand) {
      // Let native menu teardown finish before the modal takes keyboard focus.
      base::SingleThreadTaskRunner::GetCurrentDefault()->PostTask(
          FROM_HERE,
          base::BindOnce(
              &ShowPinnedTabEditor, tab_interface_,
              tab_interface_->GetBrowserWindowInterface()->GetWeakPtr()));
    } else if (command_id == kResetPinnedTabCommand) {
      noventra::ResetPinnedTabToHome(contents);
    } else {
      std::u16string name;
      if (const auto* data = noventra::GetPinnedTabData(contents)) {
        name = data->custom_name;
      }
      noventra::SetPinnedTabData(contents, contents->GetVisibleURL(), name);
    }
    return;
  }
  if (GetCommandIdAt(index) == kSeparateTabsCommand) {
    if (IsEnabledAt(index)) {
      tab_strip_->RemoveSplit(*tab_interface_->GetSplit());
    }
    return;
  }
#if BUILDFLAG(ENABLE_EXTENSIONS)
  if (auto* matcher = GetMatcherIfExtension(index)) {
    content::WebContents* web_contents =
        tab_interface_ ? tab_interface_->GetContents() : nullptr;
    // The underlying tab could be closed, crashed, or moved in the background
    // while the context menu is still open.
    if (web_contents) {
      // Create minimal ContextMenuParams sufficient for executing the command
      // for 'contextMenus' extension API. E.g. providing the current page
      // URL.
      content::ContextMenuParams params;
      params.page_url = web_contents->GetLastCommittedURL();
      matcher->ExecuteCommand(GetCommandIdAt(index), web_contents, nullptr,
                              params);
    }
    return;
  }
#endif
  ui::SimpleMenuModel::ActivatedAt(index, event_flags);
}
