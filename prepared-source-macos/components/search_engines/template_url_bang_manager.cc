// Copyright 2025 The Helium Authors
// You can use, redistribute, and/or modify this source code under
// the terms of the GPL-3.0 license that can be found in the LICENSE file.

#include "components/search_engines/template_url_bang_manager.h"

#include <map>
#include <utility>

#include "base/functional/bind.h"
#include "base/json/json_reader.h"
#include "base/no_destructor.h"
#include "base/task/thread_pool.h"
#include "components/helium_services/helium_services_helpers.h"
#include "net/http/http_status_code.h"
#include "services/network/public/cpp/resource_request.h"
#include "services/network/public/cpp/simple_url_loader.h"
#include "services/network/public/mojom/url_response_head.mojom.h"
#include "url/gurl.h"

namespace Bangs {
namespace {

constexpr net::BackoffEntry::Policy kRetryPolicy = {
    0,       // Don't ignore initial errors.
    5000,    // Start with a five-second delay.
    2.0,     // Double the delay after each failure.
    0.2,     // Jitter retries across clients.
    300000,  // Cap delays at five minutes.
    -1,      // Never discard failure history.
    false,
};

constexpr net::NetworkTrafficAnnotationTag kCustomizationDocumentNetworkTag =
    net::DefineNetworkTrafficAnnotation("native_bangs",
                                        R"(
        semantics {
            sender: "Native bang loader"
            description:
            "Get data for native bangs."
            trigger:
            "Triggered when the browser is opened. "
            data: "None."
            destination: GOOGLE_OWNED_SERVICE
        }
        policy {
            cookies_allowed: NO
        })");

BangManager::Result ParseManifest(const std::string& manifest) {
  auto parsed = base::JSONReader::ReadAndReturnValueWithError(
      manifest, base::JSON_ALLOW_COMMENTS | base::JSON_ALLOW_TRAILING_COMMAS);
  if (!parsed.has_value()) {
    LOG(ERROR) << parsed.error().message;
    return nullptr;
  }
  if (!parsed->is_list()) {
    LOG(ERROR) << "received invalid bang data - not an array";
    return nullptr;
  }

  std::vector<Bang> bangs;
  for (const auto& value : parsed->GetList()) {
    const auto* dict = value.GetIfDict();
    if (!dict) {
      continue;
    }
    const auto* name = dict->FindString("s");
    const auto* triggers = dict->FindList("ts");
    const auto* url = dict->FindString("u");
    if (!name || name->empty() || !triggers || !url) {
      continue;
    }
    const GURL parsed_url(*url);
    if (!parsed_url.is_valid() || !parsed_url.SchemeIsHTTPOrHTTPS()) {
      continue;
    }
    const auto* category = dict->FindString("sc");
    for (const auto& trigger : *triggers) {
      if (!trigger.is_string() || trigger.GetString().empty()) {
        continue;
      }
      bangs.push_back({bangs.size(), *name, trigger.GetString(), *url,
                       category && *category == "ai" ? BANG_CATEGORY_AI
                                                     : BANG_CATEGORY_OTHER});
    }
  }
  return base::MakeRefCounted<Catalogue>(std::move(bangs));
}

class BangCache {
 public:
  using Completion = base::OnceCallback<void(const BangManager::Result&)>;

  static BangCache& GetInstance() {
    static base::NoDestructor<BangCache> cache;
    return *cache;
  }

  void Load(const GURL& url,
            network::SharedURLLoaderFactory* factory,
            Completion callback) {
    auto& entry = entries_[url];
    if (entry.bangs) {
      std::move(callback).Run(entry.bangs);
      return;
    }
    entry.callbacks.push_back(std::move(callback));
    if (entry.loader) {
      return;
    }

    auto request = std::make_unique<network::ResourceRequest>();
    request->url = url;
    request->credentials_mode = network::mojom::CredentialsMode::kOmit;
    request->headers.SetHeader("Accept", "application/json");
    entry.loader = network::SimpleURLLoader::Create(
        std::move(request), kCustomizationDocumentNetworkTag);
    entry.loader->SetTimeoutDuration(base::Seconds(30));
    entry.loader->DownloadToString(
        factory,
        base::BindOnce(&BangCache::OnComplete, base::Unretained(this), url),
        5'000'000 /* 5 MB */);
  }

 private:
  struct Entry {
    BangManager::Result bangs;
    std::unique_ptr<network::SimpleURLLoader> loader;
    std::vector<Completion> callbacks;
  };

  void OnComplete(const GURL& url, std::optional<std::string> response_body) {
    auto& entry = entries_.at(url);
    int response_code = -1;
    std::string mime_type;
    if (entry.loader->ResponseInfo() && entry.loader->ResponseInfo()->headers) {
      response_code = entry.loader->ResponseInfo()->headers->response_code();
      entry.loader->ResponseInfo()->headers->GetMimeType(&mime_type);
    }
    if (response_body && response_code == net::HTTP_OK &&
        mime_type == "application/json") {
      base::ThreadPool::PostTaskAndReplyWithResult(
          FROM_HERE, {base::TaskPriority::USER_VISIBLE},
          base::BindOnce(&ParseManifest, std::move(*response_body)),
          base::BindOnce(&BangCache::OnParsed, base::Unretained(this), url));
      return;
    } else {
      LOG(ERROR) << "Fetching bangs failed: response code = " << response_code;
    }

    OnParsed(url, nullptr);
  }

  void OnParsed(const GURL& url, BangManager::Result bangs) {
    auto& entry = entries_.at(url);
    entry.bangs = std::move(bangs);
    entry.loader.reset();
    auto callbacks = std::move(entry.callbacks);
    for (auto& callback : callbacks) {
      std::move(callback).Run(entry.bangs);
    }
  }

  std::map<GURL, Entry> entries_;
};

}  // namespace

Catalogue::Catalogue(std::vector<Bang> entries) : entries(std::move(entries)) {}
Catalogue::~Catalogue() = default;

BangManager::BangManager(
    PrefService& prefs,
    scoped_refptr<network::SharedURLLoaderFactory> url_loader_factory)
    : prefs_(prefs),
      url_loader_factory_(std::move(url_loader_factory)),
      backoff_(&kRetryPolicy) {}

BangManager::~BangManager() = default;

void BangManager::LoadBangs(Callback callback) {
  retry_timer_.Stop();
  weak_ptr_factory_.InvalidateWeakPtrs();
  backoff_.Reset();
  callback_ = std::move(callback);

  if (!helium::ShouldFetchBangs(*prefs_) ||
      helium::GetServicesBaseURL(*prefs_) == helium::GetDummyURL()) {
    OnBangsLoaded(base::MakeRefCounted<Catalogue>(std::vector<Bang>()));
    return;
  }

  StartFetch();
}

void BangManager::StartFetch() {
  BangCache::GetInstance().Load(
      helium::GetServicesBaseURL(*prefs_).Resolve("/bangs.json"),
      url_loader_factory_.get(),
      base::BindOnce(&BangManager::OnBangsLoaded,
                     weak_ptr_factory_.GetWeakPtr()));
}

void BangManager::OnBangsLoaded(const Result& result) {
  backoff_.InformOfRequest(!!result);
  if (!result && helium::ShouldFetchBangs(*prefs_)) {
    retry_timer_.Start(FROM_HERE, backoff_.GetTimeUntilRelease(), this,
                       &BangManager::StartFetch);
  }
  // Update retry state before notifying callers, which may reload.
  auto callback = callback_;
  callback.Run(result);
}

}  // namespace Bangs
