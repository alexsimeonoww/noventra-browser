// Copyright 2026 The Noventra Authors. All rights reserved.
#ifndef CHROME_BROWSER_PROFILES_NOVENTRA_ACCOUNT_SPACES_H_
#define CHROME_BROWSER_PROFILES_NOVENTRA_ACCOUNT_SPACES_H_

#include <map>
#include <string>

#include "base/uuid.h"
#include "chrome/browser/profiles/noventra_space_profile_registry.h"
#include "chrome/browser/profiles/profile.h"
#include "content/public/browser/navigation_controller.h"
#include "content/public/browser/security_principal.h"
#include "content/public/browser/session_storage_namespace_handle.h"
#include "content/public/browser/site_instance.h"
#include "content/public/browser/storage_partition_config.h"
#include "content/public/browser/web_contents.h"

namespace noventra {
inline constexpr char kSpacesPref[] = "noventra.browser.account_spaces";
inline constexpr char kSpaceOrderPref[] =
    "noventra.browser.account_space_order";
inline constexpr char kArchivedSpacesPref[] =
    "noventra.browser.archived_account_spaces";
inline constexpr char kSpaceAppearancePref[] =
    "noventra.browser.space_appearance";
inline constexpr char kSpaceExtraData[] = "noventra.account_space";
inline constexpr char kSpacePartitionDomain[] = "noventra-account-spaces";

// A regular child Profile owns its Space identity. Private Spaces and legacy
// regular tabs keep their partition identity; labels never change ownership.
inline std::string SpaceId(content::WebContents* contents) {
  if (!contents) return {};
  auto* profile = Profile::FromBrowserContext(contents->GetBrowserContext());
  if (!profile->IsOffTheRecord()) {
    const auto id = ProfileSpaceId(profile);
    if (!id.empty()) {
      return id;
    }
  }
  const auto& config = contents->GetSiteInstance()->GetSecurityPrincipal()
                           .GetStoragePartitionConfig();
  return config.partition_domain() == kSpacePartitionDomain
             ? config.partition_name() : std::string();
}

inline content::StoragePartitionConfig SpaceConfig(
    Profile* profile, const std::string& id) {
  if (ProfileSpacesEnabled() && !profile->IsOffTheRecord()) {
    const auto own_id = ProfileSpaceId(profile);
    CHECK(id == own_id);
    const auto storage = ProfileSpaceStorage(profile);
    if (storage == SpaceProfileStorage::kLegacyPartition) {
      return content::StoragePartitionConfig::Create(
          profile, kSpacePartitionDomain, own_id, false);
    }
    return content::StoragePartitionConfig::CreateDefault(profile);
  }
  if (id.empty())
    return content::StoragePartitionConfig::CreateDefault(profile);
  // IDs are generated locally and persisted independently of display names.
  // Invalid nonempty IDs must never silently restore into Personal's cookie jar.
  CHECK(base::Uuid::ParseLowercase(id).is_valid());
  return content::StoragePartitionConfig::Create(
      profile, kSpacePartitionDomain, id, profile->IsOffTheRecord());
}

// Fail closed if an engine change stops honoring the requested partition.
inline scoped_refptr<content::SiteInstance> SpaceSiteInstance(
    Profile* profile, const GURL& url, const std::string& id) {
  const auto config = SpaceConfig(profile, id);
  auto instance = config.is_default()
                      ? content::SiteInstance::CreateForURL(profile, url)
                      : content::SiteInstance::CreateForFixedStoragePartition(
                            profile, url, config);
  CHECK(instance->GetSecurityPrincipal().GetStoragePartitionConfig() == config);
  return instance;
}

inline std::string RestoredSpaceId(
    const std::map<std::string, std::string>& extra_data) {
  auto it = extra_data.find(kSpaceExtraData);
  return it == extra_data.end() ? std::string() : it->second;
}

inline content::SessionStorageNamespaceHandle* SpaceSessionStorage(
    content::WebContents* contents) {
  const auto& config = contents->GetSiteInstance()
                           ->GetSecurityPrincipal()
                           .GetStoragePartitionConfig();
  if (config.is_default()) {
    return contents->GetController().GetDefaultSessionStorageNamespace();
  }
  const auto& namespaces = contents->GetController().GetSessionStorageNamespaceMap();
  auto it = namespaces.find(config);
  return it == namespaces.end() ? nullptr : it->second.get();
}
}  // namespace noventra
#endif
