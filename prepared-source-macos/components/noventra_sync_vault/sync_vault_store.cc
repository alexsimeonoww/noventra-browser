// Copyright 2026 The Helium Authors
// You can use, redistribute, and/or modify this source code under
// the terms of the GPL-3.0 license that can be found in the LICENSE file.

#include "components/noventra_sync_vault/sync_vault_store.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "base/check.h"
#include "base/containers/span.h"
#include "base/memory/ptr_util.h"
#include "base/notreached.h"
#include "base/numerics/safe_conversions.h"
#include "base/strings/strcat.h"
#include "base/strings/string_number_conversions.h"
#include "base/time/time.h"
#include "components/noventra_sync_vault/sync_vault_cryptographer.h"
#include "crypto/hash.h"

namespace noventra_sync {

namespace {

constexpr std::string_view kObjectRootPrefix = "helium/v1/";
constexpr std::string_view kDescriptorSuffix = "/descriptor";
constexpr char kEpochSegment[] = "epoch/";
constexpr size_t kMaximumMetadataContainerBytes =
    kSyncVaultMaximumMetadataPlaintextBytes + kSyncVaultContainerOverheadBytes;
constexpr size_t kMaximumJournalContainerBytes =
    kSyncVaultMaximumJournalPlaintextBytes + kSyncVaultContainerOverheadBytes;
constexpr size_t kContentDigestHexLength = 64;
// These are writer rollover policy, not wire-format validity limits. Readers
// accept larger authenticated epochs produced by another conforming writer.
constexpr uint64_t kEpochCommitTarget = 256;
constexpr uint64_t kEpochUploadedBytesTarget = 512 * 1024 * 1024;

SyncVaultStoreError MapTransportError(SyncVaultTransportError error) {
  switch (error) {
    case SyncVaultTransportError::kAuthenticationRequired:
      return SyncVaultStoreError::kAuthenticationRequired;
    case SyncVaultTransportError::kConflict:
      return SyncVaultStoreError::kConflict;
    case SyncVaultTransportError::kNetworkError:
      return SyncVaultStoreError::kNetworkError;
    case SyncVaultTransportError::kTemporaryError:
      return SyncVaultStoreError::kTemporaryError;
    case SyncVaultTransportError::kQuotaExceeded:
      return SyncVaultStoreError::kQuotaExceeded;
    case SyncVaultTransportError::kAccessDenied:
      return SyncVaultStoreError::kAccessDenied;
    case SyncVaultTransportError::kInvalidData:
      return SyncVaultStoreError::kInvalidData;
    case SyncVaultTransportError::kUnsupported:
      return SyncVaultStoreError::kUnsupported;
    case SyncVaultTransportError::kAborted:
      return SyncVaultStoreError::kAborted;
  }
  NOTREACHED();
}

std::string ContentAddressedObjectId(std::string_view prefix,
                                     base::span<const uint8_t> encrypted_data) {
  return std::string(prefix) +
         base::HexEncode(crypto::hash::Sha256(encrypted_data));
}

SyncVaultStoreError MapCryptoError(SyncVaultCryptoError error) {
  switch (error) {
    case SyncVaultCryptoError::kInvalidKey:
      return SyncVaultStoreError::kInvalidVaultKey;
    case SyncVaultCryptoError::kInvalidContext:
    case SyncVaultCryptoError::kMalformedContainer:
      return SyncVaultStoreError::kMalformedObject;
    case SyncVaultCryptoError::kObjectTooLarge:
      return SyncVaultStoreError::kObjectTooLarge;
    case SyncVaultCryptoError::kAuthenticationFailed:
      return SyncVaultStoreError::kAuthenticationFailed;
  }
  NOTREACHED();
}

SyncVaultStoreError MapFormatError(SyncVaultFormatError error) {
  switch (error) {
    case SyncVaultFormatError::kMalformed:
    case SyncVaultFormatError::kInvalidValue:
      return SyncVaultStoreError::kMalformedObject;
    case SyncVaultFormatError::kUnsupportedVersion:
      return SyncVaultStoreError::kUnsupportedVersion;
    case SyncVaultFormatError::kLimitExceeded:
      return SyncVaultStoreError::kObjectTooLarge;
  }
  NOTREACHED();
}

bool HasValidContentAddress(const std::string& object_id,
                            base::span<const uint8_t> encrypted_data) {
  const size_t digest_start = object_id.rfind('/');
  return digest_start != std::string::npos &&
         object_id.size() - digest_start - 1 == kContentDigestHexLength &&
         object_id.substr(digest_start + 1) ==
             base::HexEncode(crypto::hash::Sha256(encrypted_data));
}

std::optional<uint32_t> GetObjectEpochForRole(const std::string& object_prefix,
                                              const std::string& object_id,
                                              std::string_view role) {
  const std::string epoch_prefix = object_prefix + kEpochSegment;
  if (!object_id.starts_with(epoch_prefix)) {
    return std::nullopt;
  }
  const size_t epoch_end = object_id.find('/', epoch_prefix.size());
  if (epoch_end == std::string::npos || epoch_end == epoch_prefix.size()) {
    return std::nullopt;
  }
  const std::string_view epoch_component = std::string_view(object_id).substr(
      epoch_prefix.size(), epoch_end - epoch_prefix.size());
  const std::string_view object_suffix =
      std::string_view(object_id).substr(epoch_end + 1);
  uint32_t epoch = 0;
  if (!base::StringToUint(epoch_component, &epoch) ||
      base::NumberToString(epoch) != epoch_component ||
      object_suffix.size() != role.size() + kContentDigestHexLength ||
      !object_suffix.starts_with(role)) {
    return std::nullopt;
  }
  return epoch;
}

bool IsObjectForRole(const std::string& object_prefix,
                     const std::string& object_id,
                     std::string_view role) {
  return GetObjectEpochForRole(object_prefix, object_id, role).has_value();
}

std::string GetSyncVaultObjectPrefix(const std::string& vault_uuid) {
  return base::StrCat({kObjectRootPrefix, vault_uuid, "/"});
}

std::string GetSyncVaultDescriptorObjectId(const std::string& vault_uuid) {
  return base::StrCat({kObjectRootPrefix, vault_uuid, kDescriptorSuffix});
}

std::string GetSyncVaultEpochObjectPrefix(const std::string& vault_uuid,
                                          uint32_t epoch) {
  return base::StrCat({GetSyncVaultObjectPrefix(vault_uuid), kEpochSegment,
                       base::NumberToString(epoch), "/"});
}

}  // namespace

// Only compact authenticated links survive a slice, never a cache of encrypted
// journal objects. `verified` may advance internally after a complete candidate
// when a fresh head moves, but `anchor` and the durable checkpoint stay fixed
// until the final reconciled state is returned.
struct SyncVaultStore::ReadCursor {
  enum class Phase { kDescriptor, kHead, kJournal, kChunks, kFreshHead };
  explicit ReadCursor(const SyncVaultCheckpoint& checkpoint)
      : anchor(checkpoint), verified(checkpoint) {}

  const SyncVaultCheckpoint anchor;
  SyncVaultCheckpoint verified;
  Phase phase = Phase::kDescriptor;
  SyncVaultState state;
  std::vector<AuthenticatedSyncVaultJournalRecord> ancestry;
  std::string next_record_id;
  uint64_t next_generation = 0;
  bool skip_ancestry = false;
  size_t next_chunk = 0;
};

// static
SyncVaultStore::CreateResult SyncVaultStore::Create(
    std::unique_ptr<SyncVaultTransport> transport,
    base::span<const uint8_t> vault_key,
    std::string vault_uuid,
    std::optional<SyncVaultCheckpoint> checkpoint,
    PersistCheckpointCallback persist_checkpoint_callback) {
  if (!transport || !persist_checkpoint_callback) {
    return base::unexpected(SyncVaultStoreError::kInvalidData);
  }
  auto cryptographer =
      SyncVaultCryptographer::Create(vault_key, std::move(vault_uuid));
  if (!cryptographer.has_value()) {
    return base::unexpected(MapCryptoError(cryptographer.error()));
  }
  if (checkpoint &&
      checkpoint->vault_uuid != cryptographer.value()->vault_uuid()) {
    return base::unexpected(SyncVaultStoreError::kInvalidData);
  }
  if (checkpoint && checkpoint->generation > 0) {
    const std::optional<uint32_t> record_epoch = GetObjectEpochForRole(
        GetSyncVaultObjectPrefix(cryptographer.value()->vault_uuid()),
        checkpoint->record_id, "journal/");
    if (!record_epoch || *record_epoch != checkpoint->epoch) {
      return base::unexpected(SyncVaultStoreError::kInvalidData);
    }
  }
  SyncVaultCheckpoint effective_checkpoint =
      checkpoint.value_or(SyncVaultCheckpoint{
          cryptographer.value()->vault_uuid(), 0, std::string(), 0});
  if (!SerializeSyncVaultCheckpoint(effective_checkpoint).has_value()) {
    return base::unexpected(SyncVaultStoreError::kInvalidData);
  }
  return base::WrapUnique(new SyncVaultStore(
      std::move(transport), std::move(cryptographer.value()),
      std::move(effective_checkpoint), std::move(persist_checkpoint_callback)));
}

SyncVaultStore::SyncVaultStore(
    std::unique_ptr<SyncVaultTransport> transport,
    std::unique_ptr<SyncVaultCryptographer> cryptographer,
    SyncVaultCheckpoint checkpoint,
    PersistCheckpointCallback persist_checkpoint_callback)
    : transport_(std::move(transport)),
      cryptographer_(std::move(cryptographer)),
      checkpoint_(std::move(checkpoint)),
      persist_checkpoint_callback_(std::move(persist_checkpoint_callback)),
      object_prefix_(GetSyncVaultObjectPrefix(cryptographer_->vault_uuid())),
      descriptor_object_id_(
          GetSyncVaultDescriptorObjectId(cryptographer_->vault_uuid())),
      head_object_id_(object_prefix_ + "head") {
  // Production stores are assembled on the UI sequence and then transferred
  // to the Sync sequence with their backend. Setup verification stores bind
  // naturally on the ThreadPool sequence that first uses them.
  DETACH_FROM_SEQUENCE(sequence_checker_);
}

SyncVaultStore::~SyncVaultStore() = default;

void SyncVaultStore::SetCancelationSignal(
    CancelationSignal* cancelation_signal) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  CHECK(cancelation_signal);
  AbortRead();
  transport_->SetCancelationSignal(cancelation_signal);
}

void SyncVaultStore::AbortRead() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  read_cursor_.reset();
}

base::expected<void, SyncVaultStoreError> SyncVaultStore::BeginReadCandidate(
    SyncVaultHead head,
    std::string revision) {
  auto& cursor = *read_cursor_;
  const auto& checkpoint = cursor.verified;
  const auto validation = ValidateSyncVaultHistory(checkpoint, head, {});
  if (head.generation <= checkpoint.generation) {
    if (validation == SyncVaultHistoryValidationResult::kRollback) {
      return base::unexpected(SyncVaultStoreError::kRollback);
    }
    if (validation != SyncVaultHistoryValidationResult::kAcceptCurrent) {
      return base::unexpected(SyncVaultStoreError::kFork);
    }
  }
  cursor.skip_ancestry =
      head.generation <= checkpoint.generation ||
      checkpoint.generation < head.minimum_retained_generation;
  if (head.generation > checkpoint.generation && cursor.skip_ancestry &&
      validation != SyncVaultHistoryValidationResult::kAcceptCompacted) {
    return base::unexpected(SyncVaultStoreError::kMalformedObject);
  }
  // Keep the inherited ancestry limit relative to the original durable
  // anchor, including when a moving head is reconciled in several slices.
  if ((!cursor.skip_ancestry && checkpoint.generation > 0 &&
       head.generation - checkpoint.generation >
           kSyncVaultMaximumJournalAncestryRecords) ||
      (cursor.anchor.generation > 0 &&
       cursor.anchor.generation >= head.minimum_retained_generation &&
       head.generation - cursor.anchor.generation >
           kSyncVaultMaximumJournalAncestryRecords)) {
    return base::unexpected(SyncVaultStoreError::kObjectTooLarge);
  }
  if (head.generation > 0 &&
      !IsObjectForRole(object_prefix_, head.record_id, "journal/")) {
    return base::unexpected(SyncVaultStoreError::kMalformedObject);
  }
  cursor.state = SyncVaultState{{}, std::move(head), std::move(revision), true};
  cursor.ancestry.clear();
  cursor.next_chunk = 0;
  cursor.next_record_id = cursor.state.head.record_id;
  cursor.next_generation = cursor.state.head.generation;
  cursor.phase = cursor.next_generation == 0 ? ReadCursor::Phase::kFreshHead
                                             : ReadCursor::Phase::kJournal;
  return base::ok();
}

SyncVaultStore::RefreshResult SyncVaultStore::ReadStateIncrementally(
    const ReadBudget& budget) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  auto fail = [this, &budget](SyncVaultStoreError error) -> RefreshResult {
    if (error == SyncVaultStoreError::kNetworkError ||
        error == SyncVaultStoreError::kTemporaryError) {
      // A request can exhaust the deadline after admission. Retain completed
      // links/chunks so a slow store cannot cause each slice to start over.
      if (base::TimeTicks::Now() >= budget.deadline) {
        return std::nullopt;
      }
      return base::unexpected(error);
    }
    AbortRead();
    return base::unexpected(error);
  };
  if (budget.maximum_objects == 0 ||
      budget.maximum_bytes < kSyncVaultMaximumEncryptedObjectBytes ||
      budget.deadline.is_null() || !budget.is_cancelled) {
    return fail(SyncVaultStoreError::kInvalidData);
  }
  if (!read_cursor_) {
    read_cursor_ = std::make_unique<ReadCursor>(checkpoint_);
  }
  CHECK(read_cursor_->anchor == checkpoint_);
  size_t objects_left = budget.maximum_objects;
  size_t bytes_left = budget.maximum_bytes;
  auto can_read = [&](size_t maximum_bytes) {
    return objects_left != 0 && bytes_left >= maximum_bytes &&
           base::TimeTicks::Now() < budget.deadline;
  };
  auto read = [&](const std::string& id, size_t maximum_bytes) -> ObjectResult {
    --objects_left;
    auto result = ReadObject(id, maximum_bytes);
    if (budget.is_cancelled.Run()) {
      return base::unexpected(SyncVaultStoreError::kAborted);
    }
    if (result.has_value() && result->has_value()) {
      bytes_left -= result->value().data.size();
    }
    return result;
  };
  auto& cursor = *read_cursor_;
  while (true) {
    if (budget.is_cancelled.Run()) {
      return fail(SyncVaultStoreError::kAborted);
    }
    if (base::TimeTicks::Now() >= budget.deadline) {
      return std::nullopt;
    }
    switch (cursor.phase) {
      case ReadCursor::Phase::kDescriptor: {
        if (!can_read(kMaximumMetadataContainerBytes)) {
          return std::nullopt;
        }
        auto object =
            read(descriptor_object_id_, kMaximumMetadataContainerBytes);
        if (!object.has_value()) {
          return fail(object.error());
        }
        if (!object->has_value()) {
          if (checkpoint_.generation != 0) {
            return fail(SyncVaultStoreError::kRollback);
          }
          AbortRead();
          return SyncVaultState();
        }
        auto validation = ValidateDescriptor(object->value());
        if (!validation.has_value()) {
          return fail(validation.error());
        }
        cursor.phase = ReadCursor::Phase::kHead;
        break;
      }
      case ReadCursor::Phase::kHead:
      case ReadCursor::Phase::kFreshHead: {
        if (!can_read(kMaximumMetadataContainerBytes)) {
          return std::nullopt;
        }
        auto object = read(head_object_id_, kMaximumMetadataContainerBytes);
        if (!object.has_value()) {
          return fail(object.error());
        }
        if (!object->has_value()) {
          if (cursor.phase == ReadCursor::Phase::kHead &&
              checkpoint_.generation == 0) {
            AbortRead();
            return SyncVaultState();
          }
          return fail(SyncVaultStoreError::kIncompleteHistory);
        }
        auto head = AuthenticateHeadObject(std::move(object->value()));
        if (!head.has_value()) {
          return fail(head.error());
        }
        if (cursor.phase == ReadCursor::Phase::kFreshHead) {
          if (head->first == cursor.state.head) {
            // An ETag may change even when the authenticated head does not.
            // Always return the fresh exact revision for a subsequent CAS.
            cursor.state.head_revision = std::move(head->second);
            if (budget.is_cancelled.Run()) {
              return fail(SyncVaultStoreError::kAborted);
            }
            if (base::TimeTicks::Now() >= budget.deadline) {
              return std::nullopt;  // Refresh again in the next operation.
            }
            if (cursor.state.head.generation > checkpoint_.generation &&
                !AdvanceCheckpoint(cursor.state.head)) {
              return fail(SyncVaultStoreError::kCheckpointPersistenceFailed);
            }
            SyncVaultState result = std::move(cursor.state);
            AbortRead();
            // Incremental reads do not issue optional collection I/O outside
            // their budget. Collection remains available on normal commits.
            return result;
          }
          // This candidate's ancestry AND state are fully authenticated. Use
          // it as an internal anchor without publishing a checkpoint or data.
          cursor.verified = SyncVaultCheckpoint{
              cryptographer_->vault_uuid(), cursor.state.head.generation,
              cursor.state.head.record_id, cursor.state.head.epoch};
        }
        auto started =
            BeginReadCandidate(std::move(head->first), std::move(head->second));
        if (!started.has_value()) {
          return fail(started.error());
        }
        break;
      }
      case ReadCursor::Phase::kJournal: {
        if (!can_read(kMaximumJournalContainerBytes)) {
          return std::nullopt;
        }
        auto object =
            read(cursor.next_record_id, kMaximumJournalContainerBytes);
        if (!object.has_value()) {
          return fail(object.error());
        }
        if (!object->has_value()) {
          return fail(SyncVaultStoreError::kIncompleteHistory);
        }
        auto record =
            AuthenticateJournalObject(cursor.next_record_id, object->value());
        if (!record.has_value()) {
          return fail(record.error());
        }
        if (record->generation != cursor.next_generation ||
            (record->generation > 1 &&
             !IsObjectForRole(object_prefix_, record->parent_record_id,
                              "journal/"))) {
          return fail(SyncVaultStoreError::kMalformedObject);
        }
        const auto& head = cursor.state.head;
        if (record->generation == head.generation) {
          if (record->epoch != head.epoch) {
            return fail(SyncVaultStoreError::kMalformedObject);
          }
          cursor.state.state_chunks = std::move(record->state_chunks);
        }
        if (cursor.skip_ancestry) {
          cursor.phase = ReadCursor::Phase::kChunks;
          break;
        }
        const uint32_t expected_epoch =
            record->generation >= head.epoch_start_generation
                ? head.epoch
                : head.minimum_retained_epoch;
        if (record->epoch != expected_epoch) {
          return fail(SyncVaultStoreError::kMalformedObject);
        }
        cursor.ancestry.push_back({cursor.next_record_id, record->generation,
                                   record->parent_record_id});
        if (cursor.verified.generation == 0 ||
            record->generation == cursor.verified.generation + 1) {
          switch (ValidateSyncVaultHistory(cursor.verified, head,
                                           cursor.ancestry)) {
            case SyncVaultHistoryValidationResult::kAcceptDescendant:
              cursor.phase = ReadCursor::Phase::kChunks;
              break;
            case SyncVaultHistoryValidationResult::kRollback:
              return fail(SyncVaultStoreError::kRollback);
            case SyncVaultHistoryValidationResult::kFork:
              return fail(SyncVaultStoreError::kFork);
            case SyncVaultHistoryValidationResult::kIncompleteHistory:
              return fail(SyncVaultStoreError::kIncompleteHistory);
            default:
              return fail(SyncVaultStoreError::kMalformedObject);
          }
          break;
        }
        if (record->generation <= cursor.verified.generation + 1 ||
            cursor.ancestry.size() >= kSyncVaultMaximumJournalAncestryRecords) {
          return fail(SyncVaultStoreError::kIncompleteHistory);
        }
        cursor.next_record_id = std::move(record->parent_record_id);
        cursor.next_generation = record->generation - 1;
        break;
      }
      case ReadCursor::Phase::kChunks: {
        if (cursor.next_chunk == cursor.state.state_chunks.size()) {
          cursor.phase = ReadCursor::Phase::kFreshHead;
          break;
        }
        const auto& chunk = cursor.state.state_chunks[cursor.next_chunk];
        const auto epoch =
            GetObjectEpochForRole(object_prefix_, chunk.object_id, "entity/");
        if (!epoch || *epoch != cursor.state.head.epoch) {
          return fail(SyncVaultStoreError::kMalformedObject);
        }
        if (!can_read(kSyncVaultMaximumEncryptedObjectBytes)) {
          return std::nullopt;
        }
        if (cursor.next_chunk == 0) {
          // Parsing the authenticated journal already checked this sum against
          // the reconstructed-state limit. Reserve once to bound capacity as
          // well as size while chunks arrive over multiple operations.
          size_t total_size = 0;
          for (const auto& reference : cursor.state.state_chunks) {
            total_size += reference.plaintext_size;
          }
          cursor.state.data.reserve(total_size);
        }
        auto object =
            read(chunk.object_id, kSyncVaultMaximumEncryptedObjectBytes);
        if (!object.has_value()) {
          return fail(object.error());
        }
        if (!object->has_value()) {
          return fail(SyncVaultStoreError::kIncompleteHistory);
        }
        if (!HasValidContentAddress(chunk.object_id, object->value().data)) {
          return fail(SyncVaultStoreError::kAuthenticationFailed);
        }
        auto plaintext = cryptographer_->Decrypt(SyncVaultObjectRole::kEntity,
                                                 object->value().data);
        if (!plaintext.has_value()) {
          return fail(MapCryptoError(plaintext.error()));
        }
        if (plaintext->size() != chunk.plaintext_size ||
            cursor.state.data.size() >
                kSyncVaultMaximumReconstructedBytes - plaintext->size()) {
          return fail(SyncVaultStoreError::kMalformedObject);
        }
        cursor.state.data.insert(cursor.state.data.end(), plaintext->begin(),
                                 plaintext->end());
        ++cursor.next_chunk;
        break;
      }
    }
  }
}

SyncVaultStore::ReadResult SyncVaultStore::ReadState() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  AbortRead();
  ObjectResult descriptor =
      ReadObject(descriptor_object_id_, kMaximumMetadataContainerBytes);
  if (!descriptor.has_value()) {
    return base::unexpected(descriptor.error());
  }
  if (!descriptor->has_value()) {
    if (checkpoint_.generation != 0) {
      return base::unexpected(SyncVaultStoreError::kRollback);
    }
    return SyncVaultState();
  }
  auto descriptor_status = ValidateDescriptor(descriptor->value());
  if (!descriptor_status.has_value()) {
    return base::unexpected(descriptor_status.error());
  }

  HeadResult head_result = ReadHead();
  if (!head_result.has_value()) {
    if (head_result.error() == SyncVaultStoreError::kIncompleteHistory &&
        checkpoint_.generation == 0) {
      return SyncVaultState();
    }
    return base::unexpected(head_result.error());
  }
  SyncVaultHead head = std::move(head_result->first);
  std::string head_revision = std::move(head_result->second);
  if (head.generation == 0) {
    const SyncVaultHistoryValidationResult validation =
        ValidateSyncVaultHistory(checkpoint_, head, {});
    if (validation == SyncVaultHistoryValidationResult::kRollback) {
      return base::unexpected(SyncVaultStoreError::kRollback);
    }
    if (validation != SyncVaultHistoryValidationResult::kAcceptCurrent) {
      return base::unexpected(SyncVaultStoreError::kFork);
    }
    return SyncVaultState{{}, std::move(head), std::move(head_revision), true};
  }

  if (!IsObjectForRole(object_prefix_, head.record_id, "journal/")) {
    return base::unexpected(SyncVaultStoreError::kMalformedObject);
  }

  JournalResult record_result = AuthenticateHead(head);
  if (!record_result.has_value()) {
    return base::unexpected(record_result.error());
  }
  SyncVaultJournalRecord latest_record = std::move(record_result.value());
  const bool should_advance_checkpoint =
      head.generation > checkpoint_.generation;

  std::vector<uint8_t> state;
  size_t total_size = 0;
  for (const SyncVaultObjectReference& chunk : latest_record.state_chunks) {
    const std::optional<uint32_t> chunk_epoch =
        GetObjectEpochForRole(object_prefix_, chunk.object_id, "entity/");
    if (!chunk_epoch || *chunk_epoch != latest_record.epoch) {
      return base::unexpected(SyncVaultStoreError::kMalformedObject);
    }
    ObjectResult object =
        ReadObject(chunk.object_id, kSyncVaultMaximumEncryptedObjectBytes);
    if (!object.has_value()) {
      return base::unexpected(object.error());
    }
    if (!object->has_value()) {
      return base::unexpected(SyncVaultStoreError::kIncompleteHistory);
    }
    if (!HasValidContentAddress(chunk.object_id, object->value().data)) {
      return base::unexpected(SyncVaultStoreError::kAuthenticationFailed);
    }
    auto plaintext = cryptographer_->Decrypt(SyncVaultObjectRole::kEntity,
                                             object->value().data);
    if (!plaintext.has_value()) {
      return base::unexpected(MapCryptoError(plaintext.error()));
    }
    if (plaintext->size() != chunk.plaintext_size ||
        total_size > kSyncVaultMaximumReconstructedBytes - plaintext->size()) {
      return base::unexpected(SyncVaultStoreError::kMalformedObject);
    }
    total_size += plaintext->size();
    state.insert(state.end(), plaintext->begin(), plaintext->end());
  }

  if (should_advance_checkpoint && !AdvanceCheckpoint(head)) {
    return base::unexpected(SyncVaultStoreError::kCheckpointPersistenceFailed);
  }
  MaybeCollectEpochs(head);

  return SyncVaultState{std::move(state), std::move(head),
                        std::move(head_revision), true,
                        std::move(latest_record.state_chunks)};
}

SyncVaultStore::RefreshResult SyncVaultStore::ReadStateIfChanged(
    const SyncVaultState& loaded_state) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  AbortRead();
  if (!loaded_state.initialized || !loaded_state.head_revision) {
    return ReadState();
  }
  HeadResult current = ReadHead();
  if (!current.has_value()) {
    return base::unexpected(current.error());
  }
  if (current->first == loaded_state.head &&
      current->second == loaded_state.head_revision) {
    return std::nullopt;
  }
  ReadResult refreshed = ReadState();
  if (!refreshed.has_value()) {
    return base::unexpected(refreshed.error());
  }
  return std::move(refreshed.value());
}

base::expected<void, SyncVaultStoreError>
SyncVaultStore::VerifyInitializedVault() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  AbortRead();
  ObjectResult descriptor =
      ReadObject(descriptor_object_id_, kMaximumMetadataContainerBytes);
  if (!descriptor.has_value()) {
    return base::unexpected(descriptor.error());
  }
  if (!descriptor->has_value()) {
    return base::unexpected(SyncVaultStoreError::kIncompleteHistory);
  }
  auto descriptor_status = ValidateDescriptor(descriptor->value());
  if (!descriptor_status.has_value()) {
    return descriptor_status;
  }
  HeadResult head = ReadHead();
  if (!head.has_value()) {
    return base::unexpected(head.error());
  }
  const SyncVaultHead& candidate_head = head->first;
  if (candidate_head.generation == 0) {
    const SyncVaultHistoryValidationResult validation =
        ValidateSyncVaultHistory(checkpoint_, candidate_head, {});
    if (validation == SyncVaultHistoryValidationResult::kRollback) {
      return base::unexpected(SyncVaultStoreError::kRollback);
    }
    if (validation != SyncVaultHistoryValidationResult::kAcceptCurrent) {
      return base::unexpected(SyncVaultStoreError::kFork);
    }
  } else {
    JournalResult record = AuthenticateHead(candidate_head);
    if (!record.has_value()) {
      return base::unexpected(record.error());
    }
  }
  if (!AdvanceCheckpoint(candidate_head)) {
    return base::unexpected(SyncVaultStoreError::kCheckpointPersistenceFailed);
  }
  return base::ok();
}

SyncVaultStore::ReadResult SyncVaultStore::InitializeEmptyVaultAndReadState() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  ReadResult state = ReadState();
  if (!state.has_value() || state->initialized) {
    return state;
  }

  WriteResult initialization = InitializeVault();
  if (!initialization.has_value()) {
    return base::unexpected(initialization.error());
  }

  ReadResult verified_state = ReadState();
  if (!verified_state.has_value()) {
    return base::unexpected(verified_state.error());
  }
  if (!verified_state->initialized || !verified_state->head_revision ||
      verified_state->head.generation != 0) {
    return base::unexpected(SyncVaultStoreError::kIncompleteHistory);
  }
  return verified_state;
}

SyncVaultStore::CommitResult SyncVaultStore::CommitState(
    const SyncVaultState& base_state,
    base::span<const uint8_t> state,
    uint32_t mutation_count) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  AbortRead();
  if (mutation_count == 0 || state.empty() ||
      state.size() > kSyncVaultMaximumReconstructedBytes ||
      base_state.head.generation == std::numeric_limits<uint64_t>::max()) {
    return base::unexpected(SyncVaultStoreError::kInvalidData);
  }

  SyncVaultHead base_head = base_state.head;
  std::optional<std::string> expected_head_revision = base_state.head_revision;
  if (!base_state.initialized) {
    if (base_state.head.generation != 0 || expected_head_revision) {
      return base::unexpected(SyncVaultStoreError::kInvalidData);
    }
    WriteResult initialized_revision = InitializeVault();
    if (!initialized_revision.has_value()) {
      return base::unexpected(initialized_revision.error());
    }
    base_head = SyncVaultHead();
    expected_head_revision = std::move(initialized_revision.value());
  } else if (!expected_head_revision) {
    return base::unexpected(SyncVaultStoreError::kInvalidData);
  }

  uint64_t pending_entity_bytes = 0;
  size_t chunk_index = 0;
  for (size_t offset = 0; offset < state.size(); ++chunk_index) {
    const size_t chunk_size =
        std::min(state.size() - offset, kSyncVaultMaximumEntityPlaintextBytes);
    const bool can_reuse =
        chunk_index < base_state.state_chunks.size() &&
        base_state.state_chunks[chunk_index].plaintext_size == chunk_size &&
        offset + chunk_size <= base_state.data.size() &&
        std::ranges::equal(
            state.subspan(offset, chunk_size),
            base::span(base_state.data).subspan(offset, chunk_size));
    if (!can_reuse) {
      pending_entity_bytes += chunk_size + kSyncVaultContainerOverheadBytes;
    }
    offset += chunk_size;
  }

  const uint64_t commits_in_epoch =
      base_head.generation == 0
          ? 0
          : base_head.generation - base_head.epoch_start_generation + 1;
  const bool byte_target_reached =
      pending_entity_bytes > kEpochUploadedBytesTarget ||
      base_head.epoch_uploaded_bytes >
          kEpochUploadedBytesTarget - pending_entity_bytes;
  const bool start_new_epoch =
      base_head.generation > 0 &&
      (commits_in_epoch >= kEpochCommitTarget || byte_target_reached);
  if (start_new_epoch &&
      base_head.epoch == std::numeric_limits<uint32_t>::max()) {
    return base::unexpected(SyncVaultStoreError::kObjectTooLarge);
  }

  const uint64_t new_generation = base_head.generation + 1;
  const uint32_t new_epoch =
      start_new_epoch ? base_head.epoch + 1 : base_head.epoch;
  const uint64_t new_epoch_start_generation =
      start_new_epoch || base_head.generation == 0
          ? new_generation
          : base_head.epoch_start_generation;
  const uint32_t new_minimum_retained_epoch =
      start_new_epoch ? new_epoch - 1 : base_head.minimum_retained_epoch;
  const uint64_t new_minimum_retained_generation =
      start_new_epoch ? (new_epoch <= 1 ? 0 : base_head.epoch_start_generation)
                      : base_head.minimum_retained_generation;
  uint64_t new_epoch_uploaded_bytes =
      start_new_epoch ? 0 : base_head.epoch_uploaded_bytes;

  const std::string epoch_object_prefix =
      GetSyncVaultEpochObjectPrefix(cryptographer_->vault_uuid(), new_epoch);
  const std::string journal_object_prefix = epoch_object_prefix + "journal/";
  const std::string entity_object_prefix = epoch_object_prefix + "entity/";

  std::vector<SyncVaultObjectReference> chunks;
  chunk_index = 0;
  for (size_t offset = 0; offset < state.size(); ++chunk_index) {
    const size_t chunk_size =
        std::min(state.size() - offset, kSyncVaultMaximumEntityPlaintextBytes);
    if (!start_new_epoch && chunk_index < base_state.state_chunks.size() &&
        base_state.state_chunks[chunk_index].plaintext_size == chunk_size &&
        offset + chunk_size <= base_state.data.size() &&
        std::ranges::equal(
            state.subspan(offset, chunk_size),
            base::span(base_state.data).subspan(offset, chunk_size))) {
      chunks.push_back(base_state.state_chunks[chunk_index]);
      offset += chunk_size;
      continue;
    }
    auto encrypted = cryptographer_->Encrypt(SyncVaultObjectRole::kEntity,
                                             state.subspan(offset, chunk_size));
    if (!encrypted.has_value()) {
      return base::unexpected(MapCryptoError(encrypted.error()));
    }
    new_epoch_uploaded_bytes += encrypted->size();
    const std::string object_id =
        ContentAddressedObjectId(entity_object_prefix, encrypted.value());
    WriteResult write_result =
        WriteImmutableObject(object_id, std::move(encrypted.value()));
    if (!write_result.has_value()) {
      return base::unexpected(write_result.error());
    }
    chunks.push_back({object_id, base::checked_cast<uint32_t>(chunk_size)});
    offset += chunk_size;
  }

  SyncVaultJournalRecord record;
  record.generation = new_generation;
  record.creation_time_millis =
      base::Time::Now().InMillisecondsSinceUnixEpoch();
  record.mutation_count = mutation_count;
  record.parent_record_id = base_head.record_id;
  record.state_chunks = chunks;
  record.epoch = new_epoch;
  auto serialized_record = SerializeSyncVaultJournalRecord(record);
  if (!serialized_record.has_value()) {
    return base::unexpected(MapFormatError(serialized_record.error()));
  }
  auto encrypted_record = cryptographer_->Encrypt(SyncVaultObjectRole::kJournal,
                                                  serialized_record.value());
  if (!encrypted_record.has_value()) {
    return base::unexpected(MapCryptoError(encrypted_record.error()));
  }
  new_epoch_uploaded_bytes += encrypted_record->size();
  const std::string record_id =
      ContentAddressedObjectId(journal_object_prefix, encrypted_record.value());
  WriteResult record_write =
      WriteImmutableObject(record_id, std::move(encrypted_record.value()));
  if (!record_write.has_value()) {
    return base::unexpected(record_write.error());
  }

  SyncVaultHead new_head;
  new_head.generation = record.generation;
  new_head.record_id = record_id;
  new_head.epoch = new_epoch;
  new_head.epoch_start_generation = new_epoch_start_generation;
  new_head.minimum_retained_epoch = new_minimum_retained_epoch;
  new_head.minimum_retained_generation = new_minimum_retained_generation;
  new_head.epoch_uploaded_bytes = new_epoch_uploaded_bytes;
  auto serialized_head = SerializeSyncVaultHead(new_head);
  if (!serialized_head.has_value()) {
    return base::unexpected(MapFormatError(serialized_head.error()));
  }
  auto encrypted_head = cryptographer_->Encrypt(SyncVaultObjectRole::kHead,
                                                serialized_head.value());
  if (!encrypted_head.has_value()) {
    return base::unexpected(MapCryptoError(encrypted_head.error()));
  }
  WriteResult head_write =
      WriteObject(head_object_id_, std::move(expected_head_revision),
                  std::move(encrypted_head.value()));
  std::string committed_head_revision;
  if (!head_write.has_value()) {
    if (head_write.error() == SyncVaultStoreError::kConflict) {
      return base::unexpected(SyncVaultStoreError::kConflict);
    }
    // Resolve an ambiguous commit response by authenticating the current head.
    HeadResult current = ReadHead();
    if (!current.has_value() || current->first != new_head) {
      return base::unexpected(head_write.error());
    }
    committed_head_revision = std::move(current->second);
  } else {
    committed_head_revision = std::move(head_write.value());
  }
  if (!AdvanceCheckpoint(new_head)) {
    return base::unexpected(SyncVaultStoreError::kCheckpointPersistenceFailed);
  }
  MaybeCollectEpochs(new_head);
  return SyncVaultState{std::vector<uint8_t>(state.begin(), state.end()),
                        std::move(new_head), std::move(committed_head_revision),
                        true, std::move(chunks)};
}

SyncVaultStore::ObjectResult SyncVaultStore::ReadObject(
    const std::string& object_id,
    size_t maximum_bytes) const {
  if (object_id.empty() ||
      object_id.size() > kSyncVaultMaximumIdentifierBytes ||
      maximum_bytes == 0 ||
      maximum_bytes > kSyncVaultMaximumEncryptedObjectBytes) {
    return base::unexpected(SyncVaultStoreError::kInvalidData);
  }
  SyncVaultTransport::ReadResult result =
      transport_->ReadObject(object_id, maximum_bytes);
  if (!result.has_value()) {
    return base::unexpected(MapTransportError(result.error()));
  }
  if (result->has_value() &&
      (result->value().data.size() > maximum_bytes ||
       result->value().revision.empty() ||
       result->value().revision.size() > kSyncVaultMaximumIdentifierBytes)) {
    return base::unexpected(SyncVaultStoreError::kInvalidData);
  }
  return std::move(result.value());
}

SyncVaultStore::WriteResult SyncVaultStore::WriteObject(
    const std::string& object_id,
    std::optional<std::string> expected_revision,
    std::vector<uint8_t> data) {
  if (object_id.empty() ||
      object_id.size() > kSyncVaultMaximumIdentifierBytes || data.empty() ||
      data.size() > kSyncVaultMaximumEncryptedObjectBytes ||
      (expected_revision &&
       (expected_revision->empty() ||
        expected_revision->size() > kSyncVaultMaximumIdentifierBytes))) {
    return base::unexpected(SyncVaultStoreError::kInvalidData);
  }
  SyncVaultTransport::WriteResult result = transport_->WriteObject(
      object_id, std::move(expected_revision), std::move(data));
  if (!result.has_value()) {
    return base::unexpected(MapTransportError(result.error()));
  }
  if (result->empty() || result->size() > kSyncVaultMaximumIdentifierBytes) {
    return base::unexpected(SyncVaultStoreError::kInvalidData);
  }
  return std::move(result.value());
}

base::expected<void, SyncVaultStoreError> SyncVaultStore::ValidateDescriptor(
    const OpaqueSyncVaultObject& object) const {
  auto plaintext =
      cryptographer_->Decrypt(SyncVaultObjectRole::kDescriptor, object.data);
  if (!plaintext.has_value()) {
    return base::unexpected(MapCryptoError(plaintext.error()));
  }
  auto descriptor = ParseSyncVaultDescriptor(plaintext.value());
  if (!descriptor.has_value()) {
    return base::unexpected(MapFormatError(descriptor.error()));
  }
  if (descriptor->vault_uuid != cryptographer_->vault_uuid()) {
    return base::unexpected(SyncVaultStoreError::kAuthenticationFailed);
  }
  return base::ok();
}

SyncVaultStore::HeadResult SyncVaultStore::ReadHead() {
  ObjectResult object =
      ReadObject(head_object_id_, kMaximumMetadataContainerBytes);
  if (!object.has_value()) {
    return base::unexpected(object.error());
  }
  if (!object->has_value()) {
    return base::unexpected(SyncVaultStoreError::kIncompleteHistory);
  }
  return AuthenticateHeadObject(std::move(object->value()));
}

SyncVaultStore::HeadResult SyncVaultStore::AuthenticateHeadObject(
    OpaqueSyncVaultObject object) const {
  auto plaintext =
      cryptographer_->Decrypt(SyncVaultObjectRole::kHead, object.data);
  if (!plaintext.has_value()) {
    return base::unexpected(MapCryptoError(plaintext.error()));
  }
  auto head = ParseSyncVaultHead(plaintext.value());
  if (!head.has_value()) {
    return base::unexpected(MapFormatError(head.error()));
  }
  return std::make_pair(std::move(head.value()), std::move(object.revision));
}

SyncVaultStore::JournalResult SyncVaultStore::AuthenticateHead(
    const SyncVaultHead& head) const {
  if (head.generation == 0 ||
      !IsObjectForRole(object_prefix_, head.record_id, "journal/")) {
    return base::unexpected(SyncVaultStoreError::kMalformedObject);
  }

  const bool is_current_checkpoint = head.generation <= checkpoint_.generation;
  if (is_current_checkpoint) {
    const SyncVaultHistoryValidationResult validation =
        ValidateSyncVaultHistory(checkpoint_, head, {});
    if (validation == SyncVaultHistoryValidationResult::kRollback) {
      return base::unexpected(SyncVaultStoreError::kRollback);
    }
    if (validation != SyncVaultHistoryValidationResult::kAcceptCurrent) {
      return base::unexpected(SyncVaultStoreError::kFork);
    }
  }

  JournalResult latest_result = ReadJournalRecord(head.record_id);
  if (!latest_result.has_value()) {
    return base::unexpected(latest_result.error());
  }
  SyncVaultJournalRecord latest = std::move(latest_result.value());
  if (latest.generation != head.generation || latest.epoch != head.epoch ||
      (latest.generation > 1 &&
       !IsObjectForRole(object_prefix_, latest.parent_record_id, "journal/"))) {
    return base::unexpected(SyncVaultStoreError::kMalformedObject);
  }

  if (is_current_checkpoint) {
    return latest;
  }

  if (checkpoint_.generation < head.minimum_retained_generation) {
    if (ValidateSyncVaultHistory(checkpoint_, head, {}) !=
        SyncVaultHistoryValidationResult::kAcceptCompacted) {
      return base::unexpected(SyncVaultStoreError::kMalformedObject);
    }
    return latest;
  }

  if (checkpoint_.generation > 0 &&
      head.generation - checkpoint_.generation >
          kSyncVaultMaximumJournalAncestryRecords) {
    return base::unexpected(SyncVaultStoreError::kObjectTooLarge);
  }

  std::vector<AuthenticatedSyncVaultJournalRecord> ancestry;
  std::string record_id = head.record_id;
  uint64_t record_generation = latest.generation;
  uint32_t record_epoch = latest.epoch;
  std::string parent_record_id = latest.parent_record_id;
  while (true) {
    const uint32_t expected_epoch =
        record_generation >= head.epoch_start_generation
            ? head.epoch
            : head.minimum_retained_epoch;
    if (record_epoch != expected_epoch) {
      return base::unexpected(SyncVaultStoreError::kMalformedObject);
    }
    ancestry.push_back({record_id, record_generation, parent_record_id});
    if (checkpoint_.generation == 0 ||
        record_generation == checkpoint_.generation + 1) {
      break;
    }
    if (record_generation <= checkpoint_.generation + 1 ||
        ancestry.size() >= kSyncVaultMaximumJournalAncestryRecords) {
      return base::unexpected(SyncVaultStoreError::kIncompleteHistory);
    }

    record_id = parent_record_id;
    JournalResult parent_result = ReadJournalRecord(record_id);
    if (!parent_result.has_value()) {
      return base::unexpected(parent_result.error());
    }
    SyncVaultJournalRecord parent = std::move(parent_result.value());
    if (parent.generation != ancestry.back().generation - 1 ||
        (parent.generation > 1 &&
         !IsObjectForRole(object_prefix_, parent.parent_record_id,
                          "journal/"))) {
      return base::unexpected(SyncVaultStoreError::kMalformedObject);
    }
    record_generation = parent.generation;
    record_epoch = parent.epoch;
    parent_record_id = std::move(parent.parent_record_id);
  }

  switch (ValidateSyncVaultHistory(checkpoint_, head, ancestry)) {
    case SyncVaultHistoryValidationResult::kAcceptDescendant:
      return latest;
    case SyncVaultHistoryValidationResult::kRollback:
      return base::unexpected(SyncVaultStoreError::kRollback);
    case SyncVaultHistoryValidationResult::kFork:
      return base::unexpected(SyncVaultStoreError::kFork);
    case SyncVaultHistoryValidationResult::kIncompleteHistory:
      return base::unexpected(SyncVaultStoreError::kIncompleteHistory);
    case SyncVaultHistoryValidationResult::kAcceptCurrent:
    case SyncVaultHistoryValidationResult::kAcceptCompacted:
    case SyncVaultHistoryValidationResult::kInvalidHistory:
      return base::unexpected(SyncVaultStoreError::kMalformedObject);
  }
  NOTREACHED();
}

base::expected<SyncVaultJournalRecord, SyncVaultStoreError>
SyncVaultStore::ReadJournalRecord(const std::string& record_id) const {
  ObjectResult result =
      ReadObject(record_id, kSyncVaultMaximumEncryptedObjectBytes);
  if (!result.has_value()) {
    return base::unexpected(result.error());
  }
  if (!result->has_value()) {
    return base::unexpected(SyncVaultStoreError::kIncompleteHistory);
  }
  return AuthenticateJournalObject(record_id, result->value());
}

SyncVaultStore::JournalResult SyncVaultStore::AuthenticateJournalObject(
    const std::string& record_id,
    const OpaqueSyncVaultObject& object) const {
  if (!HasValidContentAddress(record_id, object.data)) {
    return base::unexpected(SyncVaultStoreError::kAuthenticationFailed);
  }
  auto plaintext =
      cryptographer_->Decrypt(SyncVaultObjectRole::kJournal, object.data);
  if (!plaintext.has_value()) {
    return base::unexpected(MapCryptoError(plaintext.error()));
  }
  auto record = ParseSyncVaultJournalRecord(plaintext.value());
  if (!record.has_value()) {
    return base::unexpected(MapFormatError(record.error()));
  }
  const std::optional<uint32_t> object_epoch =
      GetObjectEpochForRole(object_prefix_, record_id, "journal/");
  if (!object_epoch || *object_epoch != record->epoch) {
    return base::unexpected(SyncVaultStoreError::kMalformedObject);
  }
  return std::move(record.value());
}

SyncVaultStore::WriteResult SyncVaultStore::InitializeVault() {
  SyncVaultDescriptor descriptor;
  descriptor.vault_uuid = cryptographer_->vault_uuid();
  descriptor.creation_time_millis =
      base::Time::Now().InMillisecondsSinceUnixEpoch();
  auto serialized_descriptor = SerializeSyncVaultDescriptor(descriptor);
  if (!serialized_descriptor.has_value()) {
    return base::unexpected(MapFormatError(serialized_descriptor.error()));
  }
  auto encrypted_descriptor = cryptographer_->Encrypt(
      SyncVaultObjectRole::kDescriptor, serialized_descriptor.value());
  if (!encrypted_descriptor.has_value()) {
    return base::unexpected(MapCryptoError(encrypted_descriptor.error()));
  }
  WriteResult descriptor_write =
      WriteObject(descriptor_object_id_, std::nullopt,
                  std::move(encrypted_descriptor.value()));
  if (!descriptor_write.has_value()) {
    ObjectResult existing =
        ReadObject(descriptor_object_id_, kMaximumMetadataContainerBytes);
    if (!existing.has_value() || !existing->has_value()) {
      return base::unexpected(descriptor_write.error());
    }
    auto validation = ValidateDescriptor(existing->value());
    if (!validation.has_value()) {
      return base::unexpected(validation.error());
    }
  }

  SyncVaultHead initial_head;
  auto serialized_head = SerializeSyncVaultHead(initial_head);
  if (!serialized_head.has_value()) {
    return base::unexpected(MapFormatError(serialized_head.error()));
  }
  auto encrypted_head = cryptographer_->Encrypt(SyncVaultObjectRole::kHead,
                                                serialized_head.value());
  if (!encrypted_head.has_value()) {
    return base::unexpected(MapCryptoError(encrypted_head.error()));
  }
  WriteResult head_write = WriteObject(head_object_id_, std::nullopt,
                                       std::move(encrypted_head.value()));
  if (head_write.has_value()) {
    return head_write;
  }
  HeadResult existing_head = ReadHead();
  if (!existing_head.has_value()) {
    return base::unexpected(head_write.error());
  }
  if (existing_head->first.generation != 0) {
    return base::unexpected(SyncVaultStoreError::kConflict);
  }
  return std::move(existing_head->second);
}

SyncVaultStore::WriteResult SyncVaultStore::WriteImmutableObject(
    const std::string& object_id,
    std::vector<uint8_t> encrypted_data) {
  const std::vector<uint8_t> expected_data = encrypted_data;
  WriteResult write =
      WriteObject(object_id, std::nullopt, std::move(encrypted_data));
  if (write.has_value()) {
    return write;
  }
  // A collision or missing acknowledgement is successful only when the
  // complete immutable ciphertext is already present byte-for-byte.
  ObjectResult existing =
      ReadObject(object_id, kSyncVaultMaximumEncryptedObjectBytes);
  if (existing.has_value() && existing->has_value() &&
      existing->value().data == expected_data) {
    return existing->value().revision;
  }
  return base::unexpected(write.error());
}

bool SyncVaultStore::AdvanceCheckpoint(const SyncVaultHead& head) {
  SyncVaultCheckpoint next{cryptographer_->vault_uuid(), head.generation,
                           head.record_id, head.epoch};
  if (!persist_checkpoint_callback_.Run(next)) {
    return false;
  }
  checkpoint_ = std::move(next);
  return true;
}

void SyncVaultStore::MaybeCollectEpochs(const SyncVaultHead& head) {
  if (head.minimum_retained_epoch == 0 ||
      (attempted_collection_floor_ &&
       *attempted_collection_floor_ >= head.minimum_retained_epoch)) {
    return;
  }
  attempted_collection_floor_ = head.minimum_retained_epoch;
  // Collection is an optimization. The authenticated head already commits to
  // the retention floor, and a failed provider cleanup must not fail syncing.
  transport_->CollectEpochsBefore(head.minimum_retained_epoch);
}

}  // namespace noventra_sync
