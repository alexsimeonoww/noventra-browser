// Copyright 2025 The Helium Authors
// You can use, redistribute, and/or modify this source code under
// the terms of the GPL-3.0 license that can be found in the LICENSE file.

#include "components/helium_services/schema.h"

#include "components/helium_services/helium_services_helpers.h"
#include "components/helium_services/pref_names.h"
#include "components/prefs/pref_service.h"

namespace helium {

bool HasAcceptedSchema(const PrefService& prefs, int version) {
  return prefs.GetBoolean(prefs::kHeliumDisableSchemaAlerts) ||
         prefs.GetInteger(prefs::kHeliumSchemaVersion) >= version;
}

void AcceptCurrentSchema(PrefService& prefs) {
  prefs.SetInteger(prefs::kHeliumSchemaVersion, kHeliumCurrentSchemaVersion);
}

bool ShouldShowSchemaNotification(const PrefService& prefs) {
  return ShouldAccessServices(prefs) &&
         !HasAcceptedSchema(prefs, kHeliumCurrentSchemaVersion);
}

ServicesChangelog& GetChangelog() {
  static constexpr auto kHeliumSchemaChangelog =
      base::MakeFixedFlatMap<int, std::string_view>({
          {
            1,
            "Automatic component updates are now available. They're managed by the same toggle that enables automatic browser updates.\n"
            "From now on, you'll be notified about major changes to Noventra service controls. Even though these notifications are extremely rare, you can choose to ignore them and accept all future changes automatically."
          }
      });

  return kHeliumSchemaChangelog;
}

}  // namespace helium
