// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef CHROME_SERVICES_NOVENTRA_UBO_PROBE_CLIENT_H_
#define CHROME_SERVICES_NOVENTRA_UBO_PROBE_CLIENT_H_
#include "base/files/file_path.h"
#include "base/memory/weak_ptr.h"
#include "base/timer/timer.h"
#include "base/values.h"
#include "chrome/browser/chrome_browser_main_extra_parts.h"
#include "chrome/services/noventra_ubo/ubo_service.mojom.h"
#include "mojo/public/cpp/bindings/remote.h"
namespace noventra_ubo {
class ProbeClient final : public ChromeBrowserMainExtraParts {
 public:
  ProbeClient();
  ~ProbeClient() override;
  void PostBrowserStart() override;
 private:
  struct Inputs;
  void Loaded(std::unique_ptr<Inputs> inputs);
  void Next();
  void Replied(const std::string& reply);
  void Finish(const std::string& error);
  void Write();
  base::FilePath output_;
  base::ListValue operations_;
  base::DictValue receipt_;
  size_t next_ = 0;
  size_t outstanding_ = 0;
  bool pipeline_ = false;
  bool failed_ = false;
  int hold_ms_ = 0;
  base::OneShotTimer hold_;
  mojo::Remote<mojom::UboService> remote_;
  base::WeakPtrFactory<ProbeClient> weak_factory_{this};
};
}
#endif
