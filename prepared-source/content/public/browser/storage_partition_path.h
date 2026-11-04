// Copyright 2026 The Noventra Authors. All rights reserved.
#ifndef CONTENT_PUBLIC_BROWSER_STORAGE_PARTITION_PATH_H_
#define CONTENT_PUBLIC_BROWSER_STORAGE_PARTITION_PATH_H_

#include <string>

#include "base/files/file_path.h"
#include "content/common/content_export.h"

namespace content {

// Returns the engine-owned partition path relative to its Profile directory.
// May run on any sequence, including cold migration before a context exists.
CONTENT_EXPORT base::FilePath GetStoragePartitionRelativePath(
    const std::string& partition_domain,
    const std::string& partition_name);

}  // namespace content

#endif  // CONTENT_PUBLIC_BROWSER_STORAGE_PARTITION_PATH_H_
