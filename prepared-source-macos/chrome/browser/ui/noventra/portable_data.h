// Copyright 2026 The Noventra Authors. All rights reserved.
#ifndef CHROME_BROWSER_UI_NOVENTRA_PORTABLE_DATA_H_
#define CHROME_BROWSER_UI_NOVENTRA_PORTABLE_DATA_H_

#include <cstddef>
#include <string>

#include "base/functional/callback.h"
#include "base/types/expected.h"
#include "base/values.h"
class BrowserWindowInterface;

namespace noventra {

// Register as a dictionary preference. Each transfer UUID maps to a dictionary
// with completed-category booleans, tab progress and a `space_map` dictionary.
// Password import owns the `passwords` boolean and writes it only after its
// native import ends.
inline constexpr char kPortableImportsPref[] =
    "noventra.browser.portable_imports";
inline constexpr size_t kPortableMaximumBytes = 32 * 1024 * 1024;
using PortableResult = base::expected<base::DictValue, std::string>;

// UI-sequence, regular-profile operations. Missing category flags mean false;
// tabs require spaces. Collect returns native-only plaintext for encryption,
// never a WebUI response. Password collection/import is a separate native gate.
PortableResult CollectPortableData(BrowserWindowInterface* browser,
                                   const base::DictValue& categories);

// Pure schema validation. The returned count-only review contains transfer_id,
// created_at, categories, bookmarks, bookmark_folders, spaces (not Personal),
// tabs, pins, settings, extensions, extension_items, excluded and
// password_present. It contains neither passwords nor page URLs or other
// imported contents. Optional passwords_csv is bounded but parsed by the native
// password module.
PortableResult ValidatePortableData(const base::DictValue& snapshot);

// Validates the whole snapshot and selected categories, policy and layout
// revision before mutation. Adds imported bookmarks under a dedicated folder,
// creates fresh signed-out account identities and preserves existing work.
// Already completed categories are skipped using kPortableImportsPref. There
// is no cross-database crash-atomic transaction; successful categories are
// recorded separately. Extension items are returned for reviewed reinstall;
// this function never installs code or grants extension permissions.
// Tabs are inserted asynchronously in their owning Profiles. Cancellation
// preserves completed categories and the exact inserted-tab progress.
void ApplyPortableData(BrowserWindowInterface* browser,
                       const base::DictValue& snapshot,
                       const base::DictValue& categories,
                       int expected_layout_revision,
                       base::OnceCallback<void(PortableResult)> callback,
                       base::RepeatingCallback<bool()> can_continue = {});

}  // namespace noventra

#endif  // CHROME_BROWSER_UI_NOVENTRA_PORTABLE_DATA_H_
