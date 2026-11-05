// Copyright 2015 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "content/browser/notifications/notification_id_generator.h"

#include <optional>
#include <sstream>
#include <string_view>

#include "base/check_op.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/stringprintf.h"
#include "base/strings/utf_string_conversions.h"
#include "base/uuid.h"
#include "content/public/browser/browser_context.h"
#include "content/public/browser/storage_partition_config.h"
#include "url/gurl.h"

namespace content {
namespace {

const char kNotificationTagSeparator = '#';
const char kPersistentNotificationPrefix = 'p';
const char kNonPersistentNotificationPrefix = 'n';
const char kNotificationShownByBrowserFlag = 'b';
constexpr char kAccountSpacePartitionDomain[] = "noventra-account-spaces";
constexpr size_t kAccountSpaceNamespaceLength = 38;  // s + memory bit + UUID.

std::string PartitionNamespace(const StoragePartitionConfig& config) {
  if (config.partition_domain() != kAccountSpacePartitionDomain) {
    return {};
  }
  CHECK(base::Uuid::ParseLowercase(config.partition_name()).is_valid());
  return std::string(config.in_memory() ? "s1" : "s0") +
         config.partition_name();
}

// Parse only the bounded prefix and the expected origin, not the arbitrary
// developer tag. The prefix is p[b][s<memory bit><canonical UUID>].
std::optional<std::string_view> ParsePersistentPartitionNamespace(
    std::string_view notification_id,
    const GURL& origin) {
  if (!origin.is_valid() || origin != origin.DeprecatedGetOriginAsURL()) {
    return std::nullopt;
  }
  const size_t separator =
      notification_id.substr(0, 2 + kAccountSpaceNamespaceLength + 1)
          .find(kNotificationTagSeparator);
  if (separator == std::string_view::npos) {
    return std::nullopt;
  }
  std::string_view prefix = notification_id.substr(0, separator);
  if (!prefix.starts_with(kPersistentNotificationPrefix)) {
    return std::nullopt;
  }
  prefix.remove_prefix(1);
  if (prefix.starts_with(kNotificationShownByBrowserFlag)) {
    prefix.remove_prefix(1);
  }
  if (!prefix.empty() &&
      (prefix.size() != kAccountSpaceNamespaceLength || prefix[0] != 's' ||
       (prefix[1] != '0' && prefix[1] != '1') ||
       !base::Uuid::ParseLowercase(prefix.substr(2)).is_valid())) {
    return std::nullopt;
  }
  notification_id.remove_prefix(separator + 1);
  if (!notification_id.starts_with(origin.spec())) {
    return std::nullopt;
  }
  notification_id.remove_prefix(origin.spec().size());
  if (notification_id.size() < 3 || notification_id[0] != '#' ||
      (notification_id[1] != '0' && notification_id[1] != '1')) {
    return std::nullopt;
  }
  return prefix;
}

}  // namespace

NotificationIdGenerator::NotificationIdGenerator(
    const StoragePartitionConfig& config)
    : partition_namespace_(PartitionNamespace(config)) {}

// static
StoragePartition*
NotificationIdGenerator::GetStoragePartitionForPersistentNotification(
    BrowserContext* browser_context,
    std::string_view notification_id,
    const GURL& origin) {
  const auto partition_namespace =
      ParsePersistentPartitionNamespace(notification_id, origin);
  if (!partition_namespace) {
    return nullptr;
  }
  if (partition_namespace->empty()) {
    return browser_context->GetStoragePartitionForUrl(origin);
  }
  const bool in_memory = (*partition_namespace)[1] == '1';
  // SpaceConfig uses in-memory partitions only for off-the-record profiles.
  // Do not let Create() silently coerce a regular ID into an OTR partition.
  if (in_memory != browser_context->IsOffTheRecord()) {
    return nullptr;
  }
  return browser_context->GetStoragePartition(StoragePartitionConfig::Create(
      browser_context, kAccountSpacePartitionDomain,
      partition_namespace->substr(2), in_memory));
}

bool NotificationIdGenerator::IsValidPersistentNotificationId(
    std::string_view notification_id,
    const GURL& origin) const {
  const auto partition_namespace =
      ParsePersistentPartitionNamespace(notification_id, origin);
  return partition_namespace && *partition_namespace == partition_namespace_;
}

// static
bool NotificationIdGenerator::IsPersistentNotification(
    const std::string_view& notification_id) {
  return notification_id.length() > 0 &&
         notification_id.front() == kPersistentNotificationPrefix;
}

// static
bool NotificationIdGenerator::IsNonPersistentNotification(
    const std::string_view& notification_id) {
  return notification_id.length() > 0 &&
         notification_id.front() == kNonPersistentNotificationPrefix;
}

// Notification Id is of the following format:
// p[b][s<memory bit><space UUID>]#<origin>#[1|0][tag|persistent_id]
std::string NotificationIdGenerator::GenerateForPersistentNotification(
    const GURL& origin,
    const std::string& tag,
    bool is_shown_by_browser,
    int64_t persistent_notification_id) const {
  CHECK(origin.is_valid(), base::NotFatalUntil::M159);
  CHECK_EQ(origin, origin.DeprecatedGetOriginAsURL(),
           base::NotFatalUntil::M159);

  std::stringstream stream;

  stream << kPersistentNotificationPrefix;

  if (is_shown_by_browser)
    stream << kNotificationShownByBrowserFlag;

  stream << partition_namespace_;
  stream << kNotificationTagSeparator;
  stream << origin;
  stream << kNotificationTagSeparator;

  stream << base::NumberToString(!tag.empty());
  if (tag.size())
    stream << tag;
  else
    stream << base::NumberToString(persistent_notification_id);

  return stream.str();
}

// Notification Id is of the following format:
// n[s<memory bit><space UUID>]#<origin>#<token>
std::string NotificationIdGenerator::GenerateForNonPersistentNotification(
    const url::Origin& origin,
    const std::string& token) const {
  CHECK(!origin.opaque(), base::NotFatalUntil::M159);
  CHECK(!token.empty(), base::NotFatalUntil::M159);
  return base::StringPrintf(
      "%c%s%c%s%c%s", kNonPersistentNotificationPrefix,
      partition_namespace_.c_str(), kNotificationTagSeparator,
      origin.Serialize().c_str(), kNotificationTagSeparator, token.c_str());
}

}  // namespace content
