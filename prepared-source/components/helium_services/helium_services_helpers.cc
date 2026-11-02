// Copyright 2025 The Helium Authors
// You can use, redistribute, and/or modify this source code under
// the terms of the GPL-3.0 license that can be found in the LICENSE file.

#include "components/helium_services/helium_services_helpers.h"

#include <optional>

#include "base/command_line.h"
#include "base/functional/bind.h"
#include "base/strings/stringprintf.h"
#include "components/helium_services/pref_names.h"
#include "components/helium_services/schema.h"
#include "components/prefs/pref_service.h"
#include "net/base/url_util.h"
#include "url/gurl.h"
#include "url/url_constants.h"

namespace helium {
#if defined(ARCH_CPU_ARM64)
#define CPU_ARCH_STR "arm64"
#elif defined(ARCH_CPU_X86_64)
#define CPU_ARCH_STR "x86_64"
#endif

#if defined(CPU_ARCH_STR) && (BUILDFLAG(IS_MAC) || BUILDFLAG(IS_WIN))
#define UPDATES_SUPPORTED
#endif

#if defined(UPDATES_SUPPORTED)
constexpr const char kUpdatePath[] =
#if BUILDFLAG(IS_MAC)
    "mac/appcast-" CPU_ARCH_STR ".xml";
#elif BUILDFLAG(IS_WIN)
    "win/appcast.xml";
#endif

// Duplicated from helium_flag_choices.h to avoid importing it.
constexpr const char kUpdateServerUrlCommandLine[] = "custom-update-server-url";
#endif

std::optional<GURL> GetValidUserOverridenURL(std::string_view user_url_) {
    if (user_url_.empty()) {
        return std::nullopt;
    }

    GURL user_url = GURL(user_url_);
    if (!user_url.is_valid()) {
        return std::nullopt;
    }

    bool isSecure = user_url.SchemeIs(url::kHttpsScheme) || net::IsLocalhost(user_url);
    if (!isSecure) {
        return std::nullopt;
    }

    return user_url;
}

std::optional<GURL> GetValidUserOverridenURL(const PrefService& prefs) {
    return GetValidUserOverridenURL(
        prefs.GetString(prefs::kHeliumServicesOrigin)
    );
}

GURL GetServicesBaseURL(const PrefService& prefs) {
    std::optional<GURL> user_url = GetValidUserOverridenURL(prefs);
    if (user_url) {
        return *user_url;
    }

    return GetDummyURL();
}

GURL GetDummyURL() {
    return GURL(kHeliumDummyOrigin);
}

bool ShouldAccessServices(const PrefService& prefs) {
    return prefs.GetBoolean(prefs::kHeliumServicesEnabled) &&
           prefs.GetBoolean(prefs::kHeliumServicesConsented) &&
           GetValidUserOverridenURL(prefs).has_value();
}

bool ShouldAccessExtensionService(const PrefService& prefs) {
    return (ShouldAccessServices(prefs) &&
            prefs.GetBoolean(prefs::kHeliumExtProxyEnabled)) ||
           prefs.GetBoolean(prefs::kNoventraChromeWebStoreDownloads);
}

bool ShouldAccessDictionaryService(const PrefService& prefs) {
    return (ShouldAccessServices(prefs) &&
            prefs.GetBoolean(prefs::kHeliumSpellcheckEnabled)) ||
           prefs.GetBoolean(prefs::kNoventraDictionaryDownloads);
}

bool ShouldFetchBangs(const PrefService& prefs) {
    return ShouldAccessServices(prefs) &&
            prefs.GetBoolean(prefs::kHeliumBangsEnabled);
}

bool ShouldAccessUpdateService(const PrefService& prefs) {
#if defined(UPDATES_SUPPORTED)
  return prefs.GetBoolean(prefs::kHeliumServicesEnabled) &&
         prefs.GetBoolean(prefs::kHeliumServicesConsented) &&
         prefs.GetBoolean(prefs::kHeliumUpdateFetchingEnabled) &&
         GetValidUserOverridenURL(
             base::CommandLine::ForCurrentProcess()->GetSwitchValueASCII(
                 kUpdateServerUrlCommandLine)).has_value();
#else
  return false;
#endif
}

bool ShouldAccessComponentUpdateService(const PrefService& prefs) {
    return ShouldAccessServices(prefs) &&
            HasAcceptedSchema(prefs, 1) &&
            prefs.GetBoolean(prefs::kHeliumUpdateFetchingEnabled);
}

bool ShouldAccessUBlockAssets(const PrefService& prefs) {
    return ShouldAccessServices(prefs) &&
            prefs.GetBoolean(prefs::kHeliumUBlockAssetsEnabled);
}

GURL GetExtensionUpdateURL(const PrefService& prefs) {
    if (ShouldAccessServices(prefs) &&
        prefs.GetBoolean(prefs::kHeliumExtProxyEnabled)) {
        return GetServicesBaseURL(prefs).Resolve("/ext");
    }
    if (prefs.GetBoolean(prefs::kNoventraChromeWebStoreDownloads)) {
        return GURL("https://clients2.google.com/service/update2/crx");
    }
    return GetDummyURL();
}

GURL GetWebstoreSnippetURL(const PrefService& prefs, std::string_view id) {
    if (ShouldAccessServices(prefs) &&
        prefs.GetBoolean(prefs::kHeliumExtProxyEnabled)) {
        return GetServicesBaseURL(prefs).Resolve(
            base::StringPrintf("/ext/cws_snippet?id=%s", id));
    }
    if (prefs.GetBoolean(prefs::kNoventraChromeWebStoreDownloads)) {
        return GURL("https://chromewebstore.googleapis.com/").Resolve(
            base::StringPrintf("v2/items/%s:fetchItemSnippet", id));
    }
    return GetDummyURL();
}

GURL GetSpellcheckURL(const PrefService& prefs) {
    if (ShouldAccessServices(prefs) &&
        prefs.GetBoolean(prefs::kHeliumSpellcheckEnabled)) {
        return GetServicesBaseURL(prefs).Resolve("/dict/");
    }
    if (prefs.GetBoolean(prefs::kNoventraDictionaryDownloads)) {
        return GURL("https://redirector.gvt1.com/edgedl/chrome/dict/");
    }
    return GetDummyURL();
}

GURL GetBrowserUpdateURL(const PrefService& prefs) {
  if (!ShouldAccessUpdateService(prefs)) {
    return GetDummyURL();
  }

#if defined(UPDATES_SUPPORTED)
  const auto* command_line = base::CommandLine::ForCurrentProcess();
  // Never install an upstream browser over Noventra. A distributor must
  // explicitly configure a compatible update server; user consent still applies.
  const auto base_url = GetValidUserOverridenURL(
      command_line->GetSwitchValueASCII(kUpdateServerUrlCommandLine));
  return base_url ? base_url->Resolve(kUpdatePath) : GetDummyURL();
#else
  return GetDummyURL();
#endif
}

GURL GetComponentUpdateURL(const PrefService* prefs) {
    if (!prefs || !ShouldAccessComponentUpdateService(*prefs)) {
        return GetDummyURL();
    }

    return GetServicesBaseURL(*prefs).Resolve("/com");
}

GURL GetUBlockAssetsURL(const PrefService& prefs) {
    if (!ShouldAccessUBlockAssets(prefs)) {
        return GetDummyURL();
    }

    return GetServicesBaseURL(prefs).Resolve("/ubo/assets.json");
}

void ConfigurePrefChangeRegistrarFor(std::string_view pref_name,
    PrefChangeRegistrar& registrar, const base::RepeatingClosure& observer) {
  registrar.Add(prefs::kHeliumServicesEnabled, observer);
  registrar.Add(prefs::kHeliumServicesConsented, observer);
  registrar.Add(prefs::kHeliumServicesOrigin, observer);
  registrar.Add(prefs::kHeliumSchemaVersion, observer);
  registrar.Add(pref_name, observer);
    if (pref_name == prefs::kHeliumExtProxyEnabled) {
        registrar.Add(prefs::kNoventraChromeWebStoreDownloads, observer);
    }
}

}  // namespace helium
