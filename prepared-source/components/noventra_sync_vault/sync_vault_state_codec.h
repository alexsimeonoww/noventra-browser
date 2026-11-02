// Copyright 2026 The Helium Authors
// You can use, redistribute, and/or modify this source code under
// the terms of the GPL-3.0 license that can be found in the LICENSE file.

#ifndef COMPONENTS_NOVENTRA_SYNC_VAULT_SYNC_VAULT_STATE_CODEC_H_
#define COMPONENTS_NOVENTRA_SYNC_VAULT_SYNC_VAULT_STATE_CODEC_H_

#include <cstdint>
#include <memory>
#include <vector>

#include "base/containers/span.h"
#include "base/types/expected.h"

namespace noventra_sync {

enum class SyncVaultStateCodecError {
  kMalformed,
  kUnsupportedVersion,
  kLimitExceeded,
  kInvalidValue,
};

using SyncVaultStateCodecResult =
    base::expected<std::vector<uint8_t>, SyncVaultStateCodecError>;
using SyncVaultStateValidationResult =
    base::expected<void, SyncVaultStateCodecError>;
// Validates and reconstructs backend state from authenticated payloads. The
// vault format treats snapshots and deltas as opaque. Base snapshots passed
// to StartReconstruction() have already passed validation.
class SyncVaultStateCodec {
 public:
  virtual ~SyncVaultStateCodec() = default;

  virtual SyncVaultStateValidationResult ValidateSnapshot(
      base::span<const uint8_t> state) const = 0;

  class Reconstruction {
   public:
    virtual ~Reconstruction() = default;
    virtual SyncVaultStateValidationResult ApplyDelta(
        base::span<const uint8_t> delta) = 0;
    virtual SyncVaultStateCodecResult Finish() const = 0;
  };
  using ReconstructionResult =
      base::expected<std::unique_ptr<Reconstruction>, SyncVaultStateCodecError>;
  // Keeps parsed state between deltas. Callers can release each payload before
  // reading the next one, without reparsing the full state for every delta.
  virtual ReconstructionResult StartReconstruction(
      base::span<const uint8_t> base_state) const = 0;
};

}  // namespace noventra_sync

#endif  // COMPONENTS_NOVENTRA_SYNC_VAULT_SYNC_VAULT_STATE_CODEC_H_
