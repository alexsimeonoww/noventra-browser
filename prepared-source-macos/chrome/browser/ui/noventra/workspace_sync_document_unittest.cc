// Copyright 2026 The Noventra Authors. All rights reserved.
#include "chrome/browser/ui/noventra/workspace_sync_document.h"

#include <utility>

#include "base/strings/string_number_conversions.h"
#include "chrome/browser/ui/noventra/workspace_sync_service.h"
#include "components/sync/engine/cancelation_signal.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace noventra {
class WorkspaceSyncServiceTestPeer {
 public:
  static void Cancel(syncer::CancelationSignal& signal) {
    WorkspaceSyncService::SignalCancelationOnce(signal);
  }
};

namespace {
constexpr char kLogical[] = "c65f6051-1089-4518-9eb1-8fb140a9bb59";
constexpr char kOtherLogical[] = "2ab41560-f328-482b-a360-7876c93ad84a";
constexpr char kLocal[] = "ff3c3769-053b-4d1f-a193-a908ddc98809";

std::vector<LayoutCapability> Capabilities() {
  return {{"tabs", "Tabs", {DockEdge::kTop, DockEdge::kLeft}, false},
          {"spaces", "Spaces", {DockEdge::kLeft, DockEdge::kRight}, true}};
}
WorkspaceSyncDocument Document() {
  WorkspaceSyncDocument document;
  document.spaces = {{kWorkspaceSyncPersonalId, "Personal", false},
                     {kLogical, "Work", false}};
  document.layout.items = {{"tabs", DockEdge::kTop, 0, true},
                           {"spaces", DockEdge::kLeft, 0, true}};
  return document;
}

std::string CapacityUuid(size_t index) {
  const auto suffix = base::NumberToString(index);
  return "00000000-0000-4000-8000-" + std::string(12 - suffix.size(), '0') +
         suffix;
}

std::set<std::string> ExcludedSpaces(size_t count) {
  std::set<std::string> excluded;
  for (size_t i = 1; i <= count; ++i) {
    excluded.insert(CapacityUuid(i));
  }
  return excluded;
}

std::vector<WorkspaceSyncSpace> ObservedSpaces(
    const std::set<std::string>& excluded) {
  std::vector<WorkspaceSyncSpace> observed{{"", "Personal", false}};
  for (const auto& id : excluded) {
    observed.push_back({id, "Local account", false});
  }
  return observed;
}

TEST(WorkspaceSyncDocumentTest,
     RoundTripKeepsOrderAndNeverSharesLocalRevision) {
  auto original = Document();
  original.layout.revision = 487;
  original.spaces[1].archived = true;
  std::swap(original.spaces[0], original.spaces[1]);
  const auto serialized = SerializeWorkspaceSyncDocument(original);
  auto parsed = ParseWorkspaceSyncDocument(serialized, Capabilities());
  ASSERT_TRUE(parsed.has_value());
  EXPECT_EQ(parsed->layout.revision, 0);
  EXPECT_EQ(parsed->spaces, original.spaces);
  EXPECT_TRUE(WorkspaceSyncDocumentsEqual(*parsed, original));
}

TEST(WorkspaceSyncDocumentTest, PersonalIsRequiredAndCannotBeArchived) {
  auto document = Document();
  document.spaces[0].archived = true;
  EXPECT_FALSE(ParseWorkspaceSyncDocument(
                   SerializeWorkspaceSyncDocument(document), Capabilities())
                   .has_value());
  document.spaces.erase(document.spaces.begin());
  EXPECT_FALSE(ParseWorkspaceSyncDocument(
                   SerializeWorkspaceSyncDocument(document), Capabilities())
                   .has_value());
}

TEST(WorkspaceSyncDocumentTest, RejectsDuplicateIdentityAndMalformedNames) {
  auto document = Document();
  document.spaces.push_back(document.spaces.back());
  EXPECT_FALSE(ParseWorkspaceSyncDocument(
                   SerializeWorkspaceSyncDocument(document), Capabilities())
                   .has_value());
  for (const auto& name : {"", " leading", "trailing ", "line\nbreak"}) {
    document = Document();
    document.spaces[1].name = name;
    EXPECT_FALSE(ParseWorkspaceSyncDocument(
                     SerializeWorkspaceSyncDocument(document), Capabilities())
                     .has_value());
  }
  document = Document();
  document.spaces[1].name = std::string(161, 'a');
  EXPECT_FALSE(ParseWorkspaceSyncDocument(
                   SerializeWorkspaceSyncDocument(document), Capabilities())
                   .has_value());
}

TEST(WorkspaceSyncDocumentTest, RejectsDuplicateJsonKeysAndNonScopeFields) {
  auto bytes = SerializeWorkspaceSyncDocument(Document());
  bytes.insert(1, "\"version\":1,");
  EXPECT_FALSE(ParseWorkspaceSyncDocument(bytes, Capabilities()).has_value());
  auto value = WorkspaceSyncDocumentToValue(Document());
  value.Set("cookies", base::ListValue());
  EXPECT_FALSE(
      ValidateWorkspaceSyncDocument(value, Capabilities()).has_value());
}

TEST(WorkspaceSyncDocumentTest, ValidatesCompleteLayoutAndManagedVisibility) {
  auto document = Document();
  document.layout.items[0].visible = false;
  EXPECT_FALSE(ParseWorkspaceSyncDocument(
                   SerializeWorkspaceSyncDocument(document), Capabilities())
                   .has_value());
  document = Document();
  document.layout.items.pop_back();
  EXPECT_FALSE(ParseWorkspaceSyncDocument(
                   SerializeWorkspaceSyncDocument(document), Capabilities())
                   .has_value());
  auto capabilities = Capabilities();
  capabilities[1].managed = true;
  EXPECT_FALSE(ParseWorkspaceSyncDocument(
                   SerializeWorkspaceSyncDocument(Document()), capabilities)
                   .has_value());
  capabilities = Capabilities();
  capabilities[1].required_visibility = false;
  EXPECT_FALSE(ParseWorkspaceSyncDocument(
                   SerializeWorkspaceSyncDocument(Document()), capabilities)
                   .has_value());
}

TEST(WorkspaceSyncDocumentTest, BoundedDocumentAndCanonicalUuids) {
  EXPECT_FALSE(ParseWorkspaceSyncDocument(
                   std::string(kWorkspaceSyncMaximumDocumentBytes + 1, ' '),
                   Capabilities())
                   .has_value());
  EXPECT_TRUE(IsWorkspaceSyncUuid(kLogical));
  EXPECT_FALSE(IsWorkspaceSyncUuid("C65F6051-1089-4518-9EB1-8FB140A9BB59"));
  EXPECT_FALSE(IsWorkspaceSyncUuid("personal"));
}

TEST(WorkspaceSyncDocumentTest, MappingIsBijectiveAndPersonalCannotRebind) {
  WorkspaceSyncMapping mapping{{kWorkspaceSyncPersonalId, ""},
                               {kLogical, kLocal}};
  auto parsed = ParseWorkspaceSyncMapping(WorkspaceSyncMappingToValue(mapping));
  ASSERT_TRUE(parsed.has_value());
  EXPECT_EQ(*parsed, mapping);
  mapping.emplace(kOtherLogical, kLocal);
  EXPECT_FALSE(ParseWorkspaceSyncMapping(WorkspaceSyncMappingToValue(mapping))
                   .has_value());
  mapping.erase(kOtherLogical);
  mapping[kWorkspaceSyncPersonalId] = kOtherLogical;
  EXPECT_FALSE(ParseWorkspaceSyncMapping(WorkspaceSyncMappingToValue(mapping))
                   .has_value());
}

TEST(WorkspaceSyncDocumentTest, DormantExclusionsBoundRemoteAddRemoveChurn) {
  WorkspaceSyncMapping mapping{{kWorkspaceSyncPersonalId, ""}};
  auto excluded = ExcludedSpaces(1023);
  auto observed = ObservedSpaces(excluded);
  // The last tabs close in two excluded spaces. Their IDs are still permanent
  // exclusions even though they no longer appear in the observed snapshot.
  observed.resize(observed.size() - 2);
  ASSERT_EQ(observed.size(), 1022u);
  auto incoming = Document();
  incoming.spaces.push_back({kOtherLogical, "Another remote space", false});
  auto two = ValidateWorkspaceSyncApplyCapacity(mapping, excluded, observed,
                                                incoming.spaces);
  ASSERT_FALSE(two.has_value());
  EXPECT_EQ(two.error(), "too_many_local_workspaces");
  EXPECT_EQ(mapping.size(), 1u);
  EXPECT_EQ(excluded.size(), 1023u);

  incoming.spaces.pop_back();
  ASSERT_TRUE(ValidateWorkspaceSyncApplyCapacity(mapping, excluded, observed,
                                                 incoming.spaces)
                  .has_value());
  mapping.emplace(kLogical, kLocal);  // One admitted incoming reservation.
  ASSERT_TRUE(
      ValidateWorkspaceSyncIdentityCapacity(mapping, excluded).has_value());
  DetachWorkspaceSyncMappings({kWorkspaceSyncPersonalId}, mapping, excluded);
  ASSERT_EQ(excluded.size(), 1024u);
  EXPECT_TRUE(excluded.contains(kLocal));
  EXPECT_TRUE(excluded.contains(CapacityUuid(1022)));
  EXPECT_TRUE(excluded.contains(CapacityUuid(1023)));
  auto personal_only = Document();
  personal_only.spaces.resize(1);
  EXPECT_TRUE(ValidateWorkspaceSyncApplyCapacity(mapping, excluded, observed,
                                                 personal_only.spaces)
                  .has_value());

  incoming.spaces[1].id = kOtherLogical;
  const auto saved_mapping = mapping;
  const auto saved_excluded = excluded;
  auto next = ValidateWorkspaceSyncApplyCapacity(mapping, excluded, observed,
                                                 incoming.spaces);
  ASSERT_FALSE(next.has_value());
  EXPECT_EQ(next.error(), "too_many_local_workspaces");
  EXPECT_EQ(mapping, saved_mapping);
  EXPECT_EQ(excluded, saved_excluded);

  // The exact exclusion boundary stays readable through the same mapping
  // parser and reservation validation used when protected state is loaded.
  auto reloaded =
      ParseWorkspaceSyncMapping(WorkspaceSyncMappingToValue(mapping));
  ASSERT_TRUE(reloaded.has_value());
  base::ListValue serialized_excluded;
  for (const auto& id : excluded) {
    serialized_excluded.Append(id);
  }
  std::set<std::string> reloaded_excluded;
  for (const auto& id : serialized_excluded) {
    reloaded_excluded.insert(id.GetString());
  }
  EXPECT_TRUE(
      ValidateWorkspaceSyncIdentityCapacity(*reloaded, reloaded_excluded)
          .has_value());
}

TEST(WorkspaceSyncDocumentTest, LocalEnrollmentReservesFutureExclusionSlots) {
  const WorkspaceSyncMapping saved{{kWorkspaceSyncPersonalId, ""}};
  const auto excluded = ExcludedSpaces(1023);
  auto candidate = saved;
  candidate.emplace(kLogical, kLocal);
  ASSERT_TRUE(
      ValidateWorkspaceSyncIdentityCapacity(candidate, excluded).has_value());
  candidate.emplace(kOtherLogical, CapacityUuid(5000));
  auto rejected = ValidateWorkspaceSyncIdentityCapacity(candidate, excluded);
  ASSERT_FALSE(rejected.has_value());
  EXPECT_EQ(rejected.error(), "too_many_local_workspaces");
  EXPECT_EQ(saved.size(), 1u);
  EXPECT_EQ(excluded.size(), 1023u);
  // An over-reserved saved state is rejected at Load, before any model writes,
  // even if its mapping and exclusion lists each fit their individual limits.
  auto parsed =
      ParseWorkspaceSyncMapping(WorkspaceSyncMappingToValue(candidate));
  ASSERT_TRUE(parsed.has_value());
  EXPECT_FALSE(
      ValidateWorkspaceSyncIdentityCapacity(*parsed, excluded).has_value());
}

TEST(WorkspaceSyncDocumentTest,
     ReplacementAndResolutionKeepDetachReservations) {
  WorkspaceSyncMapping mapping{{kWorkspaceSyncPersonalId, ""},
                               {kLogical, kLocal}};
  auto excluded = ExcludedSpaces(1022);
  const std::vector<WorkspaceSyncSpace> observed{{"", "Personal", false},
                                                 {kLocal, "Current", false}};
  const auto local_choice = Document();
  EXPECT_TRUE(ValidateWorkspaceSyncApplyCapacity(mapping, excluded, observed,
                                                 local_choice.spaces)
                  .has_value());
  auto remote_choice = Document();
  remote_choice.spaces[1].id = kOtherLogical;
  remote_choice.spaces.push_back({CapacityUuid(7000), "Second new", false});
  // The old mapping will detach, not release its permanent identity slot.
  auto rejected = ValidateWorkspaceSyncApplyCapacity(
      mapping, excluded, observed, remote_choice.spaces);
  ASSERT_FALSE(rejected.has_value());
  EXPECT_EQ(rejected.error(), "too_many_local_workspaces");
  EXPECT_EQ(mapping.at(kLogical), kLocal);
  EXPECT_EQ(excluded.size(), 1022u);

  remote_choice.spaces.pop_back();
  ASSERT_TRUE(ValidateWorkspaceSyncApplyCapacity(mapping, excluded, observed,
                                                 remote_choice.spaces)
                  .has_value());
  const auto replacement_local = CapacityUuid(5000);
  mapping.emplace(kOtherLogical, replacement_local);
  ASSERT_TRUE(
      ValidateWorkspaceSyncIdentityCapacity(mapping, excluded).has_value());
  DetachWorkspaceSyncMappings({kWorkspaceSyncPersonalId, kOtherLogical},
                              mapping, excluded);
  EXPECT_TRUE(excluded.contains(kLocal));
  EXPECT_EQ(excluded.size(), 1023u);
  EXPECT_TRUE(
      ValidateWorkspaceSyncIdentityCapacity(mapping, excluded).has_value());
  DetachWorkspaceSyncMappings({kWorkspaceSyncPersonalId}, mapping, excluded);
  EXPECT_EQ(excluded.size(), 1024u);
  EXPECT_TRUE(excluded.contains(replacement_local));
  EXPECT_TRUE(
      ValidateWorkspaceSyncIdentityCapacity(mapping, excluded).has_value());
  const auto all_excluded = excluded;
  DetachWorkspaceSyncMappings({kWorkspaceSyncPersonalId}, mapping, excluded);
  EXPECT_EQ(excluded, all_excluded);
}

TEST(WorkspaceSyncDocumentTest, ObservedAndMappingBoundsRemainIndependent) {
  WorkspaceSyncMapping mapping{{kWorkspaceSyncPersonalId, ""}};
  const auto excluded = ExcludedSpaces(1023);
  auto observed = ObservedSpaces(excluded);
  const auto incoming = Document();
  // The identity reservation would fit, but there is no observed-space slot.
  EXPECT_FALSE(ValidateWorkspaceSyncApplyCapacity(mapping, excluded, observed,
                                                  incoming.spaces)
                   .has_value());
  observed.pop_back();
  EXPECT_TRUE(ValidateWorkspaceSyncApplyCapacity(mapping, excluded, observed,
                                                 incoming.spaces)
                  .has_value());

  for (size_t i = 1; i <= 1023; ++i) {
    mapping.emplace(CapacityUuid(i), CapacityUuid(i + 10000));
  }
  EXPECT_TRUE(ValidateWorkspaceSyncIdentityCapacity(mapping, {}).has_value());
  mapping.emplace(kLogical, kLocal);
  EXPECT_FALSE(ValidateWorkspaceSyncIdentityCapacity(mapping, {}).has_value());
}

TEST(WorkspaceSyncDocumentTest,
     PersonalAndEmptyMappingCapacityAreCheckedSafely) {
  EXPECT_FALSE(ValidateWorkspaceSyncIdentityCapacity({}, {}).has_value());
  EXPECT_FALSE(ValidateWorkspaceSyncApplyCapacity({}, {}, {}, {}).has_value());
  WorkspaceSyncMapping mapping{{kWorkspaceSyncPersonalId, ""}};
  EXPECT_TRUE(ValidateWorkspaceSyncIdentityCapacity(mapping, {}).has_value());
  EXPECT_TRUE(
      ValidateWorkspaceSyncIdentityCapacity(mapping, ExcludedSpaces(1024))
          .has_value());
  EXPECT_FALSE(
      ValidateWorkspaceSyncIdentityCapacity(mapping, ExcludedSpaces(1025))
          .has_value());
  mapping.emplace(kLogical, kLocal);
  EXPECT_FALSE(
      ValidateWorkspaceSyncIdentityCapacity(mapping, {kLocal}).has_value());
  mapping[kWorkspaceSyncPersonalId] = CapacityUuid(9000);
  EXPECT_FALSE(ValidateWorkspaceSyncIdentityCapacity(mapping, {}).has_value());
}

TEST(WorkspaceSyncDocumentTest, SameNamesNeverMergeDifferentLogicalIdentities) {
  auto local = Document();
  auto remote = local;
  remote.spaces[1].id = kOtherLogical;
  EXPECT_FALSE(WorkspaceSyncDocumentsEqual(local, remote));
  EXPECT_EQ(CompareWorkspaceSyncDocuments(local, remote, nullptr),
            WorkspaceSyncDecision::kApply);
}

TEST(WorkspaceSyncDocumentTest, ReconcilesOnlyAgainstLastCommonState) {
  const auto common = Document();
  auto local = common;
  auto remote = common;
  EXPECT_EQ(CompareWorkspaceSyncDocuments(local, remote, &common),
            WorkspaceSyncDecision::kEqual);
  local.spaces[1].name = "Local edit";
  EXPECT_EQ(CompareWorkspaceSyncDocuments(local, remote, &common),
            WorkspaceSyncDecision::kUpload);
  remote.spaces[1].archived = true;
  EXPECT_EQ(CompareWorkspaceSyncDocuments(local, remote, &common),
            WorkspaceSyncDecision::kConflict);
  local = common;
  EXPECT_EQ(CompareWorkspaceSyncDocuments(local, remote, &common),
            WorkspaceSyncDecision::kApply);
  local = remote;
  EXPECT_EQ(CompareWorkspaceSyncDocuments(local, remote, &common),
            WorkspaceSyncDecision::kApply);
}

TEST(WorkspaceSyncDocumentTest,
     EqualEditsNeedDurableApplyBeforeCommonAdvances) {
  const auto durable_common = Document();
  auto shared_edit = durable_common;
  shared_edit.spaces[1].name = "Same edit on both devices";
  EXPECT_EQ(
      CompareWorkspaceSyncDocuments(shared_edit, shared_edit, &durable_common),
      WorkspaceSyncDecision::kApply);
  EXPECT_EQ(CompareWorkspaceSyncDocuments(shared_edit, shared_edit, nullptr),
            WorkspaceSyncDecision::kApply);

  // Before the preference barrier a crash can restore the old local prefs.
  // The old common must still select download, never uploading the old data.
  EXPECT_EQ(CompareWorkspaceSyncDocuments(durable_common, shared_edit,
                                          &durable_common),
            WorkspaceSyncDecision::kApply);
  EXPECT_EQ(
      CompareWorkspaceSyncDocuments(shared_edit, shared_edit, &shared_edit),
      WorkspaceSyncDecision::kEqual);
}

TEST(WorkspaceSyncServiceTest, DisconnectRetryAndShutdownSignalOnlyOnce) {
  class Observer final : public syncer::CancelationSignal::Observer {
   public:
    void OnCancelationSignalReceived() override { ++calls; }
    int calls = 0;
  } observer;
  syncer::CancelationSignal signal;
  ASSERT_TRUE(signal.TryRegisterHandler(&observer));
  WorkspaceSyncServiceTestPeer::Cancel(signal);  // Disconnect.
  WorkspaceSyncServiceTestPeer::Cancel(signal);  // Retry after cleanup failed.
  WorkspaceSyncServiceTestPeer::Cancel(signal);  // Shutdown before completion.
  EXPECT_TRUE(signal.IsSignalled());
  EXPECT_EQ(observer.calls, 1);
  signal.UnregisterHandler(&observer);
  EXPECT_FALSE(signal.TryRegisterHandler(&observer));
}
}  // namespace
}  // namespace noventra
