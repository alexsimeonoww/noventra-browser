// Copyright 2026 The Noventra Authors. All rights reserved.
#include "components/noventra_sync_vault/sync_vault_store.h"

#include <algorithm>
#include <array>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "base/check.h"
#include "base/strings/string_number_conversions.h"
#include "base/test/bind.h"
#include "base/test/task_environment.h"
#include "components/noventra_sync_vault/sync_vault_cryptographer.h"
#include "crypto/hash.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace noventra_sync {
namespace {
constexpr char kVault[] = "c65f6051-1089-4518-9eb1-8fb140a9bb59";
constexpr size_t kSliceBytes = 32 * 1024 * 1024;
std::string Prefix() {
  return std::string("helium/v1/") + kVault + "/";
}

struct MemoryObjects {
  std::map<std::string, OpaqueSyncVaultObject> objects;
  std::vector<std::string> reads;
  size_t read_bytes = 0;
  size_t writes = 0;
  size_t collections = 0;
  unsigned revision = 0;
  std::optional<SyncVaultTransportError> next_error;
  base::RepeatingCallback<void(const std::string&)> on_read;

  std::string Put(std::string id, std::vector<uint8_t> data) {
    std::string etag = "\"" + base::NumberToString(++revision) + "\"";
    objects.insert_or_assign(std::move(id),
                             OpaqueSyncVaultObject{std::move(data), etag});
    return etag;
  }
};

class MemoryTransport final : public SyncVaultTransport {
 public:
  explicit MemoryTransport(MemoryObjects& objects) : objects_(objects) {}
  void SetCancelationSignal(CancelationSignal*) override {}
  ReadResult ReadObject(const std::string& id, size_t maximum_bytes) override {
    objects_.reads.push_back(id);
    if (objects_.on_read) {
      objects_.on_read.Run(id);
    }
    if (objects_.next_error) {
      return base::unexpected(
          *std::exchange(objects_.next_error, std::nullopt));
    }
    const auto found = objects_.objects.find(id);
    if (found == objects_.objects.end()) {
      return std::nullopt;
    }
    if (found->second.data.size() > maximum_bytes) {
      return base::unexpected(SyncVaultTransportError::kInvalidData);
    }
    objects_.read_bytes += found->second.data.size();
    return found->second;
  }
  WriteResult WriteObject(const std::string& id,
                          std::optional<std::string> revision,
                          std::vector<uint8_t> data) override {
    ++objects_.writes;
    const auto found = objects_.objects.find(id);
    if (revision ? found == objects_.objects.end() ||
                       found->second.revision != *revision
                 : found != objects_.objects.end()) {
      return base::unexpected(SyncVaultTransportError::kConflict);
    }
    return objects_.Put(id, std::move(data));
  }
  void CollectEpochsBefore(uint32_t) override { ++objects_.collections; }

 private:
  MemoryObjects& objects_;
};

class SyncVaultStoreCursorTest : public testing::Test {
 protected:
  void SetUp() override {
    auto cryptographer = SyncVaultCryptographer::Create(key_, kVault);
    ASSERT_TRUE(cryptographer.has_value());
    cryptographer_ = std::move(*cryptographer);
    auto descriptor = SerializeSyncVaultDescriptor({kVault, 0});
    ASSERT_TRUE(descriptor.has_value());
    objects_.Put(Prefix() + "descriptor",
                 Encrypt(SyncVaultObjectRole::kDescriptor, *descriptor));
    chunks_.push_back(AddChunk({1, 2, 3, 4}));
  }

  std::vector<uint8_t> Encrypt(SyncVaultObjectRole role,
                               base::span<const uint8_t> plaintext) {
    auto encrypted = cryptographer_->Encrypt(role, plaintext);
    CHECK(encrypted.has_value());
    return std::move(*encrypted);
  }
  std::string PutImmutable(std::string role,
                           uint32_t epoch,
                           std::vector<uint8_t> encrypted) {
    auto id = Prefix() + "epoch/" + base::NumberToString(epoch) + "/" + role +
              "/" + base::HexEncode(crypto::hash::Sha256(encrypted));
    objects_.Put(id, std::move(encrypted));
    return id;
  }
  SyncVaultObjectReference AddChunk(std::vector<uint8_t> plaintext,
                                    uint32_t epoch = 0) {
    const auto size = static_cast<uint32_t>(plaintext.size());
    return {PutImmutable("entity", epoch,
                         Encrypt(SyncVaultObjectRole::kEntity, plaintext)),
            size};
  }
  std::string PutRecord(const SyncVaultJournalRecord& record) {
    auto serialized = SerializeSyncVaultJournalRecord(record);
    CHECK(serialized.has_value());
    return PutImmutable("journal", record.epoch,
                        Encrypt(SyncVaultObjectRole::kJournal, *serialized));
  }
  void Publish(SyncVaultHead head) {
    head_ = std::move(head);
    auto serialized = SerializeSyncVaultHead(head_);
    CHECK(serialized.has_value());
    objects_.Put(Prefix() + "head",
                 Encrypt(SyncVaultObjectRole::kHead, *serialized));
  }
  void Append(
      uint64_t last_generation,
      base::RepeatingCallback<void(SyncVaultJournalRecord&)> adjust = {}) {
    // Deliberately do not use the store writer's 256-generation rollover
    // policy: the existing authenticated reader permits larger valid epochs.
    for (uint64_t generation = journal_ids_.size();
         generation <= last_generation; ++generation) {
      SyncVaultJournalRecord record;
      record.generation = generation;
      record.mutation_count = 1;
      record.parent_record_id = journal_ids_.back();
      record.state_chunks = chunks_;
      if (adjust) {
        adjust.Run(record);
      }
      journal_ids_.push_back(PutRecord(record));
    }
    SyncVaultHead head;
    head.generation = last_generation;
    head.record_id = journal_ids_.back();
    head.epoch_start_generation = 1;
    Publish(std::move(head));
  }
  SyncVaultCheckpoint InitialCheckpoint() const {
    return {kVault, 1, journal_ids_.at(1), 0};
  }
  void CreateReader(std::optional<SyncVaultCheckpoint> checkpoint) {
    auto store = SyncVaultStore::Create(
        std::make_unique<MemoryTransport>(objects_), key_, kVault,
        std::move(checkpoint),
        base::BindLambdaForTesting([this](const SyncVaultCheckpoint& next) {
          ++checkpoint_attempts_;
          if (!allow_checkpoint_) {
            return false;
          }
          persisted_.push_back(next);
          return true;
        }));
    ASSERT_TRUE(store.has_value());
    store_ = std::move(*store);
  }
  SyncVaultStore::RefreshResult Slice(size_t maximum_objects = 512,
                                      size_t maximum_bytes = kSliceBytes) {
    const size_t reads_before = objects_.reads.size();
    const size_t bytes_before = objects_.read_bytes;
    auto result = store_->ReadStateIncrementally(
        {maximum_objects, maximum_bytes,
         base::TimeTicks::Now() + base::Seconds(90),
         base::BindLambdaForTesting([this] { return cancelled_; })});
    EXPECT_LE(objects_.reads.size() - reads_before, maximum_objects);
    EXPECT_LE(objects_.read_bytes - bytes_before, maximum_bytes);
    return result;
  }
  SyncVaultStore::ReadResult Finish(size_t maximum_objects = 512) {
    for (size_t slice = 0; slice < 2048; ++slice) {
      auto result = Slice(maximum_objects);
      if (!result.has_value()) {
        return base::unexpected(result.error());
      }
      if (result->has_value()) {
        return std::move(result->value());
      }
      EXPECT_TRUE(persisted_.empty());
      EXPECT_EQ(checkpoint_attempts_, 0u);
      EXPECT_EQ(store_->checkpoint(), InitialCheckpoint());
    }
    ADD_FAILURE() << "A bounded sequence of slices made no complete progress";
    return base::unexpected(SyncVaultStoreError::kTemporaryError);
  }
  size_t Reads(const std::string& id) const {
    return std::ranges::count(objects_.reads, id);
  }

  base::test::TaskEnvironment task_environment_{
      base::test::TaskEnvironment::TimeSource::MOCK_TIME};
  const std::array<uint8_t, SyncVaultCryptographer::kVaultKeySize> key_{};
  MemoryObjects objects_;
  std::unique_ptr<SyncVaultCryptographer> cryptographer_;
  std::vector<std::string> journal_ids_{std::string()};
  std::vector<SyncVaultObjectReference> chunks_;
  SyncVaultHead head_;
  bool cancelled_ = false;
  bool allow_checkpoint_ = true;
  size_t checkpoint_attempts_ = 0;
  std::vector<SyncVaultCheckpoint> persisted_;
  std::unique_ptr<SyncVaultStore> store_;
};

TEST_F(SyncVaultStoreCursorTest,
       MoreThan512ReadsProgressWithoutEarlyCheckpoint) {
  Append(700);
  CreateReader(InitialCheckpoint());
  auto first = Slice();
  ASSERT_TRUE(first.has_value());
  EXPECT_FALSE(first->has_value());
  EXPECT_EQ(objects_.reads.size(), 512u);
  EXPECT_EQ(store_->checkpoint(), InitialCheckpoint());
  EXPECT_TRUE(persisted_.empty());
  EXPECT_EQ(checkpoint_attempts_, 0u);

  auto result = Finish();
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(result->head, head_);
  EXPECT_EQ(result->data, (std::vector<uint8_t>{1, 2, 3, 4}));
  ASSERT_EQ(persisted_.size(), 1u);
  EXPECT_EQ(persisted_.back().generation, 700u);
  EXPECT_EQ(objects_.reads.size(),
            703u);  // Descriptor, 2 heads, 699 links, data.
  EXPECT_EQ(Reads(Prefix() + "descriptor"), 1u);
  for (size_t generation = 2; generation <= 700; ++generation) {
    EXPECT_EQ(Reads(journal_ids_[generation]), 1u);
  }
  EXPECT_EQ(objects_.collections, 0u);
}

TEST_F(SyncVaultStoreCursorTest, FixtureIsAlsoAcceptedByTheExistingReader) {
  Append(700);
  CreateReader(InitialCheckpoint());
  auto result = store_->ReadState();
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(result->head, head_);
  EXPECT_EQ(result->data, (std::vector<uint8_t>{1, 2, 3, 4}));
  EXPECT_GT(objects_.reads.size(), 512u);
}

TEST_F(SyncVaultStoreCursorTest, ChunkBytesAlsoProgressAcrossBoundedSlices) {
  chunks_.clear();
  for (uint8_t byte = 1; byte <= 3; ++byte) {
    chunks_.push_back(AddChunk(std::vector<uint8_t>(14 * 1024 * 1024, byte)));
  }
  Append(2);
  CreateReader(InitialCheckpoint());
  auto first = Slice();
  ASSERT_TRUE(first.has_value());
  EXPECT_FALSE(first->has_value());
  EXPECT_TRUE(persisted_.empty());
  auto result = Finish();
  ASSERT_TRUE(result.has_value());
  ASSERT_EQ(result->data.size(), 42u * 1024 * 1024);
  EXPECT_EQ(result->data.front(), 1);
  EXPECT_EQ(result->data.back(), 3);
  EXPECT_GT(objects_.read_bytes, kSliceBytes);
  for (const auto& chunk : chunks_) {
    EXPECT_EQ(Reads(chunk.object_id), 1u);
  }
}

TEST_F(SyncVaultStoreCursorTest,
       MovingHeadIsReconciledWithoutPublishingOldState) {
  Append(650);
  CreateReader(InitialCheckpoint());
  auto first = Slice();
  ASSERT_TRUE(first.has_value());
  ASSERT_FALSE(first->has_value());
  chunks_ = {AddChunk({9, 8, 7})};
  Append(655);
  // One-object slices exercise every transition, including the fully verified
  // old candidate becoming an internal anchor without a durable checkpoint.
  auto result = Finish(1);
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(result->head, head_);
  EXPECT_EQ(result->data, (std::vector<uint8_t>{9, 8, 7}));
  EXPECT_EQ(result->head_revision,
            objects_.objects.at(Prefix() + "head").revision);
  ASSERT_EQ(persisted_.size(), 1u);
  EXPECT_EQ(persisted_.back().generation, 655u);
  for (size_t generation = 2; generation <= 655; ++generation) {
    EXPECT_EQ(Reads(journal_ids_[generation]), 1u);
  }

  Append(656);
  auto commit = store_->CommitState(*result, base::span(result->data), 1);
  ASSERT_FALSE(commit.has_value());
  EXPECT_EQ(commit.error(), SyncVaultStoreError::kConflict);
  EXPECT_EQ(persisted_.size(), 1u);
}

TEST_F(SyncVaultStoreCursorTest, ChangedHeadRollbackDoesNotAdvanceCheckpoint) {
  Append(650);
  CreateReader(InitialCheckpoint());
  auto first = Slice();
  ASSERT_TRUE(first.has_value());
  ASSERT_FALSE(first->has_value());
  auto rollback = head_;
  rollback.generation = 640;
  rollback.record_id = journal_ids_[640];
  Publish(std::move(rollback));
  auto result = Finish();
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error(), SyncVaultStoreError::kRollback);
  EXPECT_EQ(store_->checkpoint(), InitialCheckpoint());
  EXPECT_TRUE(persisted_.empty());
}

TEST_F(SyncVaultStoreCursorTest, AuthenticatedWrongParentAtAnchorIsFork) {
  Append(650, base::BindLambdaForTesting([](SyncVaultJournalRecord& record) {
           if (record.generation == 2) {
             record.parent_record_id =
                 Prefix() + "epoch/0/journal/" + std::string(64, '0');
           }
         }));
  CreateReader(InitialCheckpoint());
  auto first = Slice();
  ASSERT_TRUE(first.has_value());
  ASSERT_FALSE(first->has_value());
  auto result = Finish();
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error(), SyncVaultStoreError::kFork);
  EXPECT_EQ(store_->checkpoint(), InitialCheckpoint());
  EXPECT_EQ(checkpoint_attempts_, 0u);
}

TEST_F(SyncVaultStoreCursorTest, ParentGenerationAndEpochChecksSurviveSlicing) {
  for (bool wrong_epoch : {false, true}) {
    SCOPED_TRACE(wrong_epoch);
    journal_ids_ = {std::string()};
    Append(10, base::BindLambdaForTesting([&](SyncVaultJournalRecord& record) {
             if (record.generation == 8) {
               if (wrong_epoch) {
                 record.epoch = 1;
               } else {
                 record.parent_record_id = journal_ids_[6];
               }
             }
           }));
    CreateReader(InitialCheckpoint());
    auto result = Finish(1);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), SyncVaultStoreError::kMalformedObject);
    EXPECT_EQ(checkpoint_attempts_, 0u);
  }
}

TEST_F(SyncVaultStoreCursorTest, CorruptUnreadJournalRejectsWithoutCheckpoint) {
  Append(650);
  CreateReader(InitialCheckpoint());
  auto first = Slice();
  ASSERT_TRUE(first.has_value());
  ASSERT_FALSE(first->has_value());
  objects_.objects.at(journal_ids_[50]).data.back() ^= 1;
  auto result = Finish();
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error(), SyncVaultStoreError::kAuthenticationFailed);
  EXPECT_EQ(checkpoint_attempts_, 0u);
}

TEST_F(SyncVaultStoreCursorTest, WrongKeyOnFreshHeadCannotPublishCandidate) {
  Append(650);
  CreateReader(InitialCheckpoint());
  auto first = Slice();
  ASSERT_TRUE(first.has_value());
  ASSERT_FALSE(first->has_value());
  auto other_key = key_;
  other_key[0] = 42;
  auto wrong_crypto = SyncVaultCryptographer::Create(other_key, kVault);
  ASSERT_TRUE(wrong_crypto.has_value());
  auto serialized = SerializeSyncVaultHead(head_);
  ASSERT_TRUE(serialized.has_value());
  auto encrypted =
      (*wrong_crypto)->Encrypt(SyncVaultObjectRole::kHead, *serialized);
  ASSERT_TRUE(encrypted.has_value());
  objects_.Put(Prefix() + "head", std::move(*encrypted));
  auto result = Finish();
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error(), SyncVaultStoreError::kAuthenticationFailed);
  EXPECT_EQ(checkpoint_attempts_, 0u);
}

TEST_F(SyncVaultStoreCursorTest, CancellationAndAbortDiscardCursor) {
  Append(650);
  CreateReader(InitialCheckpoint());
  auto first = Slice();
  ASSERT_TRUE(first.has_value());
  ASSERT_FALSE(first->has_value());
  const size_t previous_reads = objects_.reads.size();
  cancelled_ = true;
  auto canceled = Slice();
  ASSERT_FALSE(canceled.has_value());
  EXPECT_EQ(canceled.error(), SyncVaultStoreError::kAborted);
  EXPECT_EQ(objects_.reads.size(), previous_reads);
  EXPECT_EQ(checkpoint_attempts_, 0u);
  cancelled_ = false;
  auto restarted = Slice(2);
  ASSERT_TRUE(restarted.has_value());
  ASSERT_FALSE(restarted->has_value());
  EXPECT_EQ(Reads(Prefix() + "descriptor"), 2u);
  store_->AbortRead();
  restarted = Slice(2);
  ASSERT_TRUE(restarted.has_value());
  ASSERT_FALSE(restarted->has_value());
  EXPECT_EQ(Reads(Prefix() + "descriptor"), 3u);
  EXPECT_EQ(store_->checkpoint(), InitialCheckpoint());
}

TEST_F(SyncVaultStoreCursorTest, CancellationAtFreshHeadPreventsCheckpoint) {
  Append(10);
  CreateReader(InitialCheckpoint());
  objects_.on_read = base::BindLambdaForTesting([&](const std::string& id) {
    if (id == Prefix() + "head" && Reads(id) == 2) {
      cancelled_ = true;
    }
  });
  auto result = Finish();
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error(), SyncVaultStoreError::kAborted);
  EXPECT_EQ(checkpoint_attempts_, 0u);
}

TEST_F(SyncVaultStoreCursorTest, DeadlineDuringRequestRetainsVerifiedProgress) {
  Append(650);
  CreateReader(InitialCheckpoint());
  auto first = Slice(100);
  ASSERT_TRUE(first.has_value());
  ASSERT_FALSE(first->has_value());
  objects_.on_read = base::BindLambdaForTesting([&](const std::string&) {
    task_environment_.AdvanceClock(base::Seconds(91));
    objects_.next_error = SyncVaultTransportError::kTemporaryError;
  });
  auto exhausted = Slice();
  ASSERT_TRUE(exhausted.has_value());
  EXPECT_FALSE(exhausted->has_value());
  EXPECT_EQ(checkpoint_attempts_, 0u);
  objects_.on_read.Reset();
  auto result = Finish();
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(Reads(Prefix() + "descriptor"), 1u);
  EXPECT_EQ(Reads(journal_ids_[650]), 1u);
  EXPECT_EQ(Reads(journal_ids_[552]), 2u);  // One expired attempt, then retry.
}

TEST_F(SyncVaultStoreCursorTest, FailedDurableCheckpointIsNotAccepted) {
  Append(10);
  CreateReader(InitialCheckpoint());
  allow_checkpoint_ = false;
  auto result = Finish(2);
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error(), SyncVaultStoreError::kCheckpointPersistenceFailed);
  EXPECT_EQ(store_->checkpoint(), InitialCheckpoint());
  EXPECT_TRUE(persisted_.empty());
  EXPECT_EQ(checkpoint_attempts_, 1u);
}

TEST_F(SyncVaultStoreCursorTest, OriginalAncestryLimitStillRejectsOversizeGap) {
  Append(1);
  CreateReader(InitialCheckpoint());
  SyncVaultJournalRecord record;
  record.generation = kSyncVaultMaximumJournalAncestryRecords + 2;
  record.mutation_count = 1;
  record.parent_record_id = journal_ids_[1];
  record.state_chunks = chunks_;
  auto head = head_;
  head.generation = record.generation;
  head.record_id = PutRecord(record);
  Publish(std::move(head));
  auto result = Slice();
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error(), SyncVaultStoreError::kObjectTooLarge);
  EXPECT_EQ(checkpoint_attempts_, 0u);
  EXPECT_EQ(objects_.reads.size(), 2u);
}

TEST_F(SyncVaultStoreCursorTest, MissingJoinedVaultNeverWritesOrInitializes) {
  objects_.objects.clear();
  CreateReader(std::nullopt);
  auto result = Slice();
  ASSERT_TRUE(result.has_value());
  ASSERT_TRUE(result->has_value());
  EXPECT_FALSE(result->value().initialized);
  EXPECT_EQ(objects_.writes, 0u);
  EXPECT_EQ(checkpoint_attempts_, 0u);
}
}  // namespace
}  // namespace noventra_sync
