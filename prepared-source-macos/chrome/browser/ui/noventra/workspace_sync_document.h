// Copyright 2026 The Noventra Authors. All rights reserved.
#ifndef CHROME_BROWSER_UI_NOVENTRA_WORKSPACE_SYNC_DOCUMENT_H_
#define CHROME_BROWSER_UI_NOVENTRA_WORKSPACE_SYNC_DOCUMENT_H_

#include <map>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "base/containers/span.h"
#include "base/types/expected.h"
#include "base/values.h"
#include "chrome/browser/ui/noventra/layout_model.h"

namespace noventra {

inline constexpr char kWorkspaceSyncPersonalId[] = "personal";
inline constexpr size_t kWorkspaceSyncMaximumSpaces = 256;
inline constexpr size_t kWorkspaceSyncMaximumLocalSpaces = 1024;
// Each non-Personal mapping reserves a future permanent exclusion slot.
inline constexpr size_t kWorkspaceSyncMaximumRetainedLocalIds = 1024;
inline constexpr size_t kWorkspaceSyncMaximumDocumentBytes = 1024 * 1024;

struct WorkspaceSyncSpace {
  // Logical UUID in a remote document; local partition UUID in a UI snapshot.
  // Personal is "personal" remotely and the empty ID locally.
  std::string id;
  std::string name;
  bool archived = false;
  bool operator==(const WorkspaceSyncSpace&) const = default;
};

struct WorkspaceSyncDocument {
  // The list is the order. Revision is always zero on the wire.
  std::vector<WorkspaceSyncSpace> spaces;
  BrowserLayout layout;
};

using WorkspaceSyncMapping = std::map<std::string, std::string>;

bool IsWorkspaceSyncUuid(std::string_view value);
base::DictValue WorkspaceSyncDocumentToValue(
    const WorkspaceSyncDocument& document);
base::expected<WorkspaceSyncDocument, std::string> ParseWorkspaceSyncDocument(
    std::string_view bytes,
    base::span<const LayoutCapability> capabilities);
base::expected<WorkspaceSyncDocument, std::string>
ValidateWorkspaceSyncDocument(const base::DictValue& value,
                              base::span<const LayoutCapability> capabilities);
std::string SerializeWorkspaceSyncDocument(
    const WorkspaceSyncDocument& document);
bool WorkspaceSyncDocumentsEqual(const WorkspaceSyncDocument& a,
                                 const WorkspaceSyncDocument& b);
bool WorkspaceSyncLayoutsEqual(const BrowserLayout& a, const BrowserLayout& b);

// Rejects non-bijective or partition-changing maps. Personal is reserved.
base::expected<WorkspaceSyncMapping, std::string> ParseWorkspaceSyncMapping(
    const base::DictValue& value);
base::DictValue WorkspaceSyncMappingToValue(
    const WorkspaceSyncMapping& mapping);

// Given a structurally validated mapping/exclusion set, admit only identities
// that can all be detached later without exceeding persistent exclusion limits.
// Personal is checked before subtraction and consumes no exclusion slot.
base::expected<void, std::string> ValidateWorkspaceSyncIdentityCapacity(
    const WorkspaceSyncMapping& mapping,
    const std::set<std::string>& excluded);
// Preflight all incoming logical IDs together, counting dormant exclusions,
// old mappings that will detach, and the existing observed-space bound.
base::expected<void, std::string> ValidateWorkspaceSyncApplyCapacity(
    const WorkspaceSyncMapping& mapping,
    const std::set<std::string>& excluded,
    base::span<const WorkspaceSyncSpace> observed_local,
    base::span<const WorkspaceSyncSpace> desired);
// Transfer reservations to permanent exclusions after the durable apply
// barrier. This cannot grow the admitted identity footprint and never prunes
// exclusions.
void DetachWorkspaceSyncMappings(
    const std::set<std::string>& retained_logical_ids,
    WorkspaceSyncMapping& mapping,
    std::set<std::string>& excluded);

// kEqual is the durable fast path: local, remote and common all agree. Equal
// new edits still select kApply so the preference barrier precedes new common.
enum class WorkspaceSyncDecision { kEqual, kUpload, kApply, kConflict };
// An absent common state is joining, not permission to overwrite the remote.
WorkspaceSyncDecision CompareWorkspaceSyncDocuments(
    const WorkspaceSyncDocument& local,
    const WorkspaceSyncDocument& remote,
    const WorkspaceSyncDocument* common);

}  // namespace noventra
#endif  // CHROME_BROWSER_UI_NOVENTRA_WORKSPACE_SYNC_DOCUMENT_H_
