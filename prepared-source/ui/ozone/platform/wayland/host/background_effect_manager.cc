// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "ui/ozone/platform/wayland/host/background_effect_manager.h"

#include <ext-background-effect-v1-client-protocol.h>

#include <memory>
#include <utility>

#include "base/check_op.h"
#include "base/logging.h"
#include "ui/ozone/platform/wayland/host/wayland_connection.h"

namespace ui {

// static
void BackgroundEffectManager::Instantiate(WaylandConnection* connection,
                                          wl_registry* registry,
                                          uint32_t name,
                                          const std::string& interface,
                                          uint32_t version) {
  CHECK_EQ(interface, kInterfaceName);
  connection->background_effect_globals_.insert_or_assign(name, version);
  BindAvailable(connection, registry);
}

// static
void BackgroundEffectManager::RemoveGlobal(WaylandConnection* connection,
                                           wl_registry* registry,
                                           uint32_t name) {
  connection->background_effect_globals_.erase(name);
  if (!connection->background_effect_manager_ ||
      connection->background_effect_manager_->global_name() != name) {
    return;
  }
  connection->background_effect_manager_.reset();
  // Destroy existing per-surface effects before binding a replacement. The
  // replacement stays unsupported until its own capabilities event arrives.
  connection->OnBackgroundBlurCapabilitiesChanged();
  BindAvailable(connection, registry);
}

// static
void BackgroundEffectManager::BindAvailable(WaylandConnection* connection,
                                            wl_registry* registry) {
  if (connection->background_effect_manager_) {
    return;
  }
  // Binding failures are retried only on registry changes, never on repaint.
  for (const auto& [name, version] : connection->background_effect_globals_) {
    if (!wl::CanBind(kInterfaceName, version, 1, 1)) {
      continue;
    }
    auto manager =
        wl::Bind<ext_background_effect_manager_v1>(registry, name, 1);
    if (!manager) {
      LOG(ERROR) << "Failed to bind " << kInterfaceName;
      continue;
    }
    auto instance = std::make_unique<BackgroundEffectManager>(
        std::move(manager), connection, name);
    static constexpr ext_background_effect_manager_v1_listener kListener = {
        .capabilities = &OnCapabilities,
    };
    if (ext_background_effect_manager_v1_add_listener(
            instance->manager_.get(), &kListener, instance.get()) < 0) {
      LOG(ERROR) << "Failed to listen to " << kInterfaceName;
      continue;
    }
    connection->background_effect_manager_ = std::move(instance);
    return;
  }
}

BackgroundEffectManager::BackgroundEffectManager(
    wl::Object<ext_background_effect_manager_v1> manager,
    WaylandConnection* connection,
    uint32_t global_name)
    : manager_(std::move(manager)),
      connection_(connection),
      global_name_(global_name) {}

BackgroundEffectManager::~BackgroundEffectManager() = default;

wl::Object<ext_background_effect_surface_v1>
BackgroundEffectManager::CreateSurface(wl_surface* surface) {
  if (!supports_blur_) {
    return {};
  }
  return wl::Object<ext_background_effect_surface_v1>(
      ext_background_effect_manager_v1_get_background_effect(manager_.get(),
                                                             surface));
}

// static
void BackgroundEffectManager::OnCapabilities(
    void* data,
    ext_background_effect_manager_v1* manager,
    uint32_t flags) {
  auto* self = static_cast<BackgroundEffectManager*>(data);
  const bool supports_blur =
      flags & EXT_BACKGROUND_EFFECT_MANAGER_V1_CAPABILITY_BLUR;
  if (self->supports_blur_ == supports_blur) {
    return;
  }
  self->supports_blur_ = supports_blur;
  self->connection_->OnBackgroundBlurCapabilitiesChanged();
}

}  // namespace ui
