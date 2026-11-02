// Copyright 2026 The Helium Authors
// You can use, redistribute, and/or modify this source code under
// the terms of the GPL-3.0 license that can be found in the LICENSE file.

#include "chrome/browser/ui/noventra_commands.h"
#include "chrome/browser/ui/browser_shortcuts/browser_shortcut_metadata.h"

#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "base/check.h"
#include "build/build_config.h"
#include "chrome/app/chrome_command_ids.h"
#include "chrome/browser/ui/helium/helium_layout_state_controller.h"
#include "chrome/common/pref_names.h"
#include "chrome/grit/generated_resources.h"
#include "components/prefs/pref_service.h"
#include "ui/strings/grit/ui_strings.h"

namespace browser_shortcuts {

namespace {

bool ShouldUseVerticalCollapseLabel(const PrefService* prefs) {
  return prefs && prefs->GetBoolean(prefs::kVerticalCollapseShortcut) &&
         prefs->GetInteger(prefs::kHeliumLayout) ==
             std::to_underlying(HeliumLayoutType::kVertical);
}

bool ShouldUseCopyPageUrlLabel(const PrefService* prefs) {
  return prefs && prefs->GetBoolean(prefs::kCopyPageUrlShortcut);
}

bool ShouldHideBrowserShortcutCommand(int command_id) {
  switch (command_id) {
    case IDC_COPY:
    case IDC_CUT:
    case IDC_DEBUG_TOGGLE_TABLET_MODE:
    case IDC_DEBUG_PRINT_VIEW_TREE:
    case IDC_DEBUG_PRINT_VIEW_TREE_DETAILS:
#if BUILDFLAG(IS_MAC)
    case IDC_EXIT:
#endif
    case IDC_HELP_PAGE_VIA_MENU:
    case IDC_HIDE_APP:
    case IDC_MINIMIZE_WINDOW:
    case IDC_PASTE:
    case IDC_TOGGLE_FULLSCREEN_TOOLBAR:
    case IDC_CONTENT_CONTEXT_COPY:
    case IDC_CONTENT_CONTEXT_CUT:
    case IDC_CONTENT_CONTEXT_PASTE:
    case IDC_CONTENT_CONTEXT_PASTE_AND_MATCH_STYLE:
    case IDC_CONTENT_CONTEXT_REDO:
    case IDC_CONTENT_CONTEXT_SELECTALL:
    case IDC_CONTENT_CONTEXT_UNDO:
      return true;
    default:
      return false;
  }
}

BrowserShortcutCommandLabel Label(int message_id) {
  return {message_id, std::nullopt};
}

BrowserShortcutCommandLabel Label(int message_id, std::u16string replacement) {
  return {message_id, std::move(replacement)};
}

}  // namespace

std::optional<BrowserShortcutCommandLabel> GetBrowserShortcutCommandLabel(
    int command_id,
    const PrefService* prefs) {
  if (ShouldHideBrowserShortcutCommand(command_id)) {
    return std::nullopt;
  }

  switch (command_id) {
    case IDC_NOVENTRA_SELECT_SPACE_0: return {BrowserShortcutCommandLabel{0, u"Switch to space 1"}};
    case IDC_NOVENTRA_SELECT_SPACE_1: return {BrowserShortcutCommandLabel{0, u"Switch to space 2"}};
    case IDC_NOVENTRA_SELECT_SPACE_2: return {BrowserShortcutCommandLabel{0, u"Switch to space 3"}};
    case IDC_NOVENTRA_SELECT_SPACE_3: return {BrowserShortcutCommandLabel{0, u"Switch to space 4"}};
    case IDC_NOVENTRA_SELECT_SPACE_4: return {BrowserShortcutCommandLabel{0, u"Switch to space 5"}};
    case IDC_NOVENTRA_SELECT_SPACE_5: return {BrowserShortcutCommandLabel{0, u"Switch to space 6"}};
    case IDC_NOVENTRA_SELECT_SPACE_6: return {BrowserShortcutCommandLabel{0, u"Switch to space 7"}};
    case IDC_NOVENTRA_SELECT_SPACE_7: return {BrowserShortcutCommandLabel{0, u"Switch to space 8"}};
    case IDC_NOVENTRA_SELECT_SPACE_8: return {BrowserShortcutCommandLabel{0, u"Switch to space 9"}};
    case IDC_NOVENTRA_NEXT_SPACE: return {BrowserShortcutCommandLabel{0, u"Next account space"}};
    case IDC_NOVENTRA_PREVIOUS_SPACE: return {BrowserShortcutCommandLabel{0, u"Previous account space"}};
    case IDC_NOVENTRA_GRID_SPLIT:
      return {BrowserShortcutCommandLabel{0, u"Toggle grid split layout"}};
    case IDC_NOVENTRA_MANAGE_SPACES: return {BrowserShortcutCommandLabel{0, u"Manage account spaces"}};
    case IDC_NOVENTRA_ZEN: return {BrowserShortcutCommandLabel{0, u"Toggle Zen mode"}};
    case IDC_NOVENTRA_SIDEBAR: return {BrowserShortcutCommandLabel{0, u"Show or hide sidebar"}};
    case IDC_NOVENTRA_SIDEBAR_SIDE: return {BrowserShortcutCommandLabel{0, u"Move sidebar to the other side"}};
    case IDC_NOVENTRA_CUSTOMIZE: return {BrowserShortcutCommandLabel{0, u"Customize Noventra"}};
    case IDC_NOVENTRA_SHORTCUTS: return {BrowserShortcutCommandLabel{0, u"Edit keyboard shortcuts"}};
    case IDC_BACK:
      return Label(IDS_SHORTCUT_COMMAND_BACK);
    case IDC_FORWARD:
      return Label(IDS_SHORTCUT_COMMAND_FORWARD);
    case IDC_FORCE_PASTE:
      return Label(IDS_FORCE_PASTE);
    case IDC_HOME:
      return Label(IDS_SHORTCUT_COMMAND_HOME);
    case IDC_RELOAD:
      return Label(IDS_APP_MENU_RELOAD);
    case IDC_RELOAD_BYPASSING_CACHE:
      return Label(IDS_RELOAD_MENU_HARD_RELOAD_ITEM);
    case IDC_RELOAD_CLEARING_CACHE:
      return Label(IDS_RELOAD_MENU_EMPTY_AND_HARD_RELOAD_ITEM);
    case IDC_STOP:
      return Label(IDS_TOOLTIP_STOP);
    case IDC_NEW_WINDOW:
      return Label(IDS_NEW_WINDOW);
    case IDC_NEW_INCOGNITO_WINDOW:
      return Label(IDS_NEW_INCOGNITO_WINDOW);
    case IDC_NEW_TAB:
      return Label(IDS_NEW_TAB);
    case IDC_NEW_SPLIT_TAB:
      return Label(IDS_SHORTCUT_COMMAND_NEW_SPLIT_TAB);
    case IDC_DUPLICATE_TAB:
      return Label(IDS_SHORTCUT_COMMAND_DUPLICATE_TAB);
    case IDC_EXIT:
      return Label(IDS_EXIT);
    case IDC_MOVE_TAB_TO_NEW_WINDOW:
      return Label(IDS_MOVE_TAB_TO_NEW_WINDOW);
    case IDC_NEW_TAB_TO_RIGHT:
      return Label(IDS_TAB_CXMENU_NEWTABTORIGHT);
    case IDC_WINDOW_CLOSE_TABS_TO_RIGHT:
      return Label(IDS_TAB_CXMENU_CLOSETABSTORIGHT);
    case IDC_WINDOW_CLOSE_OTHER_TABS:
      return Label(IDS_TAB_CXMENU_CLOSEOTHERTABS);
    case IDC_WINDOW_PIN_TAB:
      return Label(IDS_SHORTCUT_COMMAND_TOGGLE_PIN_TAB);
    case IDC_SHOW_AS_TAB:
      return Label(IDS_SHOW_AS_TAB);
    case IDC_OPEN_IN_PWA_WINDOW:
      return Label(IDS_INTENT_CHIP_OPEN_IN_APP);
    case IDC_WEB_APP_SETTINGS:
      return Label(IDS_WEB_APP_SETTINGS_TITLE);
    case IDC_WEB_APP_MENU_APP_INFO:
      return Label(IDS_APP_CONTEXT_MENU_SHOW_INFO);
    case IDC_CLOSE_TAB:
      return Label(IDS_TAB_SEARCH_CLOSE_TAB);
    case IDC_CLOSE_WINDOW:
      return Label(IDS_CLOSE_WARNING_FOR_CLEAR_COOKIES_ON_EXIT_CLOSE);
    case IDC_RESTORE_TAB:
      return Label(IDS_RESTORE_TAB);
    case IDC_SELECT_NEXT_TAB:
    case IDC_CYCLE_TO_NEXT_TAB:
      return Label(IDS_SHORTCUT_COMMAND_SELECT_NEXT_TAB);
    case IDC_SELECT_PREVIOUS_TAB:
    case IDC_CYCLE_TO_PREV_TAB:
      return Label(IDS_SHORTCUT_COMMAND_SELECT_PREVIOUS_TAB);
    case IDC_SELECT_LAST_TAB:
      return Label(IDS_SHORTCUT_COMMAND_SELECT_LAST_TAB);
    case IDC_SELECT_TAB_0:
      return Label(IDS_SHORTCUT_COMMAND_SELECT_TAB, u"1");
    case IDC_SELECT_TAB_1:
      return Label(IDS_SHORTCUT_COMMAND_SELECT_TAB, u"2");
    case IDC_SELECT_TAB_2:
      return Label(IDS_SHORTCUT_COMMAND_SELECT_TAB, u"3");
    case IDC_SELECT_TAB_3:
      return Label(IDS_SHORTCUT_COMMAND_SELECT_TAB, u"4");
    case IDC_SELECT_TAB_4:
      return Label(IDS_SHORTCUT_COMMAND_SELECT_TAB, u"5");
    case IDC_SELECT_TAB_5:
      return Label(IDS_SHORTCUT_COMMAND_SELECT_TAB, u"6");
    case IDC_SELECT_TAB_6:
      return Label(IDS_SHORTCUT_COMMAND_SELECT_TAB, u"7");
    case IDC_SELECT_TAB_7:
      return Label(IDS_SHORTCUT_COMMAND_SELECT_TAB, u"8");
    case IDC_MOVE_TAB_NEXT:
      return Label(IDS_SHORTCUT_COMMAND_MOVE_TAB_NEXT);
    case IDC_MOVE_TAB_PREVIOUS:
      return Label(IDS_SHORTCUT_COMMAND_MOVE_TAB_PREVIOUS);
    case IDC_ADD_NEW_TAB_TO_GROUP:
      return Label(IDS_TAB_GROUP_HEADER_CXMENU_NEW_TAB_IN_GROUP);
    case IDC_CREATE_NEW_TAB_GROUP:
      return Label(IDS_NEW_TAB_GROUP);
    case IDC_FOCUS_NEXT_TAB_GROUP:
      return Label(IDS_SHORTCUT_COMMAND_FOCUS_NEXT_TAB_GROUP);
    case IDC_FOCUS_PREV_TAB_GROUP:
      return Label(IDS_SHORTCUT_COMMAND_FOCUS_PREVIOUS_TAB_GROUP);
    case IDC_CLOSE_TAB_GROUP:
      return Label(IDS_SHORTCUT_COMMAND_CLOSE_TAB_GROUP);
    case IDC_TAB_SEARCH:
      return Label(IDS_TAB_SEARCH_MENU);
    case IDC_BOOKMARK_THIS_TAB:
      return Label(IDS_BOOKMARK_THIS_TAB);
    case IDC_BOOKMARK_ALL_TABS:
      return Label(IDS_BOOKMARK_ALL_TABS);
    case IDC_SHOW_BOOKMARK_BAR:
      return Label(IDS_SHOW_BOOKMARK_BAR);
    case IDC_SHOW_BOOKMARK_MANAGER:
      return Label(IDS_BOOKMARK_MANAGER);
    case IDC_FOCUS_BOOKMARKS:
      return Label(IDS_SHORTCUT_COMMAND_FOCUS_BOOKMARKS);
    case IDC_SHOW_HISTORY:
      return Label(IDS_HISTORY_MENU);
    case IDC_SHOW_DOWNLOADS:
      return Label(IDS_SHOW_DOWNLOADS);
    case IDC_CREATE_SHORTCUT:
      return Label(IDS_ADD_TO_OS_LAUNCH_SURFACE);
    case IDC_INSTALL_PWA:
      return Label(IDS_INSTALL_DIY_TO_OS_LAUNCH_SURFACE);
    case IDC_ROUTE_MEDIA:
      return Label(IDS_MEDIA_ROUTER_MENU_ITEM_TITLE);
    case IDC_WINDOW_MUTE_SITE:
      return Label(IDS_SHORTCUT_COMMAND_TOGGLE_MUTE_SITE);
    case IDC_QRCODE_GENERATOR:
      return Label(IDS_SHARING_HUB_GENERATE_QR_CODE_LABEL);
    case IDC_SHARING_HUB_SCREENSHOT:
      return Label(IDS_SHARING_HUB_SCREENSHOT_LABEL);
    case IDC_SHOW_FULL_URLS:
      return Label(IDS_CONTEXT_MENU_SHOW_FULL_URLS);
    case IDC_OPEN_FILE:
      return Label(IDS_OPEN_FILE_DIALOG_TITLE);
    case IDC_SAVE_PAGE:
      return Label(IDS_SAVE_PAGE);
    case IDC_CTRL_S_SHORTCUT:
      return Label(ShouldUseVerticalCollapseLabel(prefs)
                       ? IDS_SHORTCUT_COMMAND_TOGGLE_VERTICAL_TABS
                       : IDS_SAVE_PAGE);
    case IDC_TOGGLE_VERTICAL_TABS_COLLAPSE:
      return Label(IDS_SHORTCUT_COMMAND_TOGGLE_VERTICAL_TABS);
    case IDC_PRINT:
      return Label(IDS_CONTENT_CONTEXT_PRINT);
    case IDC_BASIC_PRINT:
      return Label(IDS_PRINT_PREVIEW_SYSTEM_DIALOG_OPTION, u"");
    case IDC_FIND:
      return Label(IDS_FIND);
    case IDC_FIND_NEXT:
      return Label(IDS_SHORTCUT_COMMAND_FIND_NEXT);
    case IDC_FIND_PREVIOUS:
      return Label(IDS_SHORTCUT_COMMAND_FIND_PREVIOUS);
    case IDC_CLOSE_FIND_OR_STOP:
      return Label(IDS_SHORTCUT_COMMAND_CLOSE_FIND_OR_STOP);
    case IDC_FOCUS_LOCATION:
      return Label(IDS_SHORTCUT_COMMAND_FOCUS_LOCATION);
    case IDC_FOCUS_SEARCH:
      return Label(IDS_SHORTCUT_COMMAND_FOCUS_SEARCH);
    case IDC_FOCUS_NEXT_PANE:
      return Label(IDS_SHORTCUT_COMMAND_FOCUS_NEXT_PANE);
    case IDC_FOCUS_PREVIOUS_PANE:
      return Label(IDS_SHORTCUT_COMMAND_FOCUS_PREVIOUS_PANE);
    case IDC_FOCUS_WEB_CONTENTS_PANE:
      return Label(IDS_SHORTCUT_COMMAND_FOCUS_WEB_CONTENTS_PANE);
    case IDC_FOCUS_MENU_BAR:
      return Label(IDS_SHORTCUT_COMMAND_FOCUS_MENU_BAR);
    case IDC_FOCUS_TOOLBAR:
      return Label(IDS_SHORTCUT_COMMAND_FOCUS_TOOLBAR);
    case IDC_FOCUS_INACTIVE_POPUP_FOR_ACCESSIBILITY:
      return Label(IDS_SHORTCUT_COMMAND_FOCUS_INACTIVE_POPUP);
    case IDC_ZOOM_PLUS:
      return Label(IDS_ZOOM_PLUS);
    case IDC_ZOOM_MINUS:
      return Label(IDS_ZOOM_MINUS);
    case IDC_ZOOM_NORMAL:
      return Label(IDS_ZOOM_NORMAL);
    case IDC_FULLSCREEN:
      return Label(IDS_FULLSCREEN);
    case IDC_SHOW_AVATAR_MENU:
      return Label(IDS_SHORTCUT_COMMAND_SHOW_AVATAR_MENU);
    case IDC_SHOW_APP_MENU:
      return Label(IDS_SHORTCUT_COMMAND_SHOW_APP_MENU);
    case IDC_TOGGLE_ZEN_MODE_TOP_CHROME_PIN:
      return Label(IDS_SHORTCUT_COMMAND_TOGGLE_ZEN_MODE_TOP_CHROME_PIN);
    case IDC_CARET_BROWSING_TOGGLE:
      return Label(IDS_SETTINGS_ENABLE_CARET_BROWSING_TITLE);
    case IDC_CLEAR_BROWSING_DATA:
      return Label(IDS_CLEAR_BROWSING_DATA);
    case IDC_IMPORT_SETTINGS:
      return Label(IDS_IMPORT_SETTINGS_MENU_LABEL);
    case IDC_MANAGE_EXTENSIONS:
      return Label(IDS_SHOW_EXTENSIONS);
    case IDC_DEV_TOOLS_DEVICES:
      return Label(IDS_DEV_TOOLS_DEVICES);
    case IDC_SHOW_MANAGEMENT_PAGE:
      return Label(IDS_OPENS_MANAGEMENT_PAGE);
    case IDC_PERFORMANCE:
      return Label(IDS_SHOW_PERFORMANCE);
    case IDC_BROWSER_LAYOUT_CLASSIC:
      return Label(IDS_SHORTCUT_COMMAND_SWITCH_TO_CLASSIC_LAYOUT);
    case IDC_BROWSER_LAYOUT_COMPACT:
      return Label(IDS_SHORTCUT_COMMAND_SWITCH_TO_COMPACT_LAYOUT);
    case IDC_BROWSER_LAYOUT_VERTICAL:
      return Label(IDS_SHORTCUT_COMMAND_SWITCH_TO_VERTICAL_LAYOUT);
    case IDC_OPTIONS:
      return Label(IDS_SETTINGS);
    case IDC_HELP_PAGE_VIA_KEYBOARD:
      return Label(IDS_HELP_PAGE);
    case IDC_TASK_MANAGER_SHORTCUT:
      return Label(IDS_TASK_MANAGER);
    case IDC_FEEDBACK:
      return Label(IDS_FEEDBACK);
    case IDC_SHOW_READING_MODE_KEYBOARD:
      return Label(IDS_READING_MODE_TITLE);
    case IDC_DEV_TOOLS_TOGGLE:
      return Label(IDS_SHORTCUT_COMMAND_TOGGLE_DEV_TOOLS);
    case IDC_DEV_TOOLS:
      return Label(IDS_DEV_TOOLS);
    case IDC_DEV_TOOLS_CONSOLE:
      return Label(IDS_DEV_TOOLS_CONSOLE);
    case IDC_DEV_TOOLS_INSPECT:
      return Label(IDS_DEV_TOOLS_ELEMENTS);
    case IDC_COPY_URL:
      return Label(IDS_COPY_URL);
    case IDC_COPY_OR_INSPECT_SHORTCUT:
      return Label(ShouldUseCopyPageUrlLabel(prefs) ? IDS_COPY_URL
                                                    : IDS_DEV_TOOLS_ELEMENTS);
    case IDC_VIEW_SOURCE:
      return Label(IDS_VIEW_SOURCE);
  }

  DCHECK(false) << "Missing browser shortcut label for command id "
                << command_id;
  return std::nullopt;
}

std::vector<int> GetBrowserShortcutCommandsWithoutDefault(
    const PrefService* prefs) {
  std::vector<int> commands;
  if (ShouldUseVerticalCollapseLabel(prefs)) {
    commands.push_back(IDC_SAVE_PAGE);
  }
  commands.push_back(IDC_TOGGLE_VERTICAL_TABS_COLLAPSE);

  commands.push_back(ShouldUseCopyPageUrlLabel(prefs) ? IDC_DEV_TOOLS_INSPECT
                                                      : IDC_COPY_URL);
  commands.push_back(IDC_FORCE_PASTE);
  commands.push_back(IDC_DUPLICATE_TAB);
#if !BUILDFLAG(IS_MAC)
  commands.push_back(IDC_EXIT);
#endif
  commands.push_back(IDC_MOVE_TAB_TO_NEW_WINDOW);
  commands.push_back(IDC_NEW_TAB_TO_RIGHT);
  commands.push_back(IDC_WINDOW_CLOSE_TABS_TO_RIGHT);
  commands.push_back(IDC_WINDOW_CLOSE_OTHER_TABS);
  commands.push_back(IDC_WINDOW_PIN_TAB);
  commands.push_back(IDC_SHOW_AS_TAB);
  commands.push_back(IDC_OPEN_IN_PWA_WINDOW);
  commands.push_back(IDC_WEB_APP_SETTINGS);
  commands.push_back(IDC_WEB_APP_MENU_APP_INFO);
  commands.push_back(IDC_CREATE_SHORTCUT);
  commands.push_back(IDC_INSTALL_PWA);
  commands.push_back(IDC_ROUTE_MEDIA);
  commands.push_back(IDC_WINDOW_MUTE_SITE);
  commands.push_back(IDC_QRCODE_GENERATOR);
  commands.push_back(IDC_SHARING_HUB_SCREENSHOT);
  commands.push_back(IDC_SHOW_FULL_URLS);
  commands.push_back(IDC_RELOAD_CLEARING_CACHE);
  commands.push_back(IDC_IMPORT_SETTINGS);
  commands.push_back(IDC_MANAGE_EXTENSIONS);
  commands.push_back(IDC_DEV_TOOLS_DEVICES);
  commands.push_back(IDC_SHOW_MANAGEMENT_PAGE);
  commands.push_back(IDC_PERFORMANCE);
  commands.push_back(IDC_BROWSER_LAYOUT_CLASSIC);
  commands.push_back(IDC_BROWSER_LAYOUT_COMPACT);
  commands.push_back(IDC_BROWSER_LAYOUT_VERTICAL);
  commands.push_back(IDC_OPTIONS);
  for (int command :
       {IDC_NOVENTRA_ZEN, IDC_NOVENTRA_SIDEBAR, IDC_NOVENTRA_SIDEBAR_SIDE,
        IDC_NOVENTRA_CUSTOMIZE, IDC_NOVENTRA_SHORTCUTS, IDC_NOVENTRA_NEXT_SPACE,
        IDC_NOVENTRA_PREVIOUS_SPACE, IDC_NOVENTRA_MANAGE_SPACES,
        IDC_NOVENTRA_GRID_SPLIT}) {
    commands.push_back(command);
  }
  for (int command = IDC_NOVENTRA_SELECT_SPACE_0;
       command <= IDC_NOVENTRA_SELECT_SPACE_8; ++command) {
    commands.push_back(command);
  }
  return commands;
}

bool IsBrowserShortcutDefaultUserModifiable(int command_id) {
  return GetBrowserShortcutCommandLabel(command_id, nullptr).has_value();
}

}  // namespace browser_shortcuts
