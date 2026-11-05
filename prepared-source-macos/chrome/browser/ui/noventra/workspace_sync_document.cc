// Copyright 2026 The Noventra Authors. All rights reserved.
#include "chrome/browser/ui/noventra/workspace_sync_document.h"

#include <algorithm>
#include <set>
#include <tuple>

#include "base/json/json_reader.h"
#include "base/json/json_writer.h"
#include "base/strings/string_util.h"
#include "base/uuid.h"

namespace noventra {
namespace {
constexpr char kInvalidDocument[] = "invalid_workspace_document";

base::DictValue PortableLayout(const BrowserLayout& input) {
  auto layout = input;
  layout.revision = 0;
  std::ranges::sort(layout.items, [](const auto& a, const auto& b) {
    return std::tie(a.edge, a.order, a.id) < std::tie(b.edge, b.order, b.id);
  });
  return LayoutToValue(layout);
}
}  // namespace

bool IsWorkspaceSyncUuid(std::string_view value) {
  const auto uuid = base::Uuid::ParseLowercase(value);
  return uuid.is_valid() && uuid.AsLowercaseString() == value;
}

base::DictValue WorkspaceSyncDocumentToValue(
    const WorkspaceSyncDocument& document) {
  base::ListValue spaces;
  for (const auto& space : document.spaces) {
    spaces.Append(base::DictValue()
                      .Set("id", space.id)
                      .Set("name", space.name)
                      .Set("archived", space.archived));
  }
  return base::DictValue()
      .Set("format", "noventra-workspace-sync")
      .Set("version", 1)
      .Set("spaces", std::move(spaces))
      .Set("layout", PortableLayout(document.layout));
}

base::expected<WorkspaceSyncDocument, std::string> ParseWorkspaceSyncDocument(
    std::string_view bytes,
    base::span<const LayoutCapability> capabilities) {
  if (bytes.size() > kWorkspaceSyncMaximumDocumentBytes) {
    return base::unexpected(kInvalidDocument);
  }
  auto value = base::JSONReader::ReadDict(bytes, base::JSON_PARSE_RFC);
  if (!value) {
    return base::unexpected(kInvalidDocument);
  }
  auto document = ValidateWorkspaceSyncDocument(*value, capabilities);
  // The browser emits one canonical representation. Exact round-trip rejects
  // duplicate JSON keys and alternate encodings that JSONReader normalizes.
  if (!document.has_value()) {
    return document;
  }
  if (SerializeWorkspaceSyncDocument(*document) != bytes) {
    return base::unexpected(kInvalidDocument);
  }
  return document;
}

base::expected<WorkspaceSyncDocument, std::string>
ValidateWorkspaceSyncDocument(const base::DictValue& value,
                              base::span<const LayoutCapability> capabilities) {
  const auto* format = value.FindString("format");
  const auto* spaces = value.FindList("spaces");
  const auto* layout = value.FindDict("layout");
  if (value.size() != 4 || !format || *format != "noventra-workspace-sync" ||
      value.FindInt("version") != 1 || !spaces || spaces->empty() ||
      spaces->size() > kWorkspaceSyncMaximumSpaces || !layout ||
      layout->size() != 3 || layout->FindInt("revision") != 0) {
    return base::unexpected(kInvalidDocument);
  }
  WorkspaceSyncDocument result;
  std::set<std::string> ids;
  for (const auto& entry : *spaces) {
    const auto* space = entry.GetIfDict();
    const auto* id = space ? space->FindString("id") : nullptr;
    const auto* name = space ? space->FindString("name") : nullptr;
    const auto archived = space ? space->FindBool("archived") : std::nullopt;
    std::string trimmed;
    if (name) {
      base::TrimWhitespaceASCII(*name, base::TRIM_ALL, &trimmed);
    }
    if (!space || space->size() != 3 || !id || !name || !archived ||
        (*id != kWorkspaceSyncPersonalId && !IsWorkspaceSyncUuid(*id)) ||
        !ids.insert(*id).second || name->empty() || name->size() > 160 ||
        trimmed != *name || !base::IsStringUTF8(*name) ||
        std::ranges::any_of(
            *name, [](unsigned char c) { return c < 0x20 || c == 0x7f; }) ||
        (*id == kWorkspaceSyncPersonalId && *archived)) {
      return base::unexpected(kInvalidDocument);
    }
    result.spaces.push_back({*id, *name, *archived});
  }
  if (!ids.contains(kWorkspaceSyncPersonalId)) {
    return base::unexpected(kInvalidDocument);
  }
  const auto* items = layout->FindList("items");
  if (!items || std::ranges::any_of(*items, [](const base::Value& item) {
        return !item.is_dict() || item.GetDict().size() != 4;
      })) {
    return base::unexpected(kInvalidDocument);
  }
  auto checked = ValidateLayout(base::Value(layout->Clone()), capabilities);
  if (!checked.has_value()) {
    return base::unexpected("unsupported_workspace_layout");
  }
  for (const auto& capability : capabilities) {
    if (capability.managed) {
      return base::unexpected("workspace_settings_managed");
    }
    if (capability.required_visibility) {
      auto item =
          std::ranges::find(checked->items, capability.id, &LayoutItem::id);
      if (item == checked->items.end() ||
          item->visible != *capability.required_visibility) {
        return base::unexpected("workspace_settings_managed");
      }
    }
  }
  result.layout = std::move(*checked);
  return result;
}

std::string SerializeWorkspaceSyncDocument(
    const WorkspaceSyncDocument& document) {
  return base::WriteJson(WorkspaceSyncDocumentToValue(document)).value_or("");
}

bool WorkspaceSyncDocumentsEqual(const WorkspaceSyncDocument& a,
                                 const WorkspaceSyncDocument& b) {
  return a.spaces == b.spaces && WorkspaceSyncLayoutsEqual(a.layout, b.layout);
}

bool WorkspaceSyncLayoutsEqual(const BrowserLayout& a, const BrowserLayout& b) {
  return PortableLayout(a) == PortableLayout(b);
}

base::expected<WorkspaceSyncMapping, std::string> ParseWorkspaceSyncMapping(
    const base::DictValue& value) {
  const auto* personal = value.FindString(kWorkspaceSyncPersonalId);
  if (!personal || !personal->empty() ||
      value.size() > kWorkspaceSyncMaximumLocalSpaces) {
    return base::unexpected("invalid_workspace_mapping");
  }
  WorkspaceSyncMapping result;
  std::set<std::string> locals;
  for (const auto [logical, local] : value) {
    if (!local.is_string() ||
        (logical != kWorkspaceSyncPersonalId &&
         (!IsWorkspaceSyncUuid(logical) ||
          !IsWorkspaceSyncUuid(local.GetString()))) ||
        !locals.insert(local.GetString()).second) {
      return base::unexpected("invalid_workspace_mapping");
    }
    result.emplace(logical, local.GetString());
  }
  return result;
}

base::DictValue WorkspaceSyncMappingToValue(
    const WorkspaceSyncMapping& mapping) {
  base::DictValue result;
  for (const auto& [logical, local] : mapping) {
    result.Set(logical, local);
  }
  return result;
}

base::expected<void, std::string> ValidateWorkspaceSyncIdentityCapacity(
    const WorkspaceSyncMapping& mapping,
    const std::set<std::string>& excluded) {
  const auto personal = mapping.find(kWorkspaceSyncPersonalId);
  if (personal == mapping.end() || !personal->second.empty()) {
    return base::unexpected("invalid_workspace_mapping");
  }
  if (mapping.size() > kWorkspaceSyncMaximumLocalSpaces ||
      excluded.size() > kWorkspaceSyncMaximumRetainedLocalIds ||
      mapping.size() - 1 >
          kWorkspaceSyncMaximumRetainedLocalIds - excluded.size()) {
    return base::unexpected("too_many_local_workspaces");
  }
  for (const auto& [logical, local] : mapping) {
    if (excluded.contains(local)) {
      return base::unexpected("invalid_workspace_mapping");
    }
  }
  return base::ok();
}

base::expected<void, std::string> ValidateWorkspaceSyncApplyCapacity(
    const WorkspaceSyncMapping& mapping,
    const std::set<std::string>& excluded,
    base::span<const WorkspaceSyncSpace> observed_local,
    base::span<const WorkspaceSyncSpace> desired) {
  auto capacity = ValidateWorkspaceSyncIdentityCapacity(mapping, excluded);
  if (!capacity.has_value()) {
    return capacity;
  }
  std::set<std::string> locals;
  for (const auto& space : observed_local) {
    locals.insert(space.id);
  }
  for (const auto& [logical, local] : mapping) {
    locals.insert(local);
  }
  const size_t additions = std::ranges::count_if(
      desired, [&](const auto& space) { return !mapping.contains(space.id); });
  if (additions > kWorkspaceSyncMaximumLocalSpaces - mapping.size() ||
      additions > kWorkspaceSyncMaximumRetainedLocalIds - excluded.size() -
                      (mapping.size() - 1) ||
      locals.size() > kWorkspaceSyncMaximumLocalSpaces ||
      additions > kWorkspaceSyncMaximumLocalSpaces - locals.size()) {
    return base::unexpected("too_many_local_workspaces");
  }
  return base::ok();
}

void DetachWorkspaceSyncMappings(
    const std::set<std::string>& retained_logical_ids,
    WorkspaceSyncMapping& mapping,
    std::set<std::string>& excluded) {
  for (auto it = mapping.begin(); it != mapping.end();) {
    if (it->first != kWorkspaceSyncPersonalId &&
        !retained_logical_ids.contains(it->first)) {
      excluded.insert(it->second);
      it = mapping.erase(it);
    } else {
      ++it;
    }
  }
}

WorkspaceSyncDecision CompareWorkspaceSyncDocuments(
    const WorkspaceSyncDocument& local,
    const WorkspaceSyncDocument& remote,
    const WorkspaceSyncDocument* common) {
  if (WorkspaceSyncDocumentsEqual(local, remote)) {
    // Equality with an edit that has not reached the common state still needs
    // the durable apply intent and preference barrier before accepting it.
    return common && WorkspaceSyncDocumentsEqual(remote, *common)
               ? WorkspaceSyncDecision::kEqual
               : WorkspaceSyncDecision::kApply;
  }
  if (!common || WorkspaceSyncDocumentsEqual(local, *common)) {
    return WorkspaceSyncDecision::kApply;
  }
  if (WorkspaceSyncDocumentsEqual(remote, *common)) {
    return WorkspaceSyncDecision::kUpload;
  }
  return WorkspaceSyncDecision::kConflict;
}
}  // namespace noventra
