// Copyright 2026 The Noventra Authors. All rights reserved.

#include "chrome/browser/ui/noventra/workspace_routes.h"

#include <algorithm>
#include <string_view>
#include <utility>

#include "base/strings/string_util.h"
#include "base/uuid.h"
#include "base/values.h"
#include "chrome/browser/profiles/noventra_space_profile_registry.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/ui/browser_window/public/browser_window_interface.h"
#include "chrome/browser/ui/noventra_commands.h"
#include "components/prefs/pref_service.h"
#include "url/gurl.h"

namespace noventra {
namespace {

bool HasWorkspaceRoutes(Profile* profile) {
  if (!profile || profile->IsOffTheRecord()) {
    return false;
  }
  auto* catalog = SpaceCatalogProfile(profile);
  return catalog && catalog->GetPrefs()->FindPreference(kWorkspaceRoutesPref);
}

void SaveWorkspaceRoutes(Profile* profile,
                         const std::vector<WorkspaceRoute>& routes) {
  base::ListValue value;
  for (const auto& route : routes) {
    base::DictValue entry;
    entry.Set("host", route.host);
    entry.Set("space_id", route.space_id);
    value.Append(std::move(entry));
  }
  SpaceCatalogProfile(profile)->GetPrefs()->SetList(kWorkspaceRoutesPref,
                                                    std::move(value));
}

}  // namespace

base::expected<std::string, std::string> CanonicalWorkspaceRouteHost(
    const std::string& host) {
  std::string input;
  base::TrimWhitespaceASCII(host, base::TRIM_ALL, &input);
  constexpr char kInvalidHost[] =
      "Enter a hostname such as example.com, without a URL, port or wildcard.";
  if (input.empty() || input.size() > 2048 || !base::IsStringUTF8(input) ||
      input.find_first_of("/@?#\\%*") != std::string::npos ||
      (input.front() == '[' ? !input.ends_with(']')
                            : input.find(':') != std::string::npos) ||
      std::ranges::any_of(
          input, [](unsigned char c) { return c <= 0x20 || c == 0x7f; })) {
    return base::unexpected(kInvalidHost);
  }
  // GURL supplies the same IDN, case and IP canonicalization as navigation.
  const GURL url("https://" + input + "/");
  if (!url.is_valid() || !url.has_host() || url.has_username() ||
      url.has_password() || url.has_port() || url.path() != "/" ||
      url.has_query() || url.has_ref()) {
    return base::unexpected(kInvalidHost);
  }
  std::string canonical(url.host());
  if (url.HostIsIPAddress()) {
    return base::ok(std::move(canonical));
  }
  if (canonical.ends_with('.')) {
    canonical.pop_back();
  }
  if (canonical.empty() || canonical.size() > 253) {
    return base::unexpected(kInvalidHost);
  }
  // GURL also accepts some non-DNS hosts. Reject empty, oversized or malformed
  // labels so saved rules are literal hostnames, not permissive URL syntax.
  std::string_view remaining(canonical);
  while (!remaining.empty()) {
    const size_t dot = remaining.find('.');
    const auto label = remaining.substr(0, dot);
    if (label.empty() || label.size() > 63 || label.front() == '-' ||
        label.back() == '-' || !std::ranges::all_of(label, [](char c) {
          return base::IsAsciiAlphaNumeric(c) || c == '-';
        })) {
      return base::unexpected(kInvalidHost);
    }
    if (dot == std::string_view::npos) {
      break;
    }
    remaining.remove_prefix(dot + 1);
    if (remaining.empty()) {
      return base::unexpected(kInvalidHost);
    }
  }
  return base::ok(std::move(canonical));
}

bool CanManageWorkspaceRoutes(Profile* profile) {
  return HasWorkspaceRoutes(profile) && !profile->ShutdownStarted() &&
         !SpaceCatalogProfile(profile)->ShutdownStarted() &&
         SpaceCatalogProfile(profile)
             ->GetPrefs()
             ->FindPreference(kWorkspaceRoutesPref)
             ->IsUserModifiable();
}

std::vector<WorkspaceRoute> GetWorkspaceRoutes(Profile* profile) {
  std::vector<WorkspaceRoute> routes;
  if (!HasWorkspaceRoutes(profile)) {
    return routes;
  }
  for (const auto& value : SpaceCatalogProfile(profile)->GetPrefs()->GetList(
           kWorkspaceRoutesPref)) {
    const auto* entry = value.GetIfDict();
    const auto* host = entry ? entry->FindString("host") : nullptr;
    const auto* id = entry ? entry->FindString("space_id") : nullptr;
    if (!host || !id ||
        (!id->empty() && !base::Uuid::ParseLowercase(*id).is_valid())) {
      continue;
    }
    auto canonical = CanonicalWorkspaceRouteHost(*host);
    if (!canonical.has_value() ||
        std::ranges::find(routes, *canonical, &WorkspaceRoute::host) !=
            routes.end()) {
      continue;
    }
    routes.push_back({std::move(*canonical), *id});
    if (routes.size() == kMaxWorkspaceRoutes) {
      break;
    }
  }
  return routes;
}

base::expected<void, std::string> SetWorkspaceRoute(
    BrowserWindowInterface* browser,
    const std::string& host,
    const std::string& space_id) {
  if (!browser || !CanManageWorkspaceRoutes(browser->GetProfile())) {
    return base::unexpected(
        "Workspace routing cannot be changed in this profile.");
  }
  auto canonical = CanonicalWorkspaceRouteHost(host);
  if (!canonical.has_value()) {
    return base::unexpected(canonical.error());
  }
  const auto ids = SpaceIds(browser, /*include_archived=*/true);
  if (std::ranges::find(ids, space_id) == ids.end() ||
      IsSpaceArchived(browser->GetProfile(), space_id)) {
    return base::unexpected("Choose an existing, non-archived space.");
  }
  auto routes = GetWorkspaceRoutes(browser->GetProfile());
  auto existing = std::ranges::find(routes, *canonical, &WorkspaceRoute::host);
  if (existing != routes.end()) {
    existing->space_id = space_id;
  } else {
    if (routes.size() >= kMaxWorkspaceRoutes) {
      return base::unexpected(
          "Remove a route before adding more than 100 hosts.");
    }
    routes.push_back({std::move(*canonical), space_id});
  }
  SaveWorkspaceRoutes(browser->GetProfile(), routes);
  return base::ok();
}

base::expected<void, std::string> RemoveWorkspaceRoute(
    Profile* profile,
    const std::string& host) {
  if (!CanManageWorkspaceRoutes(profile)) {
    return base::unexpected(
        "Workspace routing cannot be changed in this profile.");
  }
  auto canonical = CanonicalWorkspaceRouteHost(host);
  if (!canonical.has_value()) {
    return base::unexpected(canonical.error());
  }
  auto routes = GetWorkspaceRoutes(profile);
  if (!std::erase_if(routes, [&](const WorkspaceRoute& route) {
        return route.host == *canonical;
      })) {
    return base::unexpected("This workspace route no longer exists.");
  }
  SaveWorkspaceRoutes(profile, routes);
  return base::ok();
}

std::optional<std::string> ResolveWorkspaceRoute(
    BrowserWindowInterface* browser,
    const GURL& url) {
  if (!browser || !HasWorkspaceRoutes(browser->GetProfile()) ||
      !url.is_valid() || !url.SchemeIsHTTPOrHTTPS()) {
    return std::nullopt;
  }
  auto canonical = CanonicalWorkspaceRouteHost(std::string(url.host()));
  if (!canonical.has_value()) {
    return std::nullopt;
  }
  const auto routes = GetWorkspaceRoutes(browser->GetProfile());
  const auto route =
      std::ranges::find(routes, *canonical, &WorkspaceRoute::host);
  if (route == routes.end() ||
      IsSpaceArchived(browser->GetProfile(), route->space_id)) {
    return std::nullopt;
  }
  const auto ids = SpaceIds(browser, /*include_archived=*/true);
  if (std::ranges::find(ids, route->space_id) == ids.end()) {
    return std::nullopt;
  }
  return route->space_id;
}

}  // namespace noventra
