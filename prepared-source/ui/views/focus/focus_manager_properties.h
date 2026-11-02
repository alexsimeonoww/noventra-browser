// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef UI_VIEWS_FOCUS_FOCUS_MANAGER_PROPERTIES_H_
#define UI_VIEWS_FOCUS_FOCUS_MANAGER_PROPERTIES_H_

#include "ui/views/views_export.h"

namespace views {

// A non-null Widget native property suppresses only that widget's parent
// accelerator fallback. Use for an independent focus owner with a native
// embedding parent. Its own accelerators and delegate still run first;
// unhandled keys remain unhandled. The property value is not owned.
VIEWS_EXPORT extern const char kSkipParentAcceleratorFallbackKey[];

}  // namespace views

#endif  // UI_VIEWS_FOCUS_FOCUS_MANAGER_PROPERTIES_H_
