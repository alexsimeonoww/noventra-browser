// Copyright 2026 The Noventra Authors. All rights reserved.
#ifndef CHROME_BROWSER_PROFILES_NOVENTRA_SPACE_STARTUP_H_
#define CHROME_BROWSER_PROFILES_NOVENTRA_SPACE_STARTUP_H_

#include "base/types/expected.h"
#include "chrome/browser/profiles/noventra_space_migration.h"

namespace base {
class CommandLine;
class FilePath;
}  // namespace base

namespace noventra {

// The caller owns the user-data singleton and has not initialized any Profile.
// This UI-thread boundary waits for cold worker IO, durable catalog commit and
// session caching. Failure leaves source data and recovery material in place.
base::expected<void, SpaceMigrationError> PrepareProfileSpacesForStartup(
    const base::FilePath& user_data_directory,
    base::CommandLine& command_line);

}  // namespace noventra

#endif  // CHROME_BROWSER_PROFILES_NOVENTRA_SPACE_STARTUP_H_
