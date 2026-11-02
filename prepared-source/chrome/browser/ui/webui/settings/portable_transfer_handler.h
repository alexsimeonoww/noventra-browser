// Copyright 2026 The Noventra Authors. All rights reserved.
#ifndef CHROME_BROWSER_UI_WEBUI_SETTINGS_PORTABLE_TRANSFER_HANDLER_H_
#define CHROME_BROWSER_UI_WEBUI_SETTINGS_PORTABLE_TRANSFER_HANDLER_H_

#include <cstdint>
#include <memory>
#include <optional>
#include <string>

#include "base/memory/raw_ptr.h"
#include "base/memory/weak_ptr.h"
#include "base/timer/timer.h"
#include "base/types/expected.h"
#include "base/values.h"
#include "chrome/browser/ui/webui/settings/settings_page_ui_handler.h"

class Profile;
namespace content {
class WebUI;
}
namespace noventra {
class PortablePasswords;
}

namespace settings {

// Only counts and encrypted envelopes cross the trusted Settings WebUI seam.
// File and peer transfers share the same native review/apply plan.
class PortableTransferHandler final : public SettingsPageUIHandler {
 public:
  explicit PortableTransferHandler(content::WebUI* web_ui);
  ~PortableTransferHandler() override;
  void RegisterMessages() override;
  void OnJavascriptAllowed() override {}
  void OnJavascriptDisallowed() override;

 private:
  using Result = base::expected<base::DictValue, std::string>;
  void Handle(const base::ListValue& args);
  void Reply(uint64_t operation, Result result);
  void ExportCollected(uint64_t operation,
                       std::string password,
                       base::expected<std::string, std::string> csv);
  void OnOpened(uint64_t operation, Result document);
  void OnPasswordsPrepared(uint64_t operation, Result counts);
  void OnPasswordsImported(uint64_t operation, Result result);
  void ClearPlan();
  void Cancel();
  void StopPeer(bool notify = false);

  const raw_ptr<Profile> profile_;
  std::unique_ptr<noventra::PortablePasswords> passwords_;
  std::optional<base::Value> callback_;
  base::DictValue document_;
  base::DictValue review_;
  base::DictValue apply_result_;
  std::string plan_id_;
  int layout_revision_ = -1;
  uint64_t operation_ = 0;
  base::OneShotTimer expiry_;
  base::OneShotTimer peer_expiry_;
  bool peer_active_ = false;
  std::string peer_id_;
  base::WeakPtrFactory<PortableTransferHandler> weak_factory_{this};
};
}  // namespace settings
#endif
