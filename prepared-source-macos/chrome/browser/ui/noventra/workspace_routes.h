// Copyright 2026 The Noventra Authors. All rights reserved.

#ifndef CHROME_BROWSER_UI_NOVENTRA_WORKSPACE_ROUTES_H_
#define CHROME_BROWSER_UI_NOVENTRA_WORKSPACE_ROUTES_H_

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

#include "base/types/expected.h"

class BrowserWindowInterface;
class GURL;
class Profile;

namespace noventra {

inline constexpr char kWorkspaceRoutesPref[] =
    "noventra.browser.workspace_routes";
inline constexpr size_t kMaxWorkspaceRoutes = 100;

struct WorkspaceRoute {
  std::string host;
  // Personal is the empty ID. Other IDs are lowercase space UUIDs.
  std::string space_id;
};

// Accepts a bare DNS/IDN name or IP host, never a URL, port or wildcard. Pure:
// it can also validate an import before any profile preferences are changed.
base::expected<std::string, std::string> CanonicalWorkspaceRouteHost(
    const std::string& host);

// Off-the-record profiles cannot read, edit or apply these routes. Mutations
// also require the route preference to be user-modifiable.
bool CanManageWorkspaceRoutes(Profile* profile);
// Retains valid rules whose targets are missing or archived, so they remain
// visible and removable. Reading never restores a space or rewrites routes.
std::vector<WorkspaceRoute> GetWorkspaceRoutes(Profile* profile);
base::expected<void, std::string> SetWorkspaceRoute(
    BrowserWindowInterface* browser,
    const std::string& host,
    const std::string& space_id);
base::expected<void, std::string> RemoveWorkspaceRoute(Profile* profile,
                                                       const std::string& host);

// Exact canonical host match for HTTP(S) only. Missing/archived targets are
// ignored. nullopt means no match; an engaged empty string means Personal.
std::optional<std::string> ResolveWorkspaceRoute(
    BrowserWindowInterface* browser,
    const GURL& url);

}  // namespace noventra

#endif  // CHROME_BROWSER_UI_NOVENTRA_WORKSPACE_ROUTES_H_
