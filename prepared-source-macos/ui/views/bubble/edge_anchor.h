// Copyright 2026 The Noventra Authors
// SPDX-License-Identifier: GPL-3.0-only
#ifndef UI_VIEWS_BUBBLE_EDGE_ANCHOR_H_
#define UI_VIEWS_BUBBLE_EDGE_ANCHOR_H_
#include "ui/base/class_property.h"
#include "ui/views/views_export.h"
namespace views {
// Opt-in for movable edge controls: corner bubbles open toward the window
// interior, including Linux where offscreen bubble adjustment is disabled.
VIEWS_EXPORT extern const ui::ClassProperty<bool>* const kBubbleOpensInsideWindow;
}
#endif
