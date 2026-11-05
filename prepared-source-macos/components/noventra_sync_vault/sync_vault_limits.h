// Copyright 2026 The Helium Authors
// You can use, redistribute, and/or modify this source code under
// the terms of the GPL-3.0 license that can be found in the LICENSE file.

#ifndef COMPONENTS_NOVENTRA_SYNC_VAULT_SYNC_VAULT_LIMITS_H_
#define COMPONENTS_NOVENTRA_SYNC_VAULT_SYNC_VAULT_LIMITS_H_

#include <cstddef>
#include <string_view>

namespace noventra_sync {

// Limits shared by the encrypted vault format and its provider API.
inline constexpr size_t kSyncVaultContainerOverheadBytes = 28;
inline constexpr size_t kSyncVaultMaximumEncryptedObjectBytes =
    16 * 1024 * 1024;
inline constexpr size_t kSyncVaultMaximumIdentifierBytes = 4096;
inline constexpr size_t kSyncVaultMaximumMetadataPlaintextBytes =
    64 * 1024 - kSyncVaultContainerOverheadBytes;
inline constexpr size_t kSyncVaultMaximumJournalPlaintextBytes =
    4 * 1024 * 1024 - kSyncVaultContainerOverheadBytes;
inline constexpr size_t kSyncVaultMaximumEntityPlaintextBytes =
    kSyncVaultMaximumEncryptedObjectBytes - kSyncVaultContainerOverheadBytes;
inline constexpr size_t kSyncVaultMaximumReconstructedBytes = 256 * 1024 * 1024;

inline constexpr bool IsValidSyncVaultObjectId(std::string_view value) {
  return !value.empty() && value.size() <= kSyncVaultMaximumIdentifierBytes;
}

inline constexpr bool IsValidSyncVaultRevision(std::string_view value) {
  return !value.empty() && value.size() <= kSyncVaultMaximumIdentifierBytes;
}

}  // namespace noventra_sync

#endif  // COMPONENTS_NOVENTRA_SYNC_VAULT_SYNC_VAULT_LIMITS_H_
