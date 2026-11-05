// Copyright 2026 The Helium Authors
// You can use, redistribute, and/or modify this source code under
// the terms of the GPL-3.0 license that can be found in the LICENSE file.

#ifndef COMPONENTS_NOVENTRA_SYNC_VAULT_SYNC_VAULT_STORE_H_
#define COMPONENTS_NOVENTRA_SYNC_VAULT_SYNC_VAULT_STORE_H_

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "base/containers/span.h"
#include "base/functional/callback.h"
#include "base/sequence_checker.h"
#include "base/time/time.h"
#include "base/types/expected.h"
#include "components/noventra_sync_vault/sync_vault_format.h"
#include "components/noventra_sync_vault/sync_vault_transport.h"

namespace syncer {
class CancelationSignal;
}

namespace noventra_sync {

using syncer::CancelationSignal;
class SyncVaultCryptographer;

enum class SyncVaultStoreError {
  kAuthenticationRequired,
  kConflict,
  kNetworkError,
  kTemporaryError,
  kQuotaExceeded,
  kAccessDenied,
  kInvalidData,
  kUnsupported,
  kAborted,
  kInvalidVaultKey,
  kAuthenticationFailed,
  kMalformedObject,
  kUnsupportedVersion,
  kObjectTooLarge,
  kRollback,
  kFork,
  kIncompleteHistory,
  kCheckpointPersistenceFailed,
};

struct SyncVaultState {
  std::vector<uint8_t> data;
  SyncVaultHead head;
  std::optional<std::string> head_revision;
  bool initialized = false;
  std::vector<SyncVaultObjectReference> state_chunks;
};

// Implements the browser-owned encrypted journal over an opaque object
// provider. Immutable state chunks and journal records are installed first;
// only the fixed head is conditionally replaced.
class SyncVaultStore {
 public:
  using PersistCheckpointCallback =
      base::RepeatingCallback<bool(const SyncVaultCheckpoint&)>;
  using CreateResult =
      base::expected<std::unique_ptr<SyncVaultStore>, SyncVaultStoreError>;
  using ReadResult = base::expected<SyncVaultState, SyncVaultStoreError>;
  using RefreshResult =
      base::expected<std::optional<SyncVaultState>, SyncVaultStoreError>;
  using CommitResult = base::expected<SyncVaultState, SyncVaultStoreError>;

  struct ReadBudget {
    size_t maximum_objects = 512;
    size_t maximum_bytes = 32 * 1024 * 1024;
    base::TimeTicks deadline;
    base::RepeatingCallback<bool()> is_cancelled;
  };

  static CreateResult Create(
      std::unique_ptr<SyncVaultTransport> transport,
      base::span<const uint8_t> vault_key,
      std::string vault_uuid,
      std::optional<SyncVaultCheckpoint> checkpoint,
      PersistCheckpointCallback persist_checkpoint_callback);

  SyncVaultStore(const SyncVaultStore&) = delete;
  SyncVaultStore& operator=(const SyncVaultStore&) = delete;
  ~SyncVaultStore();

  void SetCancelationSignal(CancelationSignal* cancelation_signal);
  ReadResult ReadState();
  // A null optional means bounded progress, never a successfully read state.
  // Call again on the same sequence with a fresh budget to resume. A slice
  // admits a read only when its full maximum response fits the byte budget;
  // maximum_bytes must therefore admit one maximum-size encrypted object.
  // The deadline and cancellation callback are required. The transport must
  // independently bound/cancel each blocking request.
  //
  // The in-memory cursor retains authenticated metadata and at most one
  // reconstructed state, within the existing format/ancestry/state limits.
  // It is anchored to an unchanged durable checkpoint. No checkpoint advances
  // until all ancestry and chunks authenticate and a fresh head agrees.
  // Changed heads are reconciled before returning; commits still require CAS.
  // Terminal errors, AbortRead(), changing the cancellation signal, or another
  // public read/write operation discard progress. Transient network failures
  // keep verified progress; expiry of the slice deadline yields. Restart
  // safely begins again.
  RefreshResult ReadStateIncrementally(const ReadBudget& budget);
  void AbortRead();
  // Authenticates the descriptor, current head, and the journal ancestry
  // needed to anchor that head without fetching state chunks.
  base::expected<void, SyncVaultStoreError> VerifyInitializedVault();
  // Authenticates the current head and returns a complete replacement only
  // when it differs from the already loaded state.
  RefreshResult ReadStateIfChanged(const SyncVaultState& loaded_state);
  // Creates an authenticated empty descriptor/head when needed, then reads
  // both objects back through the normal verification path. This is intended
  // for new-vault setup; joining an existing vault must use ReadState() so a
  // missing remote history cannot be silently initialized.
  ReadResult InitializeEmptyVaultAndReadState();
  CommitResult CommitState(const SyncVaultState& base_state,
                           base::span<const uint8_t> state,
                           uint32_t mutation_count);

  const SyncVaultCheckpoint& checkpoint() const { return checkpoint_; }

 private:
  SyncVaultStore(std::unique_ptr<SyncVaultTransport> transport,
                 std::unique_ptr<SyncVaultCryptographer> cryptographer,
                 SyncVaultCheckpoint checkpoint,
                 PersistCheckpointCallback persist_checkpoint_callback);

  using ObjectResult =
      base::expected<std::optional<OpaqueSyncVaultObject>, SyncVaultStoreError>;
  using HeadResult = base::expected<std::pair<SyncVaultHead, std::string>,
                                    SyncVaultStoreError>;
  using JournalResult =
      base::expected<SyncVaultJournalRecord, SyncVaultStoreError>;
  using WriteResult = base::expected<std::string, SyncVaultStoreError>;

  ObjectResult ReadObject(const std::string& object_id,
                          size_t maximum_bytes) const;
  WriteResult WriteObject(const std::string& object_id,
                          std::optional<std::string> expected_revision,
                          std::vector<uint8_t> data);
  base::expected<void, SyncVaultStoreError> ValidateDescriptor(
      const OpaqueSyncVaultObject& object) const;
  HeadResult ReadHead();
  HeadResult AuthenticateHeadObject(OpaqueSyncVaultObject object) const;
  JournalResult AuthenticateHead(const SyncVaultHead& head) const;
  base::expected<SyncVaultJournalRecord, SyncVaultStoreError> ReadJournalRecord(
      const std::string& record_id) const;
  JournalResult AuthenticateJournalObject(
      const std::string& record_id,
      const OpaqueSyncVaultObject& object) const;
  base::expected<void, SyncVaultStoreError> BeginReadCandidate(
      SyncVaultHead head,
      std::string revision);
  WriteResult InitializeVault();
  WriteResult WriteImmutableObject(const std::string& object_id,
                                   std::vector<uint8_t> encrypted_data);
  bool AdvanceCheckpoint(const SyncVaultHead& head);
  void MaybeCollectEpochs(const SyncVaultHead& head);

  std::unique_ptr<SyncVaultTransport> transport_;
  std::unique_ptr<SyncVaultCryptographer> cryptographer_;
  SyncVaultCheckpoint checkpoint_;
  PersistCheckpointCallback persist_checkpoint_callback_;
  const std::string object_prefix_;
  const std::string descriptor_object_id_;
  const std::string head_object_id_;
  std::optional<uint32_t> attempted_collection_floor_;

  struct ReadCursor;
  std::unique_ptr<ReadCursor> read_cursor_;

  SEQUENCE_CHECKER(sequence_checker_);
};

}  // namespace noventra_sync

#endif  // COMPONENTS_NOVENTRA_SYNC_VAULT_SYNC_VAULT_STORE_H_
