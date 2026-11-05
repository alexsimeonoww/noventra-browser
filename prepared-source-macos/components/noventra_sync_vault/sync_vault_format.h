// Copyright 2026 The Helium Authors
// You can use, redistribute, and/or modify this source code under
// the terms of the GPL-3.0 license that can be found in the LICENSE file.

#ifndef COMPONENTS_NOVENTRA_SYNC_VAULT_SYNC_VAULT_FORMAT_H_
#define COMPONENTS_NOVENTRA_SYNC_VAULT_SYNC_VAULT_FORMAT_H_

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "base/containers/span.h"
#include "base/types/expected.h"
#include "components/noventra_sync_vault/sync_vault_limits.h"

namespace noventra_sync {

inline constexpr uint32_t kSyncVaultFormatVersion = 1;
inline constexpr size_t kSyncVaultMaximumStateChunks = 100000;
inline constexpr size_t kSyncVaultMaximumJournalAncestryRecords = 100000;

enum class SyncVaultFormatError {
  kMalformed,
  kUnsupportedVersion,
  kLimitExceeded,
  kInvalidValue,
};

struct SyncVaultDescriptor {
  std::string vault_uuid;
  int64_t creation_time_millis = 0;

  bool operator==(const SyncVaultDescriptor&) const = default;
};

// Generation zero is the only valid empty head. Every later head
// identifies and authenticates one journal record.
struct SyncVaultHead {
  uint64_t generation = 0;
  std::string record_id;
  uint32_t epoch = 0;
  uint64_t epoch_start_generation = 0;
  uint32_t minimum_retained_epoch = 0;
  uint64_t minimum_retained_generation = 0;
  uint64_t epoch_uploaded_bytes = 0;

  bool operator==(const SyncVaultHead&) const = default;
};

// A journal record references the immutable encrypted chunks that
// reconstruct the complete LoopbackServer state for that generation.
struct SyncVaultObjectReference {
  std::string object_id;
  uint32_t plaintext_size = 0;

  bool operator==(const SyncVaultObjectReference&) const = default;
};

struct SyncVaultJournalRecord {
  uint64_t generation = 0;
  int64_t creation_time_millis = 0;
  uint32_t mutation_count = 0;
  std::string parent_record_id;
  std::vector<SyncVaultObjectReference> state_chunks;
  uint32_t epoch = 0;

  bool operator==(const SyncVaultJournalRecord&) const = default;
};

// This is stored only in OS-protected local state. It is updated after
// the corresponding head and ancestry have authenticated successfully.
struct SyncVaultCheckpoint {
  std::string vault_uuid;
  uint64_t generation = 0;
  std::string record_id;
  uint32_t epoch = 0;

  bool operator==(const SyncVaultCheckpoint&) const = default;
};

using SyncVaultSerializeResult =
    base::expected<std::vector<uint8_t>, SyncVaultFormatError>;
template <typename T>
using SyncVaultParseResult = base::expected<T, SyncVaultFormatError>;

SyncVaultSerializeResult SerializeSyncVaultDescriptor(
    const SyncVaultDescriptor& descriptor);
SyncVaultParseResult<SyncVaultDescriptor> ParseSyncVaultDescriptor(
    base::span<const uint8_t> serialized);

SyncVaultSerializeResult SerializeSyncVaultHead(const SyncVaultHead& head);
SyncVaultParseResult<SyncVaultHead> ParseSyncVaultHead(
    base::span<const uint8_t> serialized);

SyncVaultSerializeResult SerializeSyncVaultJournalRecord(
    const SyncVaultJournalRecord& record);
SyncVaultParseResult<SyncVaultJournalRecord> ParseSyncVaultJournalRecord(
    base::span<const uint8_t> serialized);

SyncVaultSerializeResult SerializeSyncVaultCheckpoint(
    const SyncVaultCheckpoint& checkpoint);
SyncVaultParseResult<SyncVaultCheckpoint> ParseSyncVaultCheckpoint(
    base::span<const uint8_t> serialized);

struct AuthenticatedSyncVaultJournalRecord {
  std::string record_id;
  uint64_t generation = 0;
  std::string parent_record_id;
};

enum class SyncVaultHistoryValidationResult {
  // The candidate is the same generation and record as the checkpoint
  // Accept it without advancing the checkpoint.
  kAcceptCurrent,
  // The ancestry links the newer candidate to the checkpoint.
  // Accept it and advance the checkpoint after loading its state.
  kAcceptDescendant,
  // The checkpoint predates retained history. Accept the candidate as
  // the new baseline and advance the checkpoint.
  kAcceptCompacted,
  // The candidate generation is older than the checkpoint. Reject and
  // report a rollback.
  kRollback,
  // The candidate conflicts with or does not descend from the
  // checkpoint. Reject and report a divergent history.
  kFork,
  // More ancestry is needed before the candidate can be classified.
  // Retry after the missing history may have become available.
  kIncompleteHistory,
  // The checkpoint, head, or supplied ancestry is structurally
  // inconsistent. Reject as malformed.
  kInvalidHistory,
};

SyncVaultHistoryValidationResult ValidateSyncVaultHistory(
    const SyncVaultCheckpoint& checkpoint,
    const SyncVaultHead& candidate_head,
    base::span<const AuthenticatedSyncVaultJournalRecord>
        newest_first_ancestry);

}  // namespace noventra_sync

#endif  // COMPONENTS_NOVENTRA_SYNC_VAULT_SYNC_VAULT_FORMAT_H_
