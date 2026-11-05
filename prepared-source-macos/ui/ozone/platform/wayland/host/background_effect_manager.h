// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef UI_OZONE_PLATFORM_WAYLAND_HOST_BACKGROUND_EFFECT_MANAGER_H_
#define UI_OZONE_PLATFORM_WAYLAND_HOST_BACKGROUND_EFFECT_MANAGER_H_

#include "base/memory/raw_ptr.h"
#include "ui/ozone/platform/wayland/common/wayland_object.h"

namespace ui {

// Tracks the live capabilities of ext-background-effect-v1. Advertising the
// global alone is insufficient: blur is usable only after its capabilities
// event, and can subsequently be withdrawn by the compositor.
class BackgroundEffectManager
    : public wl::GlobalObjectRegistrar<BackgroundEffectManager> {
 public:
  static constexpr char kInterfaceName[] = "ext_background_effect_manager_v1";

  static void Instantiate(WaylandConnection* connection,
                          wl_registry* registry,
                          uint32_t name,
                          const std::string& interface,
                          uint32_t version);
  static void RemoveGlobal(WaylandConnection* connection,
                           wl_registry* registry,
                           uint32_t name);

  BackgroundEffectManager(wl::Object<ext_background_effect_manager_v1> manager,
                          WaylandConnection* connection,
                          uint32_t global_name);
  BackgroundEffectManager(const BackgroundEffectManager&) = delete;
  BackgroundEffectManager& operator=(const BackgroundEffectManager&) = delete;
  ~BackgroundEffectManager();

  uint32_t global_name() const { return global_name_; }
  bool supports_blur() const { return supports_blur_; }
  wl::Object<ext_background_effect_surface_v1> CreateSurface(
      wl_surface* surface);

 private:
  static void BindAvailable(WaylandConnection* connection,
                            wl_registry* registry);
  static void OnCapabilities(void* data,
                             ext_background_effect_manager_v1* manager,
                             uint32_t flags);

  wl::Object<ext_background_effect_manager_v1> manager_;
  const raw_ptr<WaylandConnection> connection_;
  const uint32_t global_name_;
  bool supports_blur_ = false;
};

}  // namespace ui

#endif  // UI_OZONE_PLATFORM_WAYLAND_HOST_BACKGROUND_EFFECT_MANAGER_H_
