// Copyright 2015 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "content/browser/notifications/notification_id_generator.h"

#include <stdint.h>

#include "base/strings/string_number_conversions.h"
#include "content/public/browser/storage_partition_config.h"
#include "content/test/storage_partition_test_helpers.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "url/gurl.h"
#include "url/origin.h"

namespace content {
namespace {

const int64_t kPersistentNotificationId = 430;
const char kExampleTag[] = "example";
const char kSpaceA[] = "11111111-1111-4111-8111-111111111111";
const char kSpaceB[] = "22222222-2222-4222-8222-222222222222";

StoragePartitionConfig SpaceConfig(const std::string& space,
                                   bool in_memory = false) {
  return CreateStoragePartitionConfigForTesting(
      in_memory, "noventra-account-spaces", space);
}

class NotificationIdGeneratorTest : public ::testing::Test {
 public:
  NotificationIdGeneratorTest()
      : origin_(url::Origin::Create(GURL("https://example.com"))) {}

 protected:
  url::Origin origin_;
  NotificationIdGenerator generator_;
};

// -----------------------------------------------------------------------------
// Persistent notifications

// Two calls to the generator with exactly the same information should result
// in exactly the same notification ids being generated.
TEST_F(NotificationIdGeneratorTest, GenerateForPersistent_IsDetermenistic) {
  EXPECT_EQ(generator_.GenerateForPersistentNotification(
                origin_.GetURL(), kExampleTag, false /* is_shown_by_browser */,
                kPersistentNotificationId),
            generator_.GenerateForPersistentNotification(
                origin_.GetURL(), kExampleTag, false /* is_shown_by_browser */,
                kPersistentNotificationId));

  EXPECT_EQ(generator_.GenerateForPersistentNotification(
                origin_.GetURL(), "" /* tag */, false /* is_shown_by_browser */,
                kPersistentNotificationId),
            generator_.GenerateForPersistentNotification(
                origin_.GetURL(), "" /* tag */, false /* is_shown_by_browser */,
                kPersistentNotificationId));
}

// The origin of the notification will impact the generated notification id.
TEST_F(NotificationIdGeneratorTest, GenerateForPersistent_DifferentOrigins) {
  url::Origin different_origin(
      url::Origin::Create(GURL("https://example2.com")));

  EXPECT_NE(generator_.GenerateForPersistentNotification(
                origin_.GetURL(), kExampleTag, false /* is_shown_by_browser */,
                kPersistentNotificationId),
            generator_.GenerateForPersistentNotification(
                different_origin.GetURL(), kExampleTag,
                false /* is_shown_by_browser */, kPersistentNotificationId));
}

// The tag, when non-empty, will impact the generated notification id.
TEST_F(NotificationIdGeneratorTest, GenerateForPersistent_DifferentTags) {
  const std::string& different_tag = std::string(kExampleTag) + "2";

  EXPECT_NE(generator_.GenerateForPersistentNotification(
                origin_.GetURL(), kExampleTag, false /* is_shown_by_browser */,
                kPersistentNotificationId),
            generator_.GenerateForPersistentNotification(
                origin_.GetURL(), different_tag,
                false /* is_shown_by_browser */, kPersistentNotificationId));
}

// The persistent or non-persistent notification id will impact the generated
// notification id when the tag is empty.
TEST_F(NotificationIdGeneratorTest, GenerateForPersistent_DifferentIds) {
  EXPECT_NE(generator_.GenerateForPersistentNotification(
                origin_.GetURL(), "" /* tag */, false /* is_shown_by_browser */,
                kPersistentNotificationId),
            generator_.GenerateForPersistentNotification(
                origin_.GetURL(), "" /* tag */, false /* is_shown_by_browser */,
                kPersistentNotificationId + 1));
}

// Using a numeric tag that could resemble a persistent notification id should
// not be equal to a notification without a tag, but with that id.
TEST_F(NotificationIdGeneratorTest, GenerateForPersistent_NumericTagAmbiguity) {
  EXPECT_NE(
      generator_.GenerateForPersistentNotification(
          origin_.GetURL(), base::NumberToString(kPersistentNotificationId),
          false /* is_shown_by_browser */, kPersistentNotificationId),
      generator_.GenerateForPersistentNotification(
          origin_.GetURL(), "" /* tag */, false /* is_shown_by_browser */,
          kPersistentNotificationId));
}

// Using port numbers and a tag which, when concatenated, could end up being
// equal to each other if origins stop ending with slashes.
TEST_F(NotificationIdGeneratorTest, GenerateForPersistent_OriginPortAmbiguity) {
  GURL origin_805("https://example.com:805");
  GURL origin_8051("https://example.com:8051");

  EXPECT_NE(generator_.GenerateForPersistentNotification(
                origin_805, "17", false /* is_shown_by_browser */,
                kPersistentNotificationId),
            generator_.GenerateForPersistentNotification(
                origin_8051, "7", false /* is_shown_by_browser */,
                kPersistentNotificationId));
}

// -----------------------------------------------------------------------------
// Non-persistent notifications

TEST_F(NotificationIdGeneratorTest, GenerateForNonPersistent_IsDeterministic) {
  EXPECT_EQ(
      generator_.GenerateForNonPersistentNotification(origin_, "example-token"),
      generator_.GenerateForNonPersistentNotification(origin_,
                                                      "example-token"));
}

TEST_F(NotificationIdGeneratorTest, GenerateForNonPersistent_DifferentOrigins) {
  url::Origin different_origin(
      url::Origin::Create(GURL("https://example2.com")));

  EXPECT_NE(
      generator_.GenerateForNonPersistentNotification(origin_, "example-token"),
      generator_.GenerateForNonPersistentNotification(different_origin,
                                                      "example-token"));
}

TEST_F(NotificationIdGeneratorTest, GenerateForNonPersistent_DifferentTokens) {
  EXPECT_NE(
      generator_.GenerateForNonPersistentNotification(origin_, "example-token"),
      generator_.GenerateForNonPersistentNotification(origin_, "other-token"));
}

// Use port numbers and a token which, when concatenated, could end up being
// equal to each other if origins stop ending with slashes.
TEST_F(NotificationIdGeneratorTest,
       GenerateForNonPersistent_OriginPortAmbiguity) {
  auto origin_805(url::Origin::Create(GURL("https://example.com:805")));
  auto origin_8051(url::Origin::Create(GURL("https://example.com:8051")));

  EXPECT_NE(generator_.GenerateForNonPersistentNotification(origin_805, "17"),
            generator_.GenerateForNonPersistentNotification(origin_8051, "7"));
}

// -----------------------------------------------------------------------------
// Both persistent and non-persistent notifications.

// Verifies that the static Is(Non)PersistentNotification helpers can correctly
// identify persistent and non-persistent notification IDs.
TEST_F(NotificationIdGeneratorTest, DetectIdFormat) {
  std::string persistent_id = generator_.GenerateForPersistentNotification(
      origin_.GetURL(), "" /* tag */, false /* is_shown_by_browser */,
      kPersistentNotificationId);

  std::string non_persistent_id =
      generator_.GenerateForNonPersistentNotification(origin_, "token");

  EXPECT_TRUE(NotificationIdGenerator::IsPersistentNotification(persistent_id));
  EXPECT_FALSE(
      NotificationIdGenerator::IsNonPersistentNotification(persistent_id));

  EXPECT_TRUE(
      NotificationIdGenerator::IsNonPersistentNotification(non_persistent_id));
  EXPECT_FALSE(
      NotificationIdGenerator::IsPersistentNotification(non_persistent_id));
}

TEST_F(NotificationIdGeneratorTest, ShownByBrowser) {
  std::string id = generator_.GenerateForPersistentNotification(
      origin_.GetURL(), "" /* tag */, true /* is_shown_by_browser */,
      kPersistentNotificationId);

  // We should still be able to detect browser notifications as persistent.
  EXPECT_TRUE(NotificationIdGenerator::IsPersistentNotification(id));
  EXPECT_FALSE(NotificationIdGenerator::IsNonPersistentNotification(id));

  // Browser vs non-browser ids should be different.
  EXPECT_NE(generator_.GenerateForPersistentNotification(
                origin_.GetURL(), kExampleTag, false /* is_shown_by_browser */,
                kPersistentNotificationId),
            generator_.GenerateForPersistentNotification(
                origin_.GetURL(), kExampleTag, true /* is_shown_by_browser */,
                kPersistentNotificationId));
}

TEST_F(NotificationIdGeneratorTest, LegacyBytesUnchanged) {
  EXPECT_EQ("p#https://example.com/#1example",
            generator_.GenerateForPersistentNotification(
                origin_.GetURL(), kExampleTag, false, 430));
  EXPECT_EQ("pb#https://example.com/#0430",
            generator_.GenerateForPersistentNotification(origin_.GetURL(), "",
                                                         true, 430));
  EXPECT_EQ("n#https://example.com#token",
            generator_.GenerateForNonPersistentNotification(origin_, "token"));
  NotificationIdGenerator other_partition(
      CreateStoragePartitionConfigForTesting(false, "other-domain", "name"));
  EXPECT_EQ(generator_.GenerateForPersistentNotification(
                origin_.GetURL(), kExampleTag, false, 430),
            other_partition.GenerateForPersistentNotification(
                origin_.GetURL(), kExampleTag, false, 430));
}

TEST_F(NotificationIdGeneratorTest, AccountSpacesSeparateTagsIdsAndTokens) {
  NotificationIdGenerator a(SpaceConfig(kSpaceA));
  NotificationIdGenerator b(SpaceConfig(kSpaceB));
  NotificationIdGenerator a_after_restart(SpaceConfig(kSpaceA));
  NotificationIdGenerator a_in_memory(SpaceConfig(kSpaceA, true));
  for (bool browser_notification : {false, true}) {
    for (const std::string tag : {std::string(), std::string(kExampleTag)}) {
      const auto id = a.GenerateForPersistentNotification(
          origin_.GetURL(), tag, browser_notification, 430);
      EXPECT_NE(id, generator_.GenerateForPersistentNotification(
                        origin_.GetURL(), tag, browser_notification, 430));
      EXPECT_NE(id, b.GenerateForPersistentNotification(
                        origin_.GetURL(), tag, browser_notification, 430));
      EXPECT_NE(id, a_in_memory.GenerateForPersistentNotification(
                        origin_.GetURL(), tag, browser_notification, 430));
      EXPECT_EQ(id, a_after_restart.GenerateForPersistentNotification(
                        origin_.GetURL(), tag, browser_notification, 430));
      EXPECT_TRUE(NotificationIdGenerator::IsPersistentNotification(id));
      EXPECT_TRUE(a.IsValidPersistentNotificationId(id, origin_.GetURL()));
      EXPECT_FALSE(b.IsValidPersistentNotificationId(id, origin_.GetURL()));
      EXPECT_FALSE(
          generator_.IsValidPersistentNotificationId(id, origin_.GetURL()));
    }
  }
  const auto id = a.GenerateForNonPersistentNotification(origin_, "same-token");
  EXPECT_NE(id, b.GenerateForNonPersistentNotification(origin_, "same-token"));
  EXPECT_NE(id, generator_.GenerateForNonPersistentNotification(origin_,
                                                                "same-token"));
  EXPECT_EQ(id, a_after_restart.GenerateForNonPersistentNotification(
                    origin_, "same-token"));
  EXPECT_TRUE(NotificationIdGenerator::IsNonPersistentNotification(id));
}

TEST_F(NotificationIdGeneratorTest, AccountNamespaceCannotBeForgedByTag) {
  NotificationIdGenerator a(SpaceConfig(kSpaceA));
  NotificationIdGenerator b(SpaceConfig(kSpaceB));
  const std::string fake_id = b.GenerateForPersistentNotification(
      origin_.GetURL(), kExampleTag, false, 430);
  const std::string id = a.GenerateForPersistentNotification(
      origin_.GetURL(), fake_id, false, 430);
  EXPECT_TRUE(a.IsValidPersistentNotificationId(id, origin_.GetURL()));
  EXPECT_FALSE(b.IsValidPersistentNotificationId(id, origin_.GetURL()));
  EXPECT_FALSE(
      generator_.IsValidPersistentNotificationId(id, origin_.GetURL()));
  EXPECT_NE(id, fake_id);
  const std::string personal = generator_.GenerateForPersistentNotification(
      origin_.GetURL(), fake_id, false, 430);
  EXPECT_TRUE(
      generator_.IsValidPersistentNotificationId(personal, origin_.GetURL()));
  EXPECT_FALSE(b.IsValidPersistentNotificationId(personal, origin_.GetURL()));
}

TEST_F(NotificationIdGeneratorTest,
       AccountCloseRejectsForeignOriginAndIdentity) {
  NotificationIdGenerator a(SpaceConfig(kSpaceA));
  const auto id = a.GenerateForPersistentNotification(origin_.GetURL(),
                                                      kExampleTag, false, 430);
  EXPECT_FALSE(
      a.IsValidPersistentNotificationId(id, GURL("https://other.com")));
  EXPECT_FALSE(
      a.IsValidPersistentNotificationId(id, GURL("https://example.com/path")));
  EXPECT_FALSE(a.IsValidPersistentNotificationId(
      generator_.GenerateForPersistentNotification(origin_.GetURL(),
                                                   kExampleTag, false, 430),
      origin_.GetURL()));
  NotificationIdGenerator memory(SpaceConfig(kSpaceA, true));
  EXPECT_FALSE(memory.IsValidPersistentNotificationId(id, origin_.GetURL()));
  const std::string invalid_prefixes[] = {
      "ps0../profile",
      "ps0not-a-uuid",
      "ps2" + std::string(kSpaceA),
      "ps0" + std::string(kSpaceA) + "extra",
      "pbs0",
      "ps",
      "n",
      ""};
  for (const auto& prefix : invalid_prefixes) {
    EXPECT_FALSE(a.IsValidPersistentNotificationId(
        prefix + "#https://example.com/#1example", origin_.GetURL()));
  }
}

}  // namespace
}  // namespace content
