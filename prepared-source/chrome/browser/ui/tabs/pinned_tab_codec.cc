// Copyright 2012 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/ui/tabs/pinned_tab_codec.h"

#include <stddef.h>

#include <map>
#include <utility>

#include "base/uuid.h"
#include "base/values.h"
#include "chrome/browser/profiles/noventra_account_spaces.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/ui/browser_window/public/browser_window_interface.h"
#include "chrome/browser/ui/browser_window/public/browser_window_interface_iterator.h"
#include "chrome/browser/ui/noventra/pinned_tab_state.h"
#include "chrome/browser/ui/tabs/tab_strip_model.h"
#include "chrome/common/pref_names.h"
#include "components/pref_registry/pref_registry_syncable.h"
#include "components/prefs/pref_service.h"
#include "components/prefs/scoped_user_pref_update.h"
#include "content/public/browser/navigation_controller.h"
#include "content/public/browser/navigation_entry.h"
#include "content/public/browser/web_contents.h"

using content::NavigationEntry;

namespace {

// Key used in dictionaries for the url.
const char kURL[] = "url";

// Returns a base::DictValue representing the supplied StartupTab.
base::DictValue EncodeTab(
    const GURL& url,
    const std::map<std::string, std::string>& extra_data) {
  base::DictValue dict;
  dict.Set(kURL, url.spec());
  for (const char* key :
       {noventra::kSpaceExtraData, noventra::kPinnedTabExtraData}) {
    if (auto it = extra_data.find(key); it != extra_data.end()) {
      dict.Set(key, it->second);
    }
  }
  return dict;
}

// Encodes all the pinned tabs from `browser` into `serialized_tabs`.
void EncodePinnedTabs(BrowserWindowInterface* browser,
                      base::ListValue& serialized_tabs) {
  const TabStripModel* const tab_model = browser->GetTabStripModel();
  for (const tabs::TabInterface* tab : *tab_model) {
    if (!tab->IsPinned()) {
      break;
    }

    content::WebContents* web_contents = tab->GetContents();
    NavigationEntry* entry =
        web_contents->GetController().GetLastCommittedEntry();
    if (entry) {
      std::map<std::string, std::string> extra_data;
      extra_data[noventra::kSpaceExtraData] = noventra::SpaceId(web_contents);
      noventra::PopulatePinnedTabExtraData(web_contents, &extra_data);
      serialized_tabs.Append(EncodeTab(entry->GetURL(), extra_data));
    }
  }
}

// Decodes the previously written values in `value` to `tab`, returning true
// on success.
std::optional<StartupTab> DecodeTab(const base::DictValue& value) {
  const std::string* const url_string = value.FindString(kURL);
  if (!url_string) {
    return std::nullopt;
  }
  const std::string* space = value.FindString(noventra::kSpaceExtraData);
  if ((value.Find(noventra::kSpaceExtraData) && !space) ||
      (space && !space->empty() &&
       !base::Uuid::ParseLowercase(*space).is_valid())) {
    // A malformed nondefault identity must never load in Personal instead.
    return std::nullopt;
  }
  StartupTab tab(GURL(*url_string), StartupTab::Type::kPinned);
  if (!tab.url.is_valid()) {
    return std::nullopt;
  }
  if (space) {
    tab.extra_data[noventra::kSpaceExtraData] = *space;
  }
  if (const auto* data = value.FindString(noventra::kPinnedTabExtraData)) {
    tab.extra_data[noventra::kPinnedTabExtraData] = *data;
  }
  return tab;
}

}  // namespace

// static
void PinnedTabCodec::RegisterProfilePrefs(
    user_prefs::PrefRegistrySyncable* registry) {
  registry->RegisterListPref(prefs::kPinnedTabs);
}

// static
void PinnedTabCodec::WritePinnedTabs(Profile* profile) {
  PrefService* prefs = profile->GetPrefs();
  if (!prefs) {
    return;
  }

  base::ListValue values;
  ForEachCurrentBrowserWindowInterfaceOrderedByActivation(
      [profile, &values](BrowserWindowInterface* browser) {
        if (browser->GetType() == BrowserWindowInterface::TYPE_NORMAL &&
            browser->GetProfile() == profile) {
          EncodePinnedTabs(browser, values);
        }
        return true;
      });
  prefs->SetList(prefs::kPinnedTabs, std::move(values));
}

// static
void PinnedTabCodec::WritePinnedTabs(Profile* profile,
                                     const StartupTabs& tabs) {
  PrefService* prefs = profile->GetPrefs();
  if (!prefs) {
    return;
  }

  ScopedListPrefUpdate update(prefs, prefs::kPinnedTabs);
  base::ListValue& values = update.Get();
  values.clear();
  for (const auto& tab : tabs) {
    values.Append(EncodeTab(tab.url, tab.extra_data));
  }
}

// static
StartupTabs PinnedTabCodec::ReadPinnedTabs(Profile* profile) {
  PrefService* prefs = profile->GetPrefs();
  if (!prefs) {
    return {};
  }

  StartupTabs results;

  for (const auto& serialized_tab : prefs->GetList(prefs::kPinnedTabs)) {
    if (!serialized_tab.is_dict()) {
      continue;
    }
    std::optional<StartupTab> tab = DecodeTab(serialized_tab.GetDict());
    if (tab.has_value()) {
      results.push_back(tab.value());
    }
  }

  return results;
}
