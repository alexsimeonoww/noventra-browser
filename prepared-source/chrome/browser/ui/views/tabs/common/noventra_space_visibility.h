// Copyright 2026 The Noventra Authors. All rights reserved.
#ifndef CHROME_BROWSER_UI_VIEWS_TABS_COMMON_NOVENTRA_SPACE_VISIBILITY_H_
#define CHROME_BROWSER_UI_VIEWS_TABS_COMMON_NOVENTRA_SPACE_VISIBILITY_H_
#include "chrome/browser/profiles/noventra_account_spaces.h"
#include "chrome/browser/ui/browser_window/public/browser_window_interface.h"
#include "chrome/browser/ui/tabs/tab_strip_model.h"
#include "chrome/browser/ui/views/tabs/common/tab_view.h"
#include "ui/views/view_utils.h"
namespace noventra {
// Groups and split rows are visible if they contain a tab in the current space.
inline bool SpaceViewVisible(const views::View* view) {
  bool saw_tab = false;
  auto matches = [&saw_tab](auto&& self, const views::View* node) -> bool {
    if (const auto* tab_view = views::AsViewClass<TabView>(node)) {
      const auto* tab = tab_view->GetTabInterface();
      if (!tab || !tab->GetBrowserWindowInterface()) return true;
      saw_tab = true;
      auto* model = tab->GetBrowserWindowInterface()->GetTabStripModel();
      return SpaceId(tab->GetContents()) == SpaceId(model->GetActiveWebContents());
    }
    bool matched = false;
    for (const auto& child : node->children()) matched |= self(self, child.get());
    return matched;
  };
  const bool matched = matches(matches, view);
  return matched || !saw_tab;
}
}
#endif
