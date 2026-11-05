// Copyright 2026 Noventra contributors
// SPDX-License-Identifier: GPL-3.0-only

#ifndef CHROME_UTILITY_IMPORTER_CHROME_PASSWORD_IMPORTER_H_
#define CHROME_UTILITY_IMPORTER_CHROME_PASSWORD_IMPORTER_H_

#include <optional>
#include <string>
#include <vector>

#include "base/files/file_path.h"
#include "base/functional/callback.h"
#include "base/types/expected.h"
#include "components/os_crypt/async/common/encryptor.h"
#include "components/user_data_importer/common/importer_data_types.h"

namespace noventra {

enum class PasswordImportError {
  kReadFailed,
  kNoPasswords,
  kUnlockFailed,
  kUnsupportedEncryption,
  kDecryptFailed,
  kCancelled,
};

using PasswordImportResult =
    base::expected<std::vector<user_data_importer::ImportedPasswordForm>,
                   PasswordImportError>;
using SourcePasswordKeyLookup =
    base::OnceCallback<std::optional<os_crypt_async::Encryptor::Key>()>;

// Read and validate every source row before returning any destination writes.
// The source files are only read; SQLite recovery runs on a private, verified
// copy, including WAL/journal files. The callback is invoked only for v11.
PasswordImportResult ReadChromePasswords(
    const base::FilePath& profile,
    SourcePasswordKeyLookup source_key,
    base::RepeatingCallback<bool()> cancelled);

bool SnapshotBrowserDatabase(const base::FilePath& source,
                             const base::FilePath& destination);

// Known branding selects one existing key identity, never trial keys. Missing
// credentials stay missing. This may display the normal desktop unlock prompt.
std::optional<os_crypt_async::Encryptor::Key> ReadChromePasswordKey(
    const std::u16string& importer_name,
    base::RepeatingCallback<bool()> cancelled);

// Static text only; no paths, usernames, record values or key material.
const char* PasswordImportErrorMessage(PasswordImportError error);

}  // namespace noventra

#endif  // CHROME_UTILITY_IMPORTER_CHROME_PASSWORD_IMPORTER_H_
