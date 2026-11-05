// Copyright 2026 The Noventra Authors
#ifndef CHROME_BROWSER_UI_NOVENTRA_PINNED_TAB_STATE_H_
#define CHROME_BROWSER_UI_NOVENTRA_PINNED_TAB_STATE_H_

#include <stddef.h>

#include <map>
#include <string>
#include <string_view>

#include "url/gurl.h"

namespace content {
class WebContents;
}

namespace noventra {

// Optional per-tab display/navigation metadata, never an account identity.
// The single session value is written atomically and uses no transient tab IDs.
inline constexpr char kPinnedTabExtraData[] = "noventra.pinned_tab";
inline constexpr size_t kMaxPinnedHomeUrlBytes = 32 * 1024;

struct PinnedTabData {
  GURL home_url;
  std::u16string custom_name;
};

// Only persistent browser-page schemes without embedded credentials qualify;
// URL bytes and escaped metadata are bounded below session-command limits.
// Unsupported current pages remain valid pins; they have no reset destination
// until the user selects a valid home URL or the first valid page commits.
bool IsValidPinnedHomeUrl(const GURL& url);

// A borrowed pointer, valid until the next metadata mutation or contents
// destruction. Returns nullptr for ordinary tabs and pins without metadata.
// An empty home_url means a newly pinned page has no safe destination yet.
const PinnedTabData* GetPinnedTabData(const content::WebContents* contents);

// Ensure only initializes actual pinned tabs, capturing a home when absent or
// at the first valid primary-page commit. Set/Reset return false for ordinary
// tabs or invalid input; they never change the tab's storage partition.
void EnsurePinnedTabData(content::WebContents* contents);
bool SetPinnedTabData(content::WebContents* contents,
                      const GURL& home_url,
                      std::u16string_view custom_name);
void ClearPinnedTabData(content::WebContents* contents);
bool ResetPinnedTabToHome(content::WebContents* contents);

// Applies a custom name only to pinned tabs. Empty names retain the supplied
// real page-title fallback; no WebContents/document title is overwritten.
std::u16string PinnedTabDisplayTitle(const content::WebContents* contents,
                                     std::u16string_view fallback);

// Model hooks must not report temporary detachment as a user unpin.
void OnPinnedStateChanged(content::WebContents* contents, bool pinned);

// Serializes stored state even during temporary detachment/reconstruction,
// including an empty value for explicitly cleared metadata.
// Restore is called for pinned tabs before insertion/loading; it accepts only
// valid metadata and does not persist or navigate while reconstructing a tab.
void PopulatePinnedTabExtraData(const content::WebContents* contents,
                                std::map<std::string, std::string>* extra_data);
void RestorePinnedTabExtraData(
    content::WebContents* contents,
    const std::map<std::string, std::string>& extra_data);

}  // namespace noventra

#endif  // CHROME_BROWSER_UI_NOVENTRA_PINNED_TAB_STATE_H_
