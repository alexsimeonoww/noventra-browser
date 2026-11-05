// Copyright 2025 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "components/sessions/core/tab_restore_service_helper.h"

#include <optional>

#include "testing/gtest/include/gtest/gtest.h"

namespace sessions {

std::unique_ptr<tab_restore::Group> CreateTabRestoreGroup(
    tab_groups::TabGroupId group_id,
    const std::optional<base::Uuid>& saved_group_id) {
  auto group = std::make_unique<tab_restore::Group>();
  group->group_id = group_id;

  group->saved_group_id = saved_group_id;
  return group;
}

std::unique_ptr<tab_restore::Tab> CreateTabWithGroupInfo(
    tab_groups::TabGroupId group_id,
    const std::optional<base::Uuid>& saved_group_id) {
  auto tab = std::make_unique<tab_restore::Tab>();
  tab->group = group_id;
  tab->saved_group_id = saved_group_id;
  return tab;
}

class TabRestoreServiceHelperTest : public testing::Test {};

TEST_F(TabRestoreServiceHelperTest, CreateLocalSavedGroupIDMapping) {
  std::map<tab_groups::TabGroupId, std::unique_ptr<tab_restore::Group>> groups;

  tab_groups::TabGroupId local_group_id_1 =
      tab_groups::TabGroupId::GenerateNew();
  tab_groups::TabGroupId local_group_id_2 =
      tab_groups::TabGroupId::GenerateNew();

  base::Uuid saved_uuid_1 = base::Uuid::GenerateRandomV4();

  groups[local_group_id_1] =
      CreateTabRestoreGroup(local_group_id_1, saved_uuid_1);
  groups[local_group_id_2] =
      CreateTabRestoreGroup(local_group_id_2, std::nullopt);

  std::map<tab_groups::TabGroupId, base::Uuid> result =
      TabRestoreServiceHelper::CreateLocalSavedGroupIDMapping(groups);

  // Verify that only groups with saved_group_id appear in the mapping.
  ASSERT_EQ(result.size(), 1u);
  EXPECT_EQ(result[local_group_id_1], saved_uuid_1);
}

TEST_F(TabRestoreServiceHelperTest, UpdateSavedGroupIDsForTabEntries) {
  tab_groups::TabGroupId local_group_id_1 =
      tab_groups::TabGroupId::GenerateNew();
  tab_groups::TabGroupId local_group_id_2 =
      tab_groups::TabGroupId::GenerateNew();
  tab_groups::TabGroupId local_group_id_3 =
      tab_groups::TabGroupId::GenerateNew();

  base::Uuid saved_guid_1 = base::Uuid::GenerateRandomV4();
  base::Uuid saved_guid_2 = base::Uuid::GenerateRandomV4();

  std::vector<std::unique_ptr<tab_restore::Tab>> tabs;
  tabs.push_back(CreateTabWithGroupInfo(local_group_id_1, std::nullopt));
  tabs.push_back(CreateTabWithGroupInfo(local_group_id_2, std::nullopt));
  tabs.push_back(CreateTabWithGroupInfo(local_group_id_3, std::nullopt));
  tabs.push_back(std::make_unique<tab_restore::Tab>());

  std::map<tab_groups::TabGroupId, std::unique_ptr<tab_restore::Group>> groups;
  groups[local_group_id_1] =
      CreateTabRestoreGroup(local_group_id_1, saved_guid_1);
  groups[local_group_id_2] =
      CreateTabRestoreGroup(local_group_id_2, saved_guid_2);
  groups[local_group_id_3] =
      CreateTabRestoreGroup(local_group_id_3, std::nullopt);

  TabRestoreServiceHelper::UpdateSavedGroupIDsForTabEntries(
      tabs, TabRestoreServiceHelper::CreateLocalSavedGroupIDMapping(groups));

  // Verify that only tabs with a group in the mapping are updated.
  ASSERT_TRUE(tabs[0]->saved_group_id.has_value());
  EXPECT_EQ(tabs[0]->saved_group_id.value(), saved_guid_1);

  ASSERT_TRUE(tabs[1]->saved_group_id.has_value());
  EXPECT_EQ(tabs[1]->saved_group_id.value(), saved_guid_2);

  // 3rd tab's group isn't in the mapping, so Tab 2 remains unchanged.
  EXPECT_FALSE(tabs[2]->saved_group_id.has_value());

  // The last tab has no group, so it should also remain nullopt.
  EXPECT_FALSE(tabs[3]->saved_group_id.has_value());
}

TEST_F(TabRestoreServiceHelperTest, RestoreEligibilityProtectsWholeSavedGroup) {
  tab_restore::Group group;
  group.saved_group_id = base::Uuid::GenerateRandomV4();
  auto own = std::make_unique<tab_restore::Tab>();
  own->extra_data["owner"] = "personal";
  auto foreign = std::make_unique<tab_restore::Tab>();
  foreign->extra_data["owner"] = "work";
  group.tabs.push_back(std::move(own));
  group.tabs.push_back(std::move(foreign));
  const auto personal = [](const tab_restore::Tab& tab) {
    return tab.extra_data.at("owner") == "personal";
  };
  EXPECT_FALSE(TabRestoreServiceHelper::CanRestoreEntry(group, personal));
  EXPECT_EQ(group.tabs.size(), 2u);
  EXPECT_TRUE(group.saved_group_id.has_value());
  EXPECT_EQ(group.tabs[1]->extra_data.at("owner"), "work");
  group.tabs[1]->extra_data["owner"] = "personal";
  EXPECT_TRUE(TabRestoreServiceHelper::CanRestoreEntry(group, personal));
}

TEST_F(TabRestoreServiceHelperTest, RestoreEligibilityRejectsMixedSplit) {
  tab_restore::Split split;
  split.split_id = split_tabs::SplitTabId::GenerateNew();
  for (bool eligible : {true, false, true}) {
    auto tab = std::make_unique<tab_restore::Tab>();
    tab->pinned = eligible;
    split.tabs.push_back(std::move(tab));
  }
  EXPECT_FALSE(TabRestoreServiceHelper::CanRestoreEntry(
      split, [](const tab_restore::Tab& tab) { return tab.pinned; }));
  EXPECT_EQ(split.tabs.size(), 3u);
  EXPECT_TRUE(split.split_id.has_value());
}

TEST_F(TabRestoreServiceHelperTest, RestoreEligibilityChecksEveryWindowTab) {
  tab_restore::Window window;
  window.tabs.push_back(std::make_unique<tab_restore::Tab>());
  window.tabs.push_back(std::make_unique<tab_restore::Tab>());
  window.tabs[0]->extra_data["owner"] = "work";
  window.tabs[1]->extra_data["owner"] = "personal";
  EXPECT_FALSE(TabRestoreServiceHelper::CanRestoreEntry(
      window, [](const tab_restore::Tab& tab) {
        return tab.extra_data.at("owner") == "work";
      }));
  EXPECT_TRUE(TabRestoreServiceHelper::CanRestoreEntry(
      window, [](const tab_restore::Tab& tab) { return true; }));
  EXPECT_EQ(window.tabs.size(), 2u);
}

TEST_F(TabRestoreServiceHelperTest, RestoreEligibilityRejectsEmptyBatches) {
  tab_restore::Group group;
  tab_restore::Split split;
  tab_restore::Window window;
  const auto allow = [](const tab_restore::Tab& tab) { return true; };
  EXPECT_FALSE(TabRestoreServiceHelper::CanRestoreEntry(group, allow));
  EXPECT_FALSE(TabRestoreServiceHelper::CanRestoreEntry(split, allow));
  EXPECT_FALSE(TabRestoreServiceHelper::CanRestoreEntry(window, allow));
  tab_restore::Tab tab;
  EXPECT_TRUE(TabRestoreServiceHelper::CanRestoreEntry(tab, allow));
}

}  // namespace sessions
