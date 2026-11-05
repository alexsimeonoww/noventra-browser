// Copyright 2026 Noventra contributors
// SPDX-License-Identifier: GPL-3.0-only
#ifndef CHROME_BROWSER_IMPORTER_NOVENTRA_COOKIE_PROFILE_WRITER_H_
#define CHROME_BROWSER_IMPORTER_NOVENTRA_COOKIE_PROFILE_WRITER_H_
#include <cstdint>
#include <vector>

#include "base/functional/callback.h"
#include "base/memory/weak_ptr.h"
#include "content/public/browser/storage_partition_config.h"
#include "net/cookies/canonical_cookie.h"
class Profile;
namespace noventra {
void WriteImportedCookies(
    base::WeakPtr<Profile> profile,
    const content::StoragePartitionConfig& config,
    const std::vector<net::CanonicalCookie>& cookies,
    uint32_t skipped,
    base::RepeatingCallback<bool()> can_continue,
    base::RepeatingCallback<void(uint32_t, uint32_t)> summary,
    base::OnceCallback<void(bool)> callback);
}
#endif
