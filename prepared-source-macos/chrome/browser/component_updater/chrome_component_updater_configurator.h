// Copyright 2014 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_COMPONENT_UPDATER_CHROME_COMPONENT_UPDATER_CONFIGURATOR_H_
#define CHROME_BROWSER_COMPONENT_UPDATER_CHROME_COMPONENT_UPDATER_CONFIGURATOR_H_

#include "base/memory/scoped_refptr.h"
#include "components/update_client/configurator.h"

class PrefService;

namespace base {
class CommandLine;
}

namespace component_updater {

inline constexpr char kNoventraWidevineDownloadsEnabled[] =
    "noventra.media.widevine_downloads_enabled";
inline constexpr char kNoventraWidevineComponentId[] =
    "oimompecagnajdejgnnjijobebaeigek";

scoped_refptr<update_client::Configurator>
MakeChromeComponentUpdaterConfigurator(const base::CommandLine* cmdline,
                                       PrefService* pref_service);

// This configurator must be used only by the Widevine-only update service.
// It never inherits service URLs or component-updater endpoint overrides.
scoped_refptr<update_client::Configurator>
MakeWidevineComponentUpdaterConfigurator(const base::CommandLine* cmdline,
                                         PrefService* pref_service);

}  // namespace component_updater

#endif  // CHROME_BROWSER_COMPONENT_UPDATER_CHROME_COMPONENT_UPDATER_CONFIGURATOR_H_
