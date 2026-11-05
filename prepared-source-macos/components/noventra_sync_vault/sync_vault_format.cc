// Copyright 2026 The Helium Authors
// You can use, redistribute, and/or modify this source code under
// the terms of the GPL-3.0 license that can be found in the LICENSE file.

#include "components/noventra_sync_vault/sync_vault_format.h"

#include <algorithm>
#include <cstdint>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "base/containers/span.h"
#include "base/numerics/checked_math.h"
#include "base/numerics/safe_conversions.h"
#include "base/uuid.h"
#include "components/noventra_sync_vault/sync_vault.pb.h"

namespace noventra_sync {

namespace {

bool IsValidVaultUuid(const std::string& vault_uuid) {
  return base::Uuid::ParseLowercase(vault_uuid).is_valid();
}

bool IsValidObjectId(const std::string& object_id) {
  return !object_id.empty() &&
         object_id.size() <= kSyncVaultMaximumIdentifierBytes;
}

bool IsValidHeadIdentity(uint64_t generation, const std::string& record_id) {
  if (generation == 0) {
    return record_id.empty();
  }
  return IsValidObjectId(record_id);
}

bool IsValidDescriptor(const SyncVaultDescriptor& descriptor) {
  return IsValidVaultUuid(descriptor.vault_uuid) &&
         descriptor.creation_time_millis >= 0;
}

bool IsValidHead(const SyncVaultHead& head) {
  if (!IsValidHeadIdentity(head.generation, head.record_id) ||
      head.minimum_retained_epoch > head.epoch ||
      head.minimum_retained_generation > head.epoch_start_generation) {
    return false;
  }
  if (head.generation == 0) {
    return head.epoch == 0 && head.epoch_start_generation == 0 &&
           head.minimum_retained_epoch == 0 &&
           head.minimum_retained_generation == 0 &&
           head.epoch_uploaded_bytes == 0;
  }
  if (head.epoch_start_generation == 0 ||
      head.epoch_start_generation > head.generation ||
      head.epoch >= head.generation) {
    return false;
  }
  const uint32_t expected_retained_epoch = head.epoch == 0 ? 0 : head.epoch - 1;
  return head.minimum_retained_epoch == expected_retained_epoch &&
         (head.epoch <= 1 ? head.minimum_retained_generation == 0
                          : head.minimum_retained_generation > 0 &&
                                head.minimum_retained_generation <
                                    head.epoch_start_generation);
}

bool IsValidCheckpoint(const SyncVaultCheckpoint& checkpoint) {
  return IsValidVaultUuid(checkpoint.vault_uuid) &&
         IsValidHeadIdentity(checkpoint.generation, checkpoint.record_id) &&
         (checkpoint.generation == 0
              ? checkpoint.epoch == 0
              : checkpoint.epoch < checkpoint.generation);
}

bool IsValidJournalRecord(const SyncVaultJournalRecord& record) {
  if (record.generation == 0 || record.creation_time_millis < 0 ||
      record.mutation_count == 0 || record.state_chunks.empty() ||
      record.state_chunks.size() > kSyncVaultMaximumStateChunks ||
      record.epoch >= record.generation) {
    return false;
  }
  if (record.generation == 1) {
    if (!record.parent_record_id.empty()) {
      return false;
    }
  } else if (!IsValidObjectId(record.parent_record_id)) {
    return false;
  }

  base::CheckedNumeric<size_t> reconstructed_bytes = 0;
  std::set<std::string> object_ids;
  for (const SyncVaultObjectReference& chunk : record.state_chunks) {
    if (!IsValidObjectId(chunk.object_id) || chunk.plaintext_size == 0 ||
        chunk.plaintext_size > kSyncVaultMaximumEntityPlaintextBytes ||
        !object_ids.insert(chunk.object_id).second) {
      return false;
    }
    reconstructed_bytes += chunk.plaintext_size;
  }
  return reconstructed_bytes.IsValid() &&
         reconstructed_bytes.ValueOrDie() <=
             kSyncVaultMaximumReconstructedBytes;
}

template <typename Message>
SyncVaultSerializeResult SerializeProto(const Message& message,
                                        size_t maximum_bytes) {
  const size_t size = message.ByteSizeLong();
  if (size > maximum_bytes) {
    return base::unexpected(SyncVaultFormatError::kLimitExceeded);
  }
  std::vector<uint8_t> serialized(size);
  if (!message.SerializeToArray(serialized.data(),
                                base::checked_cast<int>(size))) {
    return base::unexpected(SyncVaultFormatError::kInvalidValue);
  }
  return serialized;
}

template <typename Message>
base::expected<void, SyncVaultFormatError> ParseProto(
    base::span<const uint8_t> serialized,
    size_t maximum_bytes,
    Message* message) {
  if (serialized.size() > maximum_bytes) {
    return base::unexpected(SyncVaultFormatError::kLimitExceeded);
  }
  if (!message->ParseFromArray(serialized.data(),
                               base::checked_cast<int>(serialized.size()))) {
    return base::unexpected(SyncVaultFormatError::kMalformed);
  }
  return base::ok();
}

SyncVaultFormatError VersionError(uint32_t version) {
  return version == kSyncVaultFormatVersion
             ? SyncVaultFormatError::kMalformed
             : SyncVaultFormatError::kUnsupportedVersion;
}

}  // namespace

SyncVaultSerializeResult SerializeSyncVaultDescriptor(
    const SyncVaultDescriptor& descriptor) {
  if (!IsValidDescriptor(descriptor)) {
    return base::unexpected(SyncVaultFormatError::kInvalidValue);
  }
  noventra_sync_pb::SyncVaultDescriptor proto;
  proto.set_format_version(kSyncVaultFormatVersion);
  proto.set_vault_uuid(descriptor.vault_uuid);
  proto.set_creation_time_millis(descriptor.creation_time_millis);
  return SerializeProto(proto, kSyncVaultMaximumMetadataPlaintextBytes);
}

SyncVaultParseResult<SyncVaultDescriptor> ParseSyncVaultDescriptor(
    base::span<const uint8_t> serialized) {
  noventra_sync_pb::SyncVaultDescriptor proto;
  auto parsed =
      ParseProto(serialized, kSyncVaultMaximumMetadataPlaintextBytes, &proto);
  if (!parsed.has_value()) {
    return base::unexpected(parsed.error());
  }
  if (!proto.has_format_version() || !proto.has_vault_uuid() ||
      !proto.has_creation_time_millis()) {
    return base::unexpected(SyncVaultFormatError::kMalformed);
  }
  if (proto.format_version() != kSyncVaultFormatVersion) {
    return base::unexpected(SyncVaultFormatError::kUnsupportedVersion);
  }
  SyncVaultDescriptor descriptor;
  descriptor.vault_uuid = proto.vault_uuid();
  descriptor.creation_time_millis = proto.creation_time_millis();
  if (!IsValidDescriptor(descriptor)) {
    return base::unexpected(SyncVaultFormatError::kInvalidValue);
  }
  return descriptor;
}

SyncVaultSerializeResult SerializeSyncVaultHead(const SyncVaultHead& head) {
  if (!IsValidHead(head)) {
    return base::unexpected(SyncVaultFormatError::kInvalidValue);
  }
  noventra_sync_pb::SyncVaultHead proto;
  proto.set_format_version(kSyncVaultFormatVersion);
  proto.set_generation(head.generation);
  proto.set_record_id(head.record_id);
  proto.set_epoch(head.epoch);
  proto.set_epoch_start_generation(head.epoch_start_generation);
  proto.set_minimum_retained_epoch(head.minimum_retained_epoch);
  proto.set_minimum_retained_generation(head.minimum_retained_generation);
  proto.set_epoch_uploaded_bytes(head.epoch_uploaded_bytes);
  return SerializeProto(proto, kSyncVaultMaximumMetadataPlaintextBytes);
}

SyncVaultParseResult<SyncVaultHead> ParseSyncVaultHead(
    base::span<const uint8_t> serialized) {
  noventra_sync_pb::SyncVaultHead proto;
  auto parsed =
      ParseProto(serialized, kSyncVaultMaximumMetadataPlaintextBytes, &proto);
  if (!parsed.has_value()) {
    return base::unexpected(parsed.error());
  }
  if (!proto.has_format_version() || !proto.has_generation() ||
      !proto.has_record_id() || !proto.has_epoch() ||
      !proto.has_epoch_start_generation() ||
      !proto.has_minimum_retained_epoch() ||
      !proto.has_minimum_retained_generation() ||
      !proto.has_epoch_uploaded_bytes()) {
    return base::unexpected(SyncVaultFormatError::kMalformed);
  }
  if (proto.format_version() != kSyncVaultFormatVersion) {
    return base::unexpected(VersionError(proto.format_version()));
  }
  SyncVaultHead head;
  head.generation = proto.generation();
  head.record_id = proto.record_id();
  head.epoch = proto.epoch();
  head.epoch_start_generation = proto.epoch_start_generation();
  head.minimum_retained_epoch = proto.minimum_retained_epoch();
  head.minimum_retained_generation = proto.minimum_retained_generation();
  head.epoch_uploaded_bytes = proto.epoch_uploaded_bytes();
  if (!IsValidHead(head)) {
    return base::unexpected(SyncVaultFormatError::kInvalidValue);
  }
  return head;
}

SyncVaultSerializeResult SerializeSyncVaultJournalRecord(
    const SyncVaultJournalRecord& record) {
  if (!IsValidJournalRecord(record)) {
    return base::unexpected(SyncVaultFormatError::kInvalidValue);
  }
  noventra_sync_pb::SyncVaultJournalRecord proto;
  proto.set_format_version(kSyncVaultFormatVersion);
  proto.set_generation(record.generation);
  proto.set_creation_time_millis(record.creation_time_millis);
  proto.set_mutation_count(record.mutation_count);
  proto.set_parent_record_id(record.parent_record_id);
  proto.set_epoch(record.epoch);
  for (const SyncVaultObjectReference& chunk : record.state_chunks) {
    noventra_sync_pb::SyncVaultObjectReference* chunk_proto = proto.add_state_chunks();
    chunk_proto->set_object_id(chunk.object_id);
    chunk_proto->set_plaintext_size(chunk.plaintext_size);
  }
  return SerializeProto(proto, kSyncVaultMaximumJournalPlaintextBytes);
}

SyncVaultParseResult<SyncVaultJournalRecord> ParseSyncVaultJournalRecord(
    base::span<const uint8_t> serialized) {
  noventra_sync_pb::SyncVaultJournalRecord proto;
  auto parsed =
      ParseProto(serialized, kSyncVaultMaximumJournalPlaintextBytes, &proto);
  if (!parsed.has_value()) {
    return base::unexpected(parsed.error());
  }
  if (!proto.has_format_version() || !proto.has_generation() ||
      !proto.has_creation_time_millis() || !proto.has_mutation_count() ||
      !proto.has_parent_record_id() || !proto.has_epoch()) {
    return base::unexpected(SyncVaultFormatError::kMalformed);
  }
  if (proto.format_version() != kSyncVaultFormatVersion) {
    return base::unexpected(VersionError(proto.format_version()));
  }
  if (proto.state_chunks_size() <= 0 ||
      base::checked_cast<size_t>(proto.state_chunks_size()) >
          kSyncVaultMaximumStateChunks) {
    return base::unexpected(SyncVaultFormatError::kInvalidValue);
  }
  SyncVaultJournalRecord record;
  record.generation = proto.generation();
  record.creation_time_millis = proto.creation_time_millis();
  record.mutation_count = proto.mutation_count();
  record.parent_record_id = proto.parent_record_id();
  record.epoch = proto.epoch();
  record.state_chunks.reserve(proto.state_chunks_size());
  for (const noventra_sync_pb::SyncVaultObjectReference& chunk_proto :
       proto.state_chunks()) {
    if (!chunk_proto.has_object_id() || !chunk_proto.has_plaintext_size()) {
      return base::unexpected(SyncVaultFormatError::kMalformed);
    }
    SyncVaultObjectReference chunk;
    chunk.object_id = chunk_proto.object_id();
    chunk.plaintext_size = chunk_proto.plaintext_size();
    record.state_chunks.push_back(std::move(chunk));
  }
  if (!IsValidJournalRecord(record)) {
    return base::unexpected(SyncVaultFormatError::kInvalidValue);
  }
  return record;
}

SyncVaultSerializeResult SerializeSyncVaultCheckpoint(
    const SyncVaultCheckpoint& checkpoint) {
  if (!IsValidCheckpoint(checkpoint)) {
    return base::unexpected(SyncVaultFormatError::kInvalidValue);
  }
  noventra_sync_pb::SyncVaultCheckpoint proto;
  proto.set_format_version(kSyncVaultFormatVersion);
  proto.set_vault_uuid(checkpoint.vault_uuid);
  proto.set_generation(checkpoint.generation);
  proto.set_record_id(checkpoint.record_id);
  proto.set_epoch(checkpoint.epoch);
  return SerializeProto(proto, kSyncVaultMaximumMetadataPlaintextBytes);
}

SyncVaultParseResult<SyncVaultCheckpoint> ParseSyncVaultCheckpoint(
    base::span<const uint8_t> serialized) {
  noventra_sync_pb::SyncVaultCheckpoint proto;
  auto parsed =
      ParseProto(serialized, kSyncVaultMaximumMetadataPlaintextBytes, &proto);
  if (!parsed.has_value()) {
    return base::unexpected(parsed.error());
  }
  if (!proto.has_format_version() || !proto.has_vault_uuid() ||
      !proto.has_generation() || !proto.has_record_id() || !proto.has_epoch()) {
    return base::unexpected(SyncVaultFormatError::kMalformed);
  }
  if (proto.format_version() != kSyncVaultFormatVersion) {
    return base::unexpected(VersionError(proto.format_version()));
  }
  SyncVaultCheckpoint checkpoint;
  checkpoint.vault_uuid = proto.vault_uuid();
  checkpoint.generation = proto.generation();
  checkpoint.record_id = proto.record_id();
  checkpoint.epoch = proto.epoch();
  if (!IsValidCheckpoint(checkpoint)) {
    return base::unexpected(SyncVaultFormatError::kInvalidValue);
  }
  return checkpoint;
}

SyncVaultHistoryValidationResult ValidateSyncVaultHistory(
    const SyncVaultCheckpoint& checkpoint,
    const SyncVaultHead& candidate_head,
    base::span<const AuthenticatedSyncVaultJournalRecord>
        newest_first_ancestry) {
  if (!IsValidCheckpoint(checkpoint) || !IsValidHead(candidate_head) ||
      newest_first_ancestry.size() > kSyncVaultMaximumJournalAncestryRecords) {
    return SyncVaultHistoryValidationResult::kInvalidHistory;
  }
  if (candidate_head.generation < checkpoint.generation) {
    return SyncVaultHistoryValidationResult::kRollback;
  }
  if (candidate_head.generation == checkpoint.generation) {
    if (!newest_first_ancestry.empty()) {
      return SyncVaultHistoryValidationResult::kInvalidHistory;
    }
    return candidate_head.record_id == checkpoint.record_id
               ? SyncVaultHistoryValidationResult::kAcceptCurrent
               : SyncVaultHistoryValidationResult::kFork;
  }

  if (checkpoint.generation < candidate_head.minimum_retained_generation) {
    return newest_first_ancestry.empty()
               ? SyncVaultHistoryValidationResult::kAcceptCompacted
               : SyncVaultHistoryValidationResult::kInvalidHistory;
  }

  uint64_t expected_generation = candidate_head.generation;
  std::string expected_record_id = candidate_head.record_id;
  for (size_t index = 0; index < newest_first_ancestry.size(); ++index) {
    const AuthenticatedSyncVaultJournalRecord& authenticated_record =
        newest_first_ancestry[index];
    if (authenticated_record.generation != expected_generation ||
        authenticated_record.record_id != expected_record_id) {
      return SyncVaultHistoryValidationResult::kInvalidHistory;
    }

    if (checkpoint.generation == 0) {
      return index + 1 == newest_first_ancestry.size()
                 ? SyncVaultHistoryValidationResult::kAcceptDescendant
                 : SyncVaultHistoryValidationResult::kInvalidHistory;
    }

    if (expected_generation == checkpoint.generation + 1) {
      if (index + 1 != newest_first_ancestry.size()) {
        return SyncVaultHistoryValidationResult::kInvalidHistory;
      }
      return authenticated_record.parent_record_id == checkpoint.record_id
                 ? SyncVaultHistoryValidationResult::kAcceptDescendant
                 : SyncVaultHistoryValidationResult::kFork;
    }
    expected_generation--;
    expected_record_id = authenticated_record.parent_record_id;
  }
  return SyncVaultHistoryValidationResult::kIncompleteHistory;
}

}  // namespace noventra_sync
