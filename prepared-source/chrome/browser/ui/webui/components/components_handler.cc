// Copyright 2019 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/ui/webui/components/components_handler.h"

#include <utility>
#include <vector>

#include "base/check.h"
#include "base/command_line.h"
#include "base/functional/bind.h"
#include "base/values.h"
#include "build/build_config.h"
#include "chrome/browser/browser_process.h"
#include "chrome/browser/component_updater/chrome_component_updater_configurator.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/grit/generated_resources.h"
#include "components/component_updater/component_updater_switches.h"
#include "components/helium_services/helium_services_helpers.h"
#include "components/prefs/pref_service.h"
#include "components/update_client/crx_update_item.h"
#include "content/public/browser/web_contents.h"
#include "third_party/widevine/cdm/buildflags.h"
#include "ui/base/l10n/l10n_util.h"

#if BUILDFLAG(ENABLE_WIDEVINE_CDM_COMPONENT)
#include "chrome/browser/component_updater/widevine_cdm_component_installer.h"
#endif

namespace {

Profile* CurrentProfile(content::WebUI* web_ui) {
  auto* contents = web_ui->GetWebContents();
  return contents ? Profile::FromBrowserContext(contents->GetBrowserContext())
                  : nullptr;
}

PrefService* WidevinePrefs() {
  auto* prefs = g_browser_process ? g_browser_process->local_state() : nullptr;
  return prefs && prefs->FindPreference(
                      component_updater::kNoventraWidevineDownloadsEnabled)
             ? prefs
             : nullptr;
}

}  // namespace

ComponentsHandler::ComponentsHandler(
    component_updater::ComponentUpdateService* component_updater,
    component_updater::ComponentUpdateService* widevine_component_updater)
    : component_updater_(component_updater),
      widevine_component_updater_(widevine_component_updater) {
  DCHECK(component_updater_ || widevine_component_updater_);
}
ComponentsHandler::~ComponentsHandler() = default;

void ComponentsHandler::RegisterMessages() {
  web_ui()->RegisterMessageCallback(
      "setWidevineDownloadsEnabled",
      base::BindRepeating(&ComponentsHandler::HandleSetWidevineDownloadsEnabled,
                          base::Unretained(this)));
  web_ui()->RegisterMessageCallback(
      "requestComponentsData",
      base::BindRepeating(&ComponentsHandler::HandleRequestComponentsData,
                          base::Unretained(this)));

  web_ui()->RegisterMessageCallback(
      "checkUpdate", base::BindRepeating(&ComponentsHandler::HandleCheckUpdate,
                                         base::Unretained(this)));

  web_ui()->RegisterMessageCallback(
      "uninstallComponent",
      base::BindRepeating(&ComponentsHandler::HandleUninstallComponent,
                          base::Unretained(this)));
}

void ComponentsHandler::OnJavascriptAllowed() {
  if (component_updater_) {
    observation_.AddObservation(component_updater_.get());
  }
  if (widevine_component_updater_ &&
      widevine_component_updater_ != component_updater_) {
    observation_.AddObservation(widevine_component_updater_.get());
  }
}

void ComponentsHandler::OnJavascriptDisallowed() {
  observation_.RemoveAllObservations();
}

void ComponentsHandler::HandleRequestComponentsData(
    const base::ListValue& args) {
  if (args.empty() || !args[0].is_string()) {
    return;
  }
  AllowJavascript();
  const base::Value& callback_id = args[0];
  if (args.size() != 1) {
    RejectJavascriptCallback(callback_id, base::Value("Invalid request"));
    return;
  }

  base::DictValue result;
  result.Set("components", LoadComponents());
  result.Set("widevineAvailable", !!widevine_component_updater_);
  auto* prefs = WidevinePrefs();
  result.Set(
      "widevineDownloadsEnabled",
      prefs && prefs->GetBoolean(
                   component_updater::kNoventraWidevineDownloadsEnabled));
  bool restart_required = false;
#if BUILDFLAG(ENABLE_WIDEVINE_CDM_COMPONENT)
  restart_required = component_updater::IsWidevineCdmRestartRequired();
#endif
  result.Set("widevineRestartRequired", restart_required);
  result.Set("widevineRestartSupported", BUILDFLAG(IS_LINUX));
  Profile* profile = CurrentProfile(web_ui());
  result.Set("widevineCanChange", profile && !profile->IsOffTheRecord() &&
                                      !profile->IsGuestSession());

  ResolveJavascriptCallback(callback_id, result);
}

void ComponentsHandler::HandleSetWidevineDownloadsEnabled(
    const base::ListValue& args) {
  if (args.empty() || !args[0].is_string()) {
    return;
  }
  AllowJavascript();
  auto* prefs = WidevinePrefs();
  if (args.size() != 2 || !args[1].is_bool() || !widevine_component_updater_ ||
      !prefs) {
    RejectJavascriptCallback(args[0], base::Value("Invalid request"));
    return;
  }
  Profile* profile = CurrentProfile(web_ui());
  if (!profile || profile->IsOffTheRecord() || profile->IsGuestSession()) {
    RejectJavascriptCallback(args[0],
                             base::Value("Unavailable in private mode"));
    return;
  }
  const bool enabled = args[1].GetBool();
  prefs->SetBoolean(component_updater::kNoventraWidevineDownloadsEnabled,
                    enabled);
  if (enabled) {
    OnDemandUpdate(component_updater::kNoventraWidevineComponentId);
  }
  ResolveJavascriptCallback(args[0], base::Value(enabled));
}

// This function is called when user presses button from html UI.
// TODO(shrikant): We need to make this button available based on current
// state e.g. If component state is currently updating then we need to disable
// button. (https://code.9oo91e.qjz9zk/p/chromium/issues/detail?id=272540)
void ComponentsHandler::HandleCheckUpdate(const base::ListValue& args) {
  if (args.size() != 1 || !args[0].is_string()) {
    return;
  }
  const std::string& component_id = args[0].GetString();

  OnDemandUpdate(component_id);
}

void ComponentsHandler::HandleUninstallComponent(const base::ListValue& args) {
  if (!base::CommandLine::ForCurrentProcess()->HasSwitch(
          switches::kEnableComponentUninstall)) {
    return;
  }
  if (args.empty() || !args[0].is_string()) {
    return;
  }
  const auto& id = args[0].GetString();
  auto* service = id == component_updater::kNoventraWidevineComponentId
                      ? widevine_component_updater_.get()
                      : component_updater_.get();
  if (service) {
    service->UnregisterComponent(id);
  }
}

void ComponentsHandler::OnEvent(const update_client::CrxUpdateItem& item) {
  base::DictValue parameters;
  parameters.Set(
      "event", ServiceStatusToString(
                   item.state,
                   item.id == component_updater::kNoventraWidevineComponentId));
  parameters.Set("id", item.id);
  if (item.component) {
    parameters.Set("version", item.component->version.GetString());
  }
  FireWebUIListener("component-event", parameters);
}

std::u16string ComponentsHandler::ServiceStatusToString(
    update_client::ComponentState state,
    bool widevine) {
  if (!widevine && state == update_client::ComponentState::kUpdateError) {
    Profile* profile = CurrentProfile(web_ui());

    if (profile) {
      PrefService* prefs = profile->GetPrefs();
      if (prefs && !helium::ShouldAccessComponentUpdateService(*prefs)) {
        return l10n_util::GetStringUTF16(
            IDS_COMPONENTS_SVC_STATUS_ERROR_HELIUM_SERVICES);
      }
    }
  }

  switch (state) {
    case update_client::ComponentState::kNew:
      return l10n_util::GetStringUTF16(IDS_COMPONENTS_SVC_STATUS_NEW);
    case update_client::ComponentState::kChecking:
      return l10n_util::GetStringUTF16(IDS_COMPONENTS_SVC_STATUS_CHECKING);
    case update_client::ComponentState::kCanUpdate:
      return l10n_util::GetStringUTF16(IDS_COMPONENTS_SVC_STATUS_UPDATE);
    case update_client::ComponentState::kDownloading:
      return l10n_util::GetStringUTF16(IDS_COMPONENTS_SVC_STATUS_DNL);
    case update_client::ComponentState::kDecompressing:
      return l10n_util::GetStringUTF16(IDS_COMPONENTS_SVC_STATUS_DECOMPRESSING);
    case update_client::ComponentState::kPatching:
      return l10n_util::GetStringUTF16(IDS_COMPONENTS_SVC_STATUS_PATCHING);
    case update_client::ComponentState::kUpdating:
      return l10n_util::GetStringUTF16(IDS_COMPONENTS_SVC_STATUS_UPDATING);
    case update_client::ComponentState::kUpdated:
      return l10n_util::GetStringUTF16(IDS_COMPONENTS_SVC_STATUS_UPDATED);
    case update_client::ComponentState::kUpToDate:
      return l10n_util::GetStringUTF16(IDS_COMPONENTS_SVC_STATUS_UPTODATE);
    case update_client::ComponentState::kUpdateError:
      return l10n_util::GetStringUTF16(IDS_COMPONENTS_SVC_STATUS_UPDATE_ERROR);
    case update_client::ComponentState::kRun:
      return l10n_util::GetStringUTF16(IDS_COMPONENTS_UNKNOWN);
  }
  return l10n_util::GetStringUTF16(IDS_COMPONENTS_UNKNOWN);
}

void ComponentsHandler::OnDemandUpdate(const std::string& component_id) {
  auto* service = component_updater_.get();
  if (component_id == component_updater::kNoventraWidevineComponentId) {
    auto* prefs = WidevinePrefs();
    if (!widevine_component_updater_ || !prefs ||
        !prefs->GetBoolean(
            component_updater::kNoventraWidevineDownloadsEnabled)) {
      return;
    }
    Profile* profile = CurrentProfile(web_ui());
    if (!profile || profile->IsOffTheRecord() || profile->IsGuestSession()) {
      return;
    }
    service = widevine_component_updater_.get();
  }
  if (!service) {
    return;
  }
  service->GetOnDemandUpdater().OnDemandUpdate(
      component_id, component_updater::OnDemandUpdater::Priority::FOREGROUND,
      component_updater::Callback());
}

base::ListValue ComponentsHandler::LoadComponents() {
  base::ListValue component_list;
  std::vector<component_updater::ComponentUpdateService*> services;
  if (component_updater_) {
    services.push_back(component_updater_.get());
  }
  if (widevine_component_updater_ &&
      widevine_component_updater_ != component_updater_) {
    services.push_back(widevine_component_updater_.get());
  }
  for (auto* service : services) {
    for (const auto& component_id : service->GetComponentIDs()) {
      update_client::CrxUpdateItem item;
      if (!service->GetComponentDetails(component_id, &item)) {
        continue;
      }
      const bool widevine = service == widevine_component_updater_;
      base::DictValue component_entry;
      component_entry.Set("id", component_id);
      component_entry.Set("widevine", widevine);
      component_entry.Set("status",
                          ServiceStatusToString(item.state, widevine));
      if (item.component) {
        component_entry.Set("name", item.component->name);
        component_entry.Set("version", item.component->version.GetString());
      }
      component_list.Append(std::move(component_entry));
    }
  }

  return component_list;
}
