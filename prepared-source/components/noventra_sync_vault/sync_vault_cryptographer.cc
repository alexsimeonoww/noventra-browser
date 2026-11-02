// Copyright 2026 The Helium Authors
// You can use, redistribute, and/or modify this source code under
// the terms of the GPL-3.0 license that can be found in the LICENSE file.

#include "components/noventra_sync_vault/sync_vault_cryptographer.h"

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "base/containers/span.h"
#include "base/memory/ptr_util.h"
#include "base/notreached.h"
#include "base/strings/string_view_util.h"
#include "base/uuid.h"
#include "crypto/aead.h"
#include "crypto/hash.h"
#include "crypto/kdf.h"
#include "crypto/random.h"
#include "crypto/secure_util.h"

namespace noventra_sync {

namespace {

constexpr auto kAlgorithm = crypto::aead::AES_256_GCM_SIV;
constexpr size_t kNonceBytes = 12;
constexpr size_t kAuthenticationTagBytes = 16;
constexpr std::string_view kKeyInfo =
    "helium-private-sync-v1/object-aead/aes-256-gcm-siv";
constexpr std::string_view kAssociatedDataPrefix =
    "helium-private-sync-v1/object/";

static_assert(kNonceBytes + kAuthenticationTagBytes ==
              kSyncVaultContainerOverheadBytes);

bool IsKnownRole(SyncVaultObjectRole role) {
  return role >= SyncVaultObjectRole::kDescriptor &&
         role <= SyncVaultObjectRole::kEntity;
}

}  // namespace

// static
SyncVaultCryptographer::CreateResult SyncVaultCryptographer::Create(
    base::span<const uint8_t> vault_key,
    std::string vault_uuid) {
  if (vault_key.size() != kVaultKeySize) {
    return base::unexpected(SyncVaultCryptoError::kInvalidKey);
  }
  if (!base::Uuid::ParseLowercase(vault_uuid).is_valid()) {
    return base::unexpected(SyncVaultCryptoError::kInvalidContext);
  }
  std::array<uint8_t, kVaultKeySize> object_key =
      crypto::kdf::Hkdf<kVaultKeySize>(crypto::hash::kSha256, vault_key,
                                       base::as_byte_span(vault_uuid),
                                       base::as_byte_span(kKeyInfo));
  auto cryptographer = base::WrapUnique(
      new SyncVaultCryptographer(object_key, std::move(vault_uuid)));
  crypto::SecureZeroBuffer(object_key);
  return cryptographer;
}

SyncVaultCryptographer::SyncVaultCryptographer(
    base::span<const uint8_t, kVaultKeySize> object_key,
    std::string vault_uuid)
    : aead_(kAlgorithm, object_key), vault_uuid_(std::move(vault_uuid)) {}

SyncVaultCryptographer::~SyncVaultCryptographer() = default;

SyncVaultCryptographer::TransformResult SyncVaultCryptographer::Encrypt(
    SyncVaultObjectRole role,
    base::span<const uint8_t> plaintext) const {
  if (!IsKnownRole(role)) {
    return base::unexpected(SyncVaultCryptoError::kInvalidContext);
  }
  if (plaintext.size() > MaximumPlaintextBytes(role)) {
    return base::unexpected(SyncVaultCryptoError::kObjectTooLarge);
  }

  std::array<uint8_t, kNonceBytes> nonce;
  crypto::RandBytes(nonce);
  std::vector<uint8_t> ciphertext =
      aead_.Seal(plaintext, nonce, BuildAssociatedData(role));
  std::vector<uint8_t> container;
  container.reserve(nonce.size() + ciphertext.size());
  container.insert(container.end(), nonce.begin(), nonce.end());
  container.insert(container.end(), ciphertext.begin(), ciphertext.end());
  return container;
}

SyncVaultCryptographer::TransformResult SyncVaultCryptographer::Decrypt(
    SyncVaultObjectRole role,
    base::span<const uint8_t> container) const {
  if (!IsKnownRole(role)) {
    return base::unexpected(SyncVaultCryptoError::kInvalidContext);
  }
  if (container.size() < kSyncVaultContainerOverheadBytes) {
    return base::unexpected(SyncVaultCryptoError::kMalformedContainer);
  }
  if (container.size() >
      kSyncVaultContainerOverheadBytes + MaximumPlaintextBytes(role)) {
    return base::unexpected(SyncVaultCryptoError::kObjectTooLarge);
  }

  std::optional<std::vector<uint8_t>> plaintext =
      aead_.Open(container.subspan(kNonceBytes), container.first<kNonceBytes>(),
                 BuildAssociatedData(role));
  if (!plaintext) {
    return base::unexpected(SyncVaultCryptoError::kAuthenticationFailed);
  }
  return std::move(*plaintext);
}

std::vector<uint8_t> SyncVaultCryptographer::BuildAssociatedData(
    SyncVaultObjectRole role) const {
  std::vector<uint8_t> associated_data;
  associated_data.reserve(kAssociatedDataPrefix.size() + vault_uuid_.size() +
                          sizeof(uint8_t));
  associated_data.insert(associated_data.end(), kAssociatedDataPrefix.begin(),
                         kAssociatedDataPrefix.end());
  associated_data.insert(associated_data.end(), vault_uuid_.begin(),
                         vault_uuid_.end());
  associated_data.push_back(static_cast<uint8_t>(role));
  return associated_data;
}

// static
size_t SyncVaultCryptographer::MaximumPlaintextBytes(SyncVaultObjectRole role) {
  switch (role) {
    case SyncVaultObjectRole::kDescriptor:
    case SyncVaultObjectRole::kHead:
      return kSyncVaultMaximumMetadataPlaintextBytes;
    case SyncVaultObjectRole::kJournal:
      return kSyncVaultMaximumJournalPlaintextBytes;
    case SyncVaultObjectRole::kEntity:
      return kSyncVaultMaximumEntityPlaintextBytes;
  }
  NOTREACHED();
}

}  // namespace noventra_sync
