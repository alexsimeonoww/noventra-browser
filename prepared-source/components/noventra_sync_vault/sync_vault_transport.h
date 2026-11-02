// Copyright 2026 The Helium Authors
// You can use, redistribute, and/or modify this source code under
// the terms of the GPL-3.0 license that can be found in the LICENSE file.

#ifndef COMPONENTS_NOVENTRA_SYNC_VAULT_SYNC_VAULT_TRANSPORT_H_
#define COMPONENTS_NOVENTRA_SYNC_VAULT_SYNC_VAULT_TRANSPORT_H_

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "base/types/expected.h"

namespace syncer {
class CancelationSignal;
}

namespace noventra_sync {

using syncer::CancelationSignal;

enum class SyncVaultTransportError {
  kAuthenticationRequired,
  kConflict,
  kNetworkError,
  kTemporaryError,
  kQuotaExceeded,
  kAccessDenied,
  kInvalidData,
  kUnsupported,
  kAborted,
};

struct OpaqueSyncVaultObject {
  std::vector<uint8_t> data;
  std::string revision;
};

// Transport-neutral access to one provider vault. Every payload is already an
// opaque authenticated ciphertext. Implementations must provide read-after-
// write consistency, preserve stable exact revisions, atomically enforce
// conditional writes, and report failed conditions as kConflict.
class SyncVaultTransport {
 public:
  using ReadResult = base::expected<std::optional<OpaqueSyncVaultObject>,
                                    SyncVaultTransportError>;
  using WriteResult = base::expected<std::string, SyncVaultTransportError>;

  virtual ~SyncVaultTransport() = default;

  // Connects this transport to the Sync engine's cross-sequence shutdown
  // signal. The signal must outlive the transport. Setup-only transports may
  // operate without one.
  virtual void SetCancelationSignal(CancelationSignal* cancelation_signal) = 0;

  // Returns nullopt when the object does not exist. A returned object must not
  // exceed `maximum_bytes` and must include its exact, non-empty provider
  // revision.
  virtual ReadResult ReadObject(const std::string& object_id,
                                size_t maximum_bytes) = 0;

  // nullopt means IF_ABSENT. A revision means IF_REVISION. A successful write
  // returns the exact, non-empty revision assigned to the object.
  virtual WriteResult WriteObject(const std::string& object_id,
                                  std::optional<std::string> expected_revision,
                                  std::vector<uint8_t> data) = 0;

  // Best-effort storage reclamation after an authenticated head advances its
  // retention floor. Fixed descriptor and head objects are never affected.
  virtual void CollectEpochsBefore(uint32_t minimum_retained_epoch) = 0;
};

}  // namespace noventra_sync

#endif  // COMPONENTS_NOVENTRA_SYNC_VAULT_SYNC_VAULT_TRANSPORT_H_
