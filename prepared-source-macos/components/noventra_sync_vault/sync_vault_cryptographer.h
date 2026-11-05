// Copyright 2026 The Helium Authors
// You can use, redistribute, and/or modify this source code under
// the terms of the GPL-3.0 license that can be found in the LICENSE file.

#ifndef COMPONENTS_NOVENTRA_SYNC_VAULT_SYNC_VAULT_CRYPTOGRAPHER_H_
#define COMPONENTS_NOVENTRA_SYNC_VAULT_SYNC_VAULT_CRYPTOGRAPHER_H_

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "base/containers/span.h"
#include "base/types/expected.h"
#include "components/noventra_sync_vault/sync_vault_limits.h"
#include "crypto/aead.h"

namespace noventra_sync {

enum class SyncVaultObjectRole : uint8_t {
  kDescriptor = 1,
  kHead = 2,
  kJournal = 3,
  kEntity = 4,
};

enum class SyncVaultCryptoError {
  kInvalidKey,
  kInvalidContext,
  kMalformedContainer,
  kObjectTooLarge,
  kAuthenticationFailed,
};

// Thin object-encryption adapter over Chromium's AEAD implementation. One
// purpose-specific key is derived for the vault; role and vault identity are
// authenticated as associated data.
class SyncVaultCryptographer {
 public:
  static constexpr size_t kVaultKeySize = 32;

  using CreateResult = base::expected<std::unique_ptr<SyncVaultCryptographer>,
                                      SyncVaultCryptoError>;
  using TransformResult =
      base::expected<std::vector<uint8_t>, SyncVaultCryptoError>;

  static CreateResult Create(base::span<const uint8_t> vault_key,
                             std::string vault_uuid);

  SyncVaultCryptographer(const SyncVaultCryptographer&) = delete;
  SyncVaultCryptographer& operator=(const SyncVaultCryptographer&) = delete;
  ~SyncVaultCryptographer();

  TransformResult Encrypt(SyncVaultObjectRole role,
                          base::span<const uint8_t> plaintext) const;
  TransformResult Decrypt(SyncVaultObjectRole role,
                          base::span<const uint8_t> container) const;

  const std::string& vault_uuid() const { return vault_uuid_; }

 private:
  SyncVaultCryptographer(base::span<const uint8_t, kVaultKeySize> object_key,
                         std::string vault_uuid);

  std::vector<uint8_t> BuildAssociatedData(SyncVaultObjectRole role) const;
  static size_t MaximumPlaintextBytes(SyncVaultObjectRole role);

  crypto::Aead aead_;
  const std::string vault_uuid_;
};

}  // namespace noventra_sync

#endif  // COMPONENTS_NOVENTRA_SYNC_VAULT_SYNC_VAULT_CRYPTOGRAPHER_H_
