// Copyright 2025 The Helium Authors
// You can use, redistribute, and/or modify this source code under
// the terms of the GPL-3.0 license that can be found in the LICENSE file.

#include "chrome/browser/ui/webui/onboarding/onboarding_page_ui.h"

#include "base/strings/strcat.h"
#include "build/build_config.h"
#include "build/config/linux/dbus/buildflags.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/ui/webui/onboarding/onboarding_handler.h"
#include "chrome/browser/ui/webui/settings/brave_import_data_handler.h"
#include "chrome/browser/ui/webui/settings/search_engines_handler.h"
#include "chrome/browser/ui/webui/settings/settings_default_browser_handler.h"
#include "chrome/common/webui_url_constants.h"
#include "components/helium_onboarding/helium_onboarding_localized_strings.h"
#include "components/helium_onboarding/resources/grit/helium_onboarding_generated.h"
#include "components/helium_onboarding/resources/grit/helium_onboarding_generated_map.h"
#include "content/public/browser/web_contents.h"
#include "content/public/browser/web_ui_controller.h"
#include "content/public/browser/web_ui_data_source.h"
#include "content/public/common/bindings_policy.h"
#include "services/network/public/mojom/content_security_policy.mojom.h"
#include "ui/webui/webui_util.h"

#if BUILDFLAG(IS_MAC)
#include "base/mac/mac_util.h"
#endif

HeliumOnboardingPage::HeliumOnboardingPage(content::WebUI* web_ui)
    : WebUIController(web_ui) {
  web_ui->SetBindings({content::BindingsPolicyValue::kWebUi,
                       content::BindingsPolicyValue::kMojoWebUi});
  content::BrowserContext* browser_context =
      web_ui->GetWebContents()->GetBrowserContext();
  Profile* profile = Profile::FromBrowserContext(browser_context);
  content::WebUIDataSource* source =
      content::WebUIDataSource::CreateAndAdd(profile, chrome::kHeliumSetupHost);

  webui::SetupWebUIDataSource(source, kHeliumOnboardingGenerated,
                              IDR_HELIUM_ONBOARDING_INDEX_HTML);
  source->AddLocalizedStrings(helium_onboarding::kLocalizedStrings);
  source->UseStringsJs();
  source->AddBoolean("canImportBrowserPasswords",
                     BUILDFLAG(IS_LINUX) && BUILDFLAG(USE_DBUS));

#if BUILDFLAG(IS_MAC)
  source->AddBoolean("showMacImportPermissionNotice",
                     base::mac::MacOSMajorVersion() >= 27);
#endif

  source->OverrideContentSecurityPolicy(
      network::mojom::CSPDirectiveName::TrustedTypes,
      base::StrCat({webui::kDefaultTrustedTypesPolicies,
                    " svelte-trusted-html;"}));

  web_ui->AddMessageHandler(
      std::make_unique<OnboardingMessageHandler>(browser_context));
  web_ui->AddMessageHandler(
      std::make_unique<settings::SearchEnginesHandler>(profile));
  web_ui->AddMessageHandler(
      std::make_unique<settings::DefaultBrowserHandler>());
  web_ui->AddMessageHandler(
      std::make_unique<settings::BraveImportDataHandler>());
}

HeliumOnboardingPage::~HeliumOnboardingPage() = default;
