// Copyright 2026 The Noventra Authors. All rights reserved.

#include "components/noventra_sync_vault/sync_vault_format.h"

#include <vector>

#include "base/strings/string_number_conversions.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace noventra_sync {
namespace {

// Literal wire fixtures from the Noventra154 proto2 field contract. They must
// remain readable after changing the C++ and generated protobuf namespaces.
constexpr char kVault[] = "c65f6051-1089-4518-9eb1-8fb140a9bb59";

TEST(SyncVaultFormatCompatibilityTest, PreservesExistingDescriptorBytes) {
  std::vector<uint8_t> fixture;
  ASSERT_TRUE(base::HexStringToBytes(
      "0801122463363566363035312D313038392D343531382D396562312D386662313430"
      "6139626235391800",
      &fixture));
  auto parsed = ParseSyncVaultDescriptor(fixture);
  ASSERT_TRUE(parsed.has_value());
  EXPECT_EQ(parsed->vault_uuid, kVault);
  EXPECT_EQ(parsed->creation_time_millis, 0);
  auto serialized = SerializeSyncVaultDescriptor(*parsed);
  ASSERT_TRUE(serialized.has_value());
  EXPECT_EQ(*serialized, fixture);
}

TEST(SyncVaultFormatCompatibilityTest, PreservesExistingCheckpointBytes) {
  std::vector<uint8_t> fixture;
  ASSERT_TRUE(base::HexStringToBytes(
      "0801122463363566363035312D313038392D343531382D396562312D386662313430"
      "613962623539180022002800",
      &fixture));
  auto parsed = ParseSyncVaultCheckpoint(fixture);
  ASSERT_TRUE(parsed.has_value());
  EXPECT_EQ(parsed->vault_uuid, kVault);
  EXPECT_EQ(parsed->generation, 0u);
  EXPECT_TRUE(parsed->record_id.empty());
  EXPECT_EQ(parsed->epoch, 0u);
  auto serialized = SerializeSyncVaultCheckpoint(*parsed);
  ASSERT_TRUE(serialized.has_value());
  EXPECT_EQ(*serialized, fixture);
}

}  // namespace
}  // namespace noventra_sync
