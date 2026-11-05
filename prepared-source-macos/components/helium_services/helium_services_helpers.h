// Copyright 2025 The Helium Authors
// You can use, redistribute, and/or modify this source code under
// the terms of the GPL-3.0 license that can be found in the LICENSE file.

#ifndef COMPONENTS_HELIUM_SERVICES_HELIUM_SERVICES_HELPERS_H_
#define COMPONENTS_HELIUM_SERVICES_HELIUM_SERVICES_HELPERS_H_

#include "base/functional/bind.h"
#include "components/prefs/pref_change_registrar.h"
#include "components/prefs/pref_service.h"
#include "url/gurl.h"

namespace helium {

// Noventra has no bundled service backend. This reserved blocked host keeps
// disabled requests on the existing ERR_BLOCKED_BY_CLIENT path.
const char kHeliumDummyOrigin[] =
    "https://noventra-services-are-disabled.qjz9zk";

bool ShouldAccessServices(const PrefService& prefs);

COMPONENT_EXPORT(HELIUM) bool ShouldFetchBangs(const PrefService& prefs);
COMPONENT_EXPORT(HELIUM) bool ShouldAccessExtensionService(const PrefService& prefs);
COMPONENT_EXPORT(HELIUM) bool ShouldAccessUpdateService(const PrefService& prefs);
COMPONENT_EXPORT(HELIUM) bool ShouldAccessComponentUpdateService(const PrefService& prefs);
COMPONENT_EXPORT(HELIUM) bool ShouldAccessUBlockAssets(const PrefService& prefs);
COMPONENT_EXPORT(HELIUM) GURL GetServicesBaseURL(const PrefService& prefs);
COMPONENT_EXPORT(HELIUM) GURL GetDummyURL();
COMPONENT_EXPORT(HELIUM) GURL GetExtensionUpdateURL(const PrefService& prefs);
COMPONENT_EXPORT(HELIUM) GURL GetWebstoreSnippetURL(const PrefService& prefs, std::string_view id);
COMPONENT_EXPORT(HELIUM) GURL GetSpellcheckURL(const PrefService& prefs);
COMPONENT_EXPORT(HELIUM) GURL GetBrowserUpdateURL(const PrefService& prefs);
COMPONENT_EXPORT(HELIUM) GURL GetComponentUpdateURL(const PrefService* prefs);
COMPONENT_EXPORT(HELIUM) GURL GetUBlockAssetsURL(const PrefService& prefs);
COMPONENT_EXPORT(HELIUM) std::optional<GURL> GetValidUserOverridenURL(std::string_view user_url_);
COMPONENT_EXPORT(HELIUM) void ConfigurePrefChangeRegistrarFor(std::string_view pref_name,
                            PrefChangeRegistrar& registrar, const base::RepeatingClosure& observer);
}  // namespace helium

#endif  // COMPONENTS_HELIUM_SERVICES_HELIUM_SERVICES_HELPERS_H_
