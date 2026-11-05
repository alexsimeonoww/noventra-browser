// Copyright 2015 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "components/sessions/content/content_platform_specific_tab_data.h"

#include "content/public/browser/navigation_controller.h"
#include "content/public/browser/site_instance.h"
#include "content/public/browser/security_principal.h"
#include "content/public/browser/web_contents.h"

namespace sessions {

ContentPlatformSpecificTabData::ContentPlatformSpecificTabData(
    content::WebContents* web_contents)
    : session_storage_namespace_(nullptr) {
  const auto& config = web_contents->GetSiteInstance()->GetSecurityPrincipal()
                           .GetStoragePartitionConfig();
  if (config.is_default()) {
    session_storage_namespace_ = web_contents->GetController().GetDefaultSessionStorageNamespace();
  } else {
    const auto& namespaces = web_contents->GetController().GetSessionStorageNamespaceMap();
    auto it = namespaces.find(config);
    if (it != namespaces.end()) session_storage_namespace_ = it->second;
  }
}

ContentPlatformSpecificTabData::ContentPlatformSpecificTabData() = default;
ContentPlatformSpecificTabData::~ContentPlatformSpecificTabData() = default;

}  // namespace sessions
