// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef CHROME_SERVICES_NOVENTRA_UBO_SERVICE_H_
#define CHROME_SERVICES_NOVENTRA_UBO_SERVICE_H_
#include <memory>
#include "base/memory/weak_ptr.h"
#include "base/timer/timer.h"
#include "chrome/services/noventra_ubo/ubo_service.mojom.h"
#include "mojo/public/cpp/bindings/receiver.h"
namespace noventra_ubo {
class Runtime;
class UboService final : public mojom::UboService {
 public:
  explicit UboService(mojo::PendingReceiver<mojom::UboService> receiver);
  ~UboService() override;
  void Initialize(std::vector<mojom::AssetPtr> assets, InitializeCallback callback) override;
  void Run(mojom::Operation operation, const std::string& payload, RunCallback callback) override;
 private:
  void Pump();
  std::unique_ptr<Runtime> runtime_;
  RunCallback pending_;
  base::OneShotTimer timer_;
  mojo::Receiver<mojom::UboService> receiver_;
  base::WeakPtrFactory<UboService> weak_factory_{this};
};
}  // namespace noventra_ubo
#endif
