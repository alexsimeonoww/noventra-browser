// Copyright 2025 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "components/browser_apis/tab_strip/types/split_tab_visual_data_mojom_traits.h"

MojoSplitLayout mojo::EnumTraits<MojoSplitLayout, NativeSplitLayout>::ToMojom(
    NativeSplitLayout input) {
  switch (input) {
    case NativeSplitLayout::kSideBySide:
      return MojoSplitLayout::kSideBySide;
    case NativeSplitLayout::kStacked:
      return MojoSplitLayout::kStacked;
    case NativeSplitLayout::kGrid:
      return MojoSplitLayout::kGrid;
  }
  NOTREACHED();
}

NativeSplitLayout
mojo::EnumTraits<MojoSplitLayout, NativeSplitLayout>::FromMojom(
    MojoSplitLayout input) {
  switch (input) {
    case MojoSplitLayout::kSideBySide:
      return NativeSplitLayout::kSideBySide;
    case MojoSplitLayout::kStacked:
      return NativeSplitLayout::kStacked;
    case MojoSplitLayout::kGrid:
      return NativeSplitLayout::kGrid;
  }
  NOTREACHED();
}

NativeSplitLayout mojo::StructTraits<
    MojoSplitTabVisualDataView,
    NativeSplitTabVisualData>::layout(const NativeSplitTabVisualData& native) {
  return native.split_layout();
}

double
mojo::StructTraits<MojoSplitTabVisualDataView, NativeSplitTabVisualData>::
    split_ratio(const NativeSplitTabVisualData& native) {
  return native.split_ratio();
}

double
mojo::StructTraits<MojoSplitTabVisualDataView, NativeSplitTabVisualData>::
    grid_row_ratio(const NativeSplitTabVisualData& native) {
  return native.grid_row_ratio();
}

bool mojo::StructTraits<MojoSplitTabVisualDataView, NativeSplitTabVisualData>::
    Read(MojoSplitTabVisualDataView view, NativeSplitTabVisualData* out) {
  NativeSplitLayout layout;
  if (!view.ReadLayout(&layout)) {
    return false;
  }

  if (!(view.split_ratio() >= 0.0 && view.split_ratio() <= 1.0) ||
      !(view.grid_row_ratio() >= 0.0 && view.grid_row_ratio() <= 1.0)) {
    return false;
  }
  *out = NativeSplitTabVisualData(layout, view.split_ratio());
  out->set_grid_row_ratio(view.grid_row_ratio());
  return true;
}
