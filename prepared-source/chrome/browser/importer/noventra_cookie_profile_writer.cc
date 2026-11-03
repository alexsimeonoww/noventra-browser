// Copyright 2026 Noventra contributors
// SPDX-License-Identifier: GPL-3.0-only
#include "chrome/browser/importer/noventra_cookie_profile_writer.h"

#include <set>
#include <utility>

#include "base/functional/bind.h"
#include "base/memory/ref_counted.h"
#include "base/strings/string_number_conversions.h"
#include "base/timer/timer.h"
#include "chrome/browser/profiles/profile.h"
#include "content/public/browser/storage_partition.h"
#include "mojo/public/cpp/bindings/remote.h"
#include "net/cookies/cookie_access_result.h"
#include "net/cookies/cookie_options.h"
#include "net/cookies/cookie_util.h"
#include "net/cookies/unique_cookie_key.h"
#include "services/network/public/mojom/cookie_manager.mojom.h"

namespace noventra {
namespace {
class CookieWriteOperation : public base::RefCounted<CookieWriteOperation> {
 public:
  CookieWriteOperation(
      base::WeakPtr<Profile> profile,
      std::vector<net::CanonicalCookie> cookies,
      uint32_t skipped,
      base::RepeatingCallback<bool()> can_continue,
      base::RepeatingCallback<void(uint32_t, uint32_t)> summary,
      base::OnceCallback<void(bool)> callback)
      : profile_(profile),
        cookies_(std::move(cookies)),
        skipped_(skipped),
        can_continue_(std::move(can_continue)),
        summary_(std::move(summary)),
        callback_(std::move(callback)) {}

  void Start(const content::StoragePartitionConfig& config) {
    if (!CanContinue() || profile_->IsOffTheRecord() || config.in_memory() ||
        cookies_.size() > 100) {
      Finish(false);
      return;
    }
    auto* partition = profile_->GetStoragePartition(config, true);
    if (!partition) {
      Finish(false);
      return;
    }
    partition->GetCookieManagerForBrowserProcess()->CloneInterface(
        manager_.BindNewPipeAndPassReceiver());
    manager_.set_disconnect_handler(base::BindOnce(
        &CookieWriteOperation::Finish, base::RetainedRef(this), false));
    timeout_.Start(FROM_HERE, base::Seconds(30),
                   base::BindOnce(&CookieWriteOperation::Finish,
                                  base::RetainedRef(this), false));
    manager_->GetAllCookies(base::BindOnce(&CookieWriteOperation::OnExisting,
                                           base::RetainedRef(this)));
  }

 private:
  friend class base::RefCounted<CookieWriteOperation>;
  ~CookieWriteOperation() = default;
  bool CanContinue() const { return profile_ && can_continue_.Run(); }
  void OnExisting(const std::vector<net::CanonicalCookie>& cookies) {
    if (!CanContinue()) {
      Finish(false);
      return;
    }
    for (const auto& cookie : cookies) {
      existing_.insert(cookie.LegacyUniqueKey());
    }
    Next();
  }
  void Next() {
    if (!CanContinue()) {
      Finish(false);
      return;
    }
    while (index_ < cookies_.size()) {
      const auto& cookie = cookies_[index_++];
      if (!cookie.IsCanonical() || cookie.IsPartitioned() ||
          (!cookie.ExpiryDate().is_null() &&
           cookie.ExpiryDate() <= base::Time::Now()) ||
          !existing_.insert(cookie.LegacyUniqueKey()).second) {
        ++skipped_;
        continue;
      }
      GURL source = net::cookie_util::SimulatedCookieSource(
          cookie, cookie.SourceScheme() == net::CookieSourceScheme::kSecure ||
                          cookie.IsSecure()
                      ? "https"
                      : "http");
      if (cookie.SourcePort() >= 0) {
        const std::string port = base::NumberToString(cookie.SourcePort());
        GURL::Replacements replace;
        replace.SetPortStr(port);
        source = source.ReplaceComponents(replace);
      }
      if (!source.is_valid() || !source.SchemeIsHTTPOrHTTPS()) {
        ++skipped_;
        continue;
      }
      manager_->SetCanonicalCookie(
          cookie, source, net::CookieOptions::MakeAllInclusive(),
          base::BindOnce(&CookieWriteOperation::OnWritten,
                         base::RetainedRef(this)));
      return;
    }
    manager_->FlushCookieStore(base::BindOnce(&CookieWriteOperation::Finish,
                                              base::RetainedRef(this), true));
  }
  void OnWritten(net::CookieAccessResult result) {
    if (!result.status.IsInclude()) {
      Finish(false);
      return;
    }
    ++imported_;
    Next();
  }
  void Finish(bool succeeded) {
    if (!callback_) {
      return;
    }
    scoped_refptr<CookieWriteOperation> keep_alive(this);
    timeout_.Stop();
    manager_.reset();
    if (summary_ && profile_) {
      summary_.Run(imported_, skipped_);
    }
    std::move(callback_).Run(succeeded && CanContinue());
  }
  base::WeakPtr<Profile> profile_;
  std::vector<net::CanonicalCookie> cookies_;
  size_t index_ = 0;
  uint32_t imported_ = 0;
  uint32_t skipped_;
  std::set<net::UniqueCookieKey> existing_;
  base::RepeatingCallback<bool()> can_continue_;
  base::RepeatingCallback<void(uint32_t, uint32_t)> summary_;
  base::OnceCallback<void(bool)> callback_;
  mojo::Remote<network::mojom::CookieManager> manager_;
  base::OneShotTimer timeout_;
};
}  // namespace

void WriteImportedCookies(
    base::WeakPtr<Profile> profile,
    const content::StoragePartitionConfig& config,
    const std::vector<net::CanonicalCookie>& cookies,
    uint32_t skipped,
    base::RepeatingCallback<bool()> can_continue,
    base::RepeatingCallback<void(uint32_t, uint32_t)> summary,
    base::OnceCallback<void(bool)> callback) {
  auto operation = base::MakeRefCounted<CookieWriteOperation>(
      profile, cookies, skipped, std::move(can_continue), std::move(summary),
      std::move(callback));
  operation->Start(config);
}
}  // namespace noventra
