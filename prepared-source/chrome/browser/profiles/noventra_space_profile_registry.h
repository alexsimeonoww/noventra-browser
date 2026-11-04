// Copyright 2026 The Noventra Authors. All rights reserved.
#ifndef CHROME_BROWSER_PROFILES_NOVENTRA_SPACE_PROFILE_REGISTRY_H_
#define CHROME_BROWSER_PROFILES_NOVENTRA_SPACE_PROFILE_REGISTRY_H_

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "base/files/file_path.h"
#include "base/functional/callback_forward.h"
#include "base/memory/weak_ptr.h"

class PrefRegistrySimple;
class Profile;
class ProfileManager;
class ScopedProfileKeepAlive;

namespace noventra {

inline constexpr char kSpaceProfilesLocalState[] = "noventra.profile_spaces";

enum class SpaceProfileStorage {
  kDefault,
  kLegacyPartition,
};

bool ProfileSpacesEnabled();
void RegisterSpaceProfileLocalState(PrefRegistrySimple* registry);

// These resolve catalog ownership only. Browsing services must continue using
// the selected Browser's actual Profile, including its own OTR parent.
Profile* SpaceCatalogProfile(Profile* profile);
std::string ProfileSpaceId(Profile* profile);
bool IsProfileSpace(Profile* profile);
std::vector<std::string> RegisteredSpaceIds(Profile* profile);
std::optional<SpaceProfileStorage> ProfileSpaceStorage(Profile* profile);

// A valid Profile is kept alive until the callback returns. The caller must
// construct its Browser synchronously inside that callback to retain it.
void AcquireSpaceProfile(Profile* source,
                         const std::string& id,
                         base::OnceCallback<void(Profile*)> callback);

struct SpaceProfileBindingForStartup {
  base::FilePath root_basename;
  std::string id;
  base::FilePath profile_basename;
  SpaceProfileStorage storage;
  // Distinguishes a crash before native creation from a missing used Profile.
  bool ui_seeded;
  bool initialization_started = false;
};

// Validated cold-start inventory. nullopt rejects a malformed catalog.
std::optional<std::vector<SpaceProfileBindingForStartup>>
SpaceProfileBindingsForStartup();

struct MigratedSpaceProfileBinding {
  std::string id;
  base::FilePath profile_basename;
};

// Native ProfileManager gates all registered child creation through these
// operations, including background-only profiles loaded outside the Space UI.
bool PrepareSpaceProfileLoad(
    base::WeakPtr<ProfileManager> manager,
    const base::FilePath& profile_path,
    std::unique_ptr<ScopedProfileKeepAlive>* catalog_keep_alive);
void PrepareSpaceProfileLoadAsync(
    base::WeakPtr<ProfileManager> manager,
    const base::FilePath& profile_path,
    base::OnceCallback<void(bool, std::unique_ptr<ScopedProfileKeepAlive>)>
        callback);
bool AttachSpaceCatalogProfileLease(Profile* profile);
// Cold startup uses the same ownership marker check before session parsing.
bool IsSpaceProfileDirectoryOwned(const base::FilePath& user_data_directory,
                                  const SpaceProfileBindingForStartup& binding);

// Called before Profile initialization, after all destinations are prepared.
// Exact replay is idempotent; conflicting ownership fails without writing.
// The caller must durably commit Local State before its journal is committed.
std::optional<int> RegisterMigratedSpaceProfiles(
    const base::FilePath& root_basename,
    const std::vector<MigratedSpaceProfileBinding>& bindings);

}  // namespace noventra

#endif  // CHROME_BROWSER_PROFILES_NOVENTRA_SPACE_PROFILE_REGISTRY_H_
