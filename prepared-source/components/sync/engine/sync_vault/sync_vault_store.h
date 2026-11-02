// Copyright 2026 The Helium Authors
// You can use, redistribute, and/or modify this source code under
// the terms of the GPL-3.0 license that can be found in the LICENSE file.

#ifndef COMPONENTS_SYNC_ENGINE_SYNC_VAULT_SYNC_VAULT_STORE_H_
#define COMPONENTS_SYNC_ENGINE_SYNC_VAULT_SYNC_VAULT_STORE_H_

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "base/containers/span.h"
#include "base/functional/callback.h"
#include "base/memory/weak_ptr.h"
#include "base/sequence_checker.h"
#include "base/time/time.h"
#include "base/types/expected.h"
#include "base/unguessable_token.h"
#include "components/sync/engine/sync_vault/sync_vault_format.h"
#include "components/sync/engine/sync_vault/sync_vault_transport.h"

namespace syncer {

class CancelationSignal;
class SyncVaultCryptographer;
class SyncVaultStateCodec;

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

class SyncVaultState {
 public:
  SyncVaultState(const SyncVaultState&) = delete;
  SyncVaultState& operator=(const SyncVaultState&) = delete;
  SyncVaultState(SyncVaultState&&) = default;
  SyncVaultState& operator=(SyncVaultState&&) = default;
  ~SyncVaultState() = default;

  const std::vector<uint8_t>& data() const { return data_; }
  size_t state_size() const { return data_.size(); }
  const SyncVaultHead& head() const { return head_; }
  const std::optional<SyncVaultCommitHeader>& commit_header() const {
    return commit_header_;
  }
  const std::string& head_revision() const { return head_revision_; }
  uint64_t generation() const {
    return commit_header_ ? commit_header_->generation : 0;
  }

 private:
  friend class SyncVaultStore;

  SyncVaultState(std::vector<uint8_t> data,
                 SyncVaultHead head,
                 std::optional<SyncVaultCommitHeader> commit_header,
                 std::string head_revision,
                 base::UnguessableToken store_token);

  std::vector<uint8_t> data_;
  SyncVaultHead head_;
  std::optional<SyncVaultCommitHeader> commit_header_;
  std::string head_revision_;
  base::UnguessableToken store_token_;
};

// Implements the browser-owned encrypted journal over an opaque object
// provider. Immutable commits and any external payload chunks are installed
// first; only the fixed head is conditionally replaced.
class SyncVaultStore {
 public:
  using PersistCheckpointCallback =
      base::RepeatingCallback<bool(const SyncVaultCheckpoint&)>;
  using CreateResult =
      base::expected<std::unique_ptr<SyncVaultStore>, SyncVaultStoreError>;
  using ReadResult = base::expected<SyncVaultState, SyncVaultStoreError>;
  enum class RefreshOutcome { kUnchanged, kChanged };
  using RefreshResult = base::expected<RefreshOutcome, SyncVaultStoreError>;
  using CommitResult = base::expected<SyncVaultState, SyncVaultStoreError>;

  static CreateResult Create(
      std::unique_ptr<SyncVaultTransport> transport,
      std::unique_ptr<SyncVaultStateCodec> state_codec,
      base::span<const uint8_t> vault_key,
      std::string vault_uuid,
      std::optional<SyncVaultCheckpoint> checkpoint,
      PersistCheckpointCallback persist_checkpoint_callback);

  SyncVaultStore(const SyncVaultStore&) = delete;
  SyncVaultStore& operator=(const SyncVaultStore&) = delete;
  ~SyncVaultStore();

  void SetCancelationSignal(CancelationSignal* cancelation_signal);
  ReadResult ReadState();
  // Authenticates the current head and the commit ancestry needed to anchor it
  // without fetching state payloads or advancing the local checkpoint.
  base::expected<void, SyncVaultStoreError> VerifyInitializedVault();
  // Authenticates the current head and replaces `loaded_state` only when its
  // commit differs. A provider-only revision change is updated in place.
  RefreshResult RefreshState(SyncVaultState* loaded_state);
  // Creates an authenticated empty head when needed, then reads it back through
  // the normal verification path. This is intended
  // for new-vault setup; joining an existing vault must use ReadState() so a
  // missing remote history cannot be silently initialized.
  ReadResult InitializeEmptyVaultAndReadState();
  // Publishes a trusted local snapshot or smaller delta and retains the
  // snapshot in the returned state for subsequent incremental refreshes.
  CommitResult CommitMutation(const SyncVaultState& base_state,
                              std::optional<std::vector<uint8_t>> delta,
                              std::vector<uint8_t> snapshot);

  const SyncVaultCheckpoint& checkpoint() const { return checkpoint_; }

 private:
  SyncVaultStore(std::unique_ptr<SyncVaultTransport> transport,
                 std::unique_ptr<SyncVaultStateCodec> state_codec,
                 std::unique_ptr<SyncVaultCryptographer> cryptographer,
                 SyncVaultCheckpoint checkpoint,
                 PersistCheckpointCallback persist_checkpoint_callback);

  using ObjectResult =
      base::expected<std::optional<OpaqueSyncVaultObject>, SyncVaultStoreError>;
  struct VersionedHead {
    SyncVaultHead head;
    std::string revision;
  };

  using HeadResult = base::expected<VersionedHead, SyncVaultStoreError>;
  using CommitObjectResult =
      base::expected<SyncVaultCommit, SyncVaultStoreError>;
  using WriteResult = base::expected<std::string, SyncVaultStoreError>;
  using StateDataResult =
      base::expected<std::vector<uint8_t>, SyncVaultStoreError>;

  struct CommitEntry {
    SyncVaultObjectDigest object_digest;
    SyncVaultCommit commit;
  };

  struct AuthenticatedHead {
    // Starts with the head's commit and, when needed for checkpoint
    // authentication, continues toward the checkpoint without including it.
    std::vector<CommitEntry> commits_newest_first;
  };

  using AuthenticatedHeadResult =
      base::expected<AuthenticatedHead, SyncVaultStoreError>;

  ObjectResult ReadObject(const std::string& object_id,
                          size_t maximum_bytes) const;
  WriteResult WriteObject(const std::string& object_id,
                          std::optional<std::string> expected_revision,
                          std::vector<uint8_t> data,
                          std::optional<uint32_t> retention_epoch);
  HeadResult ReadHead();
  AuthenticatedHeadResult AuthenticateHead(const SyncVaultHead& head) const;
  bool OwnsState(const SyncVaultState& state) const;
  bool IsAtCheckpoint(const SyncVaultState& state) const;
  ReadResult ReadStateForHead(SyncVaultHead head,
                              std::string head_revision,
                              const SyncVaultState* loaded_state);
  StateDataResult TakePayload(SyncVaultCommit* commit) const;
  // Chooses inline or external storage and returns the serialized commit.
  StateDataResult PrepareCommitPayload(base::span<const uint8_t> payload,
                                       SyncVaultCommit* commit);
  StateDataResult ReconstructState(AuthenticatedHead* authenticated) const;
  StateDataResult ApplyCommitPayloads(
      base::span<const uint8_t> base_state,
      base::span<CommitEntry> newest_first) const;
  CommitObjectResult ReadCommit(
      const SyncVaultObjectDigest& commit_digest) const;
  WriteResult InitializeVault();
  WriteResult WriteImmutableObject(const SyncVaultObjectDigest& object_digest,
                                   std::vector<uint8_t> encrypted_data,
                                   uint32_t retention_epoch);
  bool AdvanceCheckpoint(const SyncVaultHead& head,
                         const SyncVaultCommitHeader& commit);
  void MaybeCollectEpochs(const SyncVaultCommit& commit);
  void OnEpochsCollected(uint32_t floor,
                         SyncVaultTransport::CollectResult result);

  std::unique_ptr<SyncVaultTransport> transport_;
  std::unique_ptr<SyncVaultStateCodec> state_codec_;
  std::unique_ptr<SyncVaultCryptographer> cryptographer_;
  SyncVaultCheckpoint checkpoint_;
  PersistCheckpointCallback persist_checkpoint_callback_;
  std::optional<uint32_t> last_collection_floor_;
  base::TimeTicks next_collection_attempt_;
  bool collection_in_progress_ = false;
  const base::UnguessableToken store_token_ = base::UnguessableToken::Create();

  SEQUENCE_CHECKER(sequence_checker_);
  base::WeakPtrFactory<SyncVaultStore> weak_ptr_factory_{this};
};

}  // namespace syncer

#endif  // COMPONENTS_SYNC_ENGINE_SYNC_VAULT_SYNC_VAULT_STORE_H_
