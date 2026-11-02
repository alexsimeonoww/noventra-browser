// Copyright 2025 The Helium Authors
// You can use, redistribute, and/or modify this source code under
// the terms of the GPL-3.0 license that can be found in the LICENSE file.

#ifndef COMPONENTS_HELIUM_SERVICES_PREF_NAMES_H_
#define COMPONENTS_HELIUM_SERVICES_PREF_NAMES_H_

namespace prefs {

inline constexpr char kHeliumSchemaVersion[] =
    "helium.services.schema_version";

inline constexpr char kHeliumDisableSchemaAlerts[] =
    "helium.services.disable_schema_alerts";

inline constexpr char kHeliumPrefPrefix[] = "helium.";

inline constexpr char kHeliumServicesEnabled[] =
    "helium.services.enabled";

inline constexpr char kHeliumServicesOrigin[] =
    "helium.services.origin_override";

inline constexpr char kHeliumServicesConsented[] =
    "helium.services.user_consented";

inline constexpr char kHeliumDidOnboarding[] =
    "helium.completed_onboarding";

inline constexpr char kHeliumExtProxyEnabled[] =
    "helium.services.ext_proxy";

// Explicit opt-in for Chrome Web Store downloads without a service proxy.
inline constexpr char kNoventraChromeWebStoreDownloads[] =
    "helium.extensions.chrome_web_store";

// Explicit opt-in for direct dictionary downloads without a service proxy.
inline constexpr char kNoventraDictionaryDownloads[] =
    "helium.downloads.spelling_dictionaries";

inline constexpr char kHeliumBangsEnabled[] =
    "helium.services.bangs";

inline constexpr char kHeliumUpdateFetchingEnabled[] =
    "helium.services.browser_updates";

inline constexpr char kHeliumSpellcheckEnabled[] =
    "helium.services.spellcheck_files";

inline constexpr char kHeliumUBlockAssetsEnabled[] =
    "helium.services.ublock_assets";
}  // namespace prefs

#endif  // COMPONENTS_HELIUM_SERVICES_PREF_NAMES_H_
