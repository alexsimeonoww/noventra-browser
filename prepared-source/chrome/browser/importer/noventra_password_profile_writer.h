// Copyright 2026 Noventra contributors
// SPDX-License-Identifier: GPL-3.0-only

#ifndef CHROME_BROWSER_IMPORTER_NOVENTRA_PASSWORD_PROFILE_WRITER_H_
#define CHROME_BROWSER_IMPORTER_NOVENTRA_PASSWORD_PROFILE_WRITER_H_

#include <memory>
#include <string>

#include "base/functional/callback.h"
#include "base/memory/scoped_refptr.h"
#include "base/types/expected.h"
#include "chrome/browser/importer/profile_writer.h"

class Profile;

namespace noventra {

using PreparedPasswordProfileWriter =
    base::expected<scoped_refptr<ProfileWriter>, std::string>;
using PasswordProfileWriterCallback =
    base::OnceCallback<void(PreparedPasswordProfileWriter)>;

// Own this while destination inspection is pending. Destruction cancels the
// request and its callback. The profile and password-store callbacks are weak;
// no source profile is opened and no destination write happens in preparation.
class PasswordProfileWriterPreparation {
 public:
  ~PasswordProfileWriterPreparation();
  PasswordProfileWriterPreparation(const PasswordProfileWriterPreparation&) =
      delete;
  PasswordProfileWriterPreparation& operator=(
      const PasswordProfileWriterPreparation&) = delete;

 private:
  friend std::unique_ptr<PasswordProfileWriterPreparation>
  PreparePasswordProfileWriter(Profile*, PasswordProfileWriterCallback);
  PasswordProfileWriterPreparation(Profile* profile,
                                   PasswordProfileWriterCallback callback);
  class Impl;
  std::unique_ptr<Impl> impl_;
};

// UI-thread only. The callback always runs asynchronously, after this function
// returns, so the caller can store the request before handling success/failure.
// It may destroy the request. The prepared writer preserves destination
// realm+username identities and all blocked realms, and deduplicates source
// identities regardless of changing form-field names. This is a snapshot plus
// live-observer guard, not an atomic transaction against concurrent UI edits.
std::unique_ptr<PasswordProfileWriterPreparation> PreparePasswordProfileWriter(
    Profile* profile,
    PasswordProfileWriterCallback callback);

}  // namespace noventra

#endif  // CHROME_BROWSER_IMPORTER_NOVENTRA_PASSWORD_PROFILE_WRITER_H_
