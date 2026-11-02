// Copyright 2026 The Noventra Authors. All rights reserved.
#include "chrome/browser/ui/noventra/workspace_sync_transport.h"

#include <algorithm>
#include <atomic>
#include <utility>

#include "base/base64url.h"
#include "base/functional/bind.h"
#include "base/strings/string_util.h"
#include "base/synchronization/lock.h"
#include "base/synchronization/waitable_event.h"
#include "chrome/browser/ui/noventra/workspace_sync_document.h"
#include "components/sync/engine/cancelation_signal.h"
#include "components/noventra_sync_vault/sync_vault_limits.h"
#include "net/base/load_flags.h"
#include "net/base/net_errors.h"
#include "net/http/http_response_headers.h"
#include "net/traffic_annotation/network_traffic_annotation.h"
#include "services/network/public/cpp/resource_request.h"
#include "services/network/public/cpp/simple_url_loader.h"
#include "services/network/public/cpp/simple_url_loader_stream_consumer.h"
#include "services/network/public/mojom/url_response_head.mojom.h"

namespace noventra {
namespace {
using Error = noventra_sync::SyncVaultTransportError;
constexpr auto kRequestTimeout = base::Seconds(30);

Error HttpError(int code) {
  switch (code) {
    case 401:
      return Error::kAuthenticationRequired;
    case 403:
      return Error::kAccessDenied;
    case 412:
      return Error::kConflict;
    case 413:
      return Error::kQuotaExceeded;
    case 429:
      return Error::kTemporaryError;
    default:
      return code >= 500 ? Error::kTemporaryError : Error::kInvalidData;
  }
}
}  // namespace

base::expected<GURL, std::string> ValidateWorkspaceSyncOrigin(
    const std::string& origin) {
  const GURL url(origin);
  // Also reject normalizations that can hide userinfo, paths or controls.
  if (origin.size() > 2048 ||
      origin.find_first_of("\\@") != std::string::npos ||
      std::ranges::any_of(
          origin, [](unsigned char c) { return c <= 0x20 || c >= 0x7f; }) ||
      !url.is_valid() || !url.has_host() || url.has_username() ||
      url.has_password() || url.path() != "/" || url.has_query() ||
      url.has_ref() ||
      !(url.SchemeIs("https") ||
        (url.SchemeIs("http") &&
         (url.host() == "localhost" || url.host() == "127.0.0.1" ||
          url.host() == "[::1]")))) {
    return base::unexpected("invalid_sync_origin");
  }
  // Require an origin spelling, not a URL whose dot segments normalize to /.
  const size_t authority = origin.find("://");
  const size_t slash = authority == std::string::npos
                           ? std::string::npos
                           : origin.find('/', authority + 3);
  if (authority == std::string::npos ||
      (slash != std::string::npos && slash != origin.size() - 1)) {
    return base::unexpected("invalid_sync_origin");
  }
  // Insecure HTTP must spell the loopback host explicitly (no 127.1, decimal
  // address, alternate IPv6 form, percent escapes or trailing-dot alias).
  if (url.SchemeIs("http")) {
    std::string_view host_port(origin);
    host_port.remove_prefix(authority + 3);
    if (host_port.ends_with('/')) {
      host_port.remove_suffix(1);
    }
    const auto end = host_port.starts_with('[') ? host_port.find(']') + 1
                                                : host_port.find(':');
    const auto literal = host_port.substr(0, end);
    if (literal != "localhost" && literal != "127.0.0.1" &&
        literal != "[::1]") {
      return base::unexpected("invalid_sync_origin");
    }
  }
  return url;
}

bool IsWorkspaceSyncToken(std::string_view token) {
  return token.size() >= 32 && token.size() <= 1024 &&
         std::ranges::all_of(
             token, [](unsigned char c) { return c >= 0x21 && c <= 0x7e; }) &&
         token.front() != ' ' && token.back() != ' ';
}

bool IsWorkspaceSyncStrongETag(std::string_view value) {
  if (value.size() < 3 || value.size() > 4096 || value.front() != '"' ||
      value.back() != '"') {
    return false;
  }
  value.remove_prefix(1);
  value.remove_suffix(1);
  return std::ranges::all_of(value, [](unsigned char c) {
    return c == 0x21 || (c >= 0x23 && c <= 0x7e);
  });
}

base::expected<GURL, std::string> WorkspaceSyncObjectURL(
    const GURL& origin,
    const std::string& vault_id,
    const std::string& object_id) {
  if (!ValidateWorkspaceSyncOrigin(origin.spec()).has_value() ||
      !IsWorkspaceSyncUuid(vault_id) ||
      !noventra_sync::IsValidSyncVaultObjectId(object_id) ||
      !base::IsStringUTF8(object_id)) {
    return base::unexpected("invalid_sync_object");
  }
  std::string encoded;
  base::Base64UrlEncode(object_id, base::Base64UrlEncodePolicy::OMIT_PADDING,
                        &encoded);
  return origin.Resolve("/v1/vaults/" + vault_id + "/objects/" + encoded);
}

// Results are protected by a lock, completion is a one-shot event. Loaders and
// their callbacks exist only on UI. Cancellation wakes the worker immediately;
// the retained request then destroys the loader on UI. No callbacks capture a
// service, profile or stack variable across sequences.
class WorkspaceSyncRequest
    : public base::RefCountedDeleteOnSequence<WorkspaceSyncRequest>,
      public syncer::CancelationSignal::Observer,
      public network::SimpleURLLoaderStreamConsumer {
 public:
  WorkspaceSyncRequest(scoped_refptr<base::SequencedTaskRunner> runner,
                       std::unique_ptr<network::ResourceRequest> request,
                       std::vector<uint8_t> data,
                       size_t limit,
                       base::TimeDelta timeout)
      : base::RefCountedDeleteOnSequence<WorkspaceSyncRequest>(runner),
        runner_(std::move(runner)),
        request_(std::move(request)),
        upload_(std::move(data)),
        limit_(limit),
        timeout_(timeout) {}

  void Start(network::SharedURLLoaderFactory* factory) {
    DCHECK(runner_->RunsTasksInCurrentSequence());
    if (aborted_.load()) {
      return;
    }
    constexpr auto annotation = net::DefineNetworkTrafficAnnotation(
        "noventra_workspace_settings_sync", R"(
        semantics {
          sender: "Noventra workspace settings sync"
          description: "Synchronizes encrypted workspace names, order, archive "
                       "flags and saved dock layout with a user-selected store."
          trigger: "User configures a vault, changes saved settings, requests "
                   "sync, or a 15 second poll runs while configured."
          data: "Authenticated encrypted objects, opaque vault/object IDs, "
                "conditional revisions and a user-supplied bearer token."
          destination: OTHER
          destination_other: "The store origin explicitly selected by the user."
        }
        policy {
          cookies_allowed: NO
          setting: "Disabled until explicitly configured in Settings. "
                   "Disconnect cancels requests and removes configuration."
          policy_exception_justification: "Not enabled by default."
        })");
    const bool write = request_->method == "PUT";
    loader_ = network::SimpleURLLoader::Create(std::move(request_), annotation);
    loader_->SetTimeoutDuration(timeout_);
    loader_->SetAllowHttpErrorResults(true);
    if (write) {
      loader_->AttachStringForUpload(
          std::string(upload_.begin(), upload_.end()),
          "application/octet-stream");
    }
    upload_.clear();
    loader_->DownloadAsStream(factory, this);
  }

  void OnDataReceived(std::string_view data,
                      base::OnceClosure resume) override {
    if (data.size() > limit_ - body_.size()) {
      // ResponseInfo()/NetError() are only legal after SimpleURLLoader has
      // finished. Overflow aborts an unfinished stream without inspecting it.
      scoped_refptr<WorkspaceSyncRequest> keep_alive(this);
      {
        base::AutoLock hold(lock_);
        result_.net_error = net::ERR_INSUFFICIENT_RESOURCES;
      }
      loader_.reset();
      body_.clear();
      done_.Signal();
      return;
    }
    body_.append(data);
    std::move(resume).Run();
  }
  void OnComplete(bool success) override { Complete(success); }
  void OnRetry(base::OnceClosure start_retry) override {
    body_.clear();
    std::move(start_retry).Run();
  }
  void OnCancelationSignalReceived() override { Cancel(); }
  void Cancel() {
    aborted_.store(true);
    done_.Signal();
    runner_->PostTask(
        FROM_HERE, base::BindOnce(&WorkspaceSyncRequest::CancelOnUI,
                                  scoped_refptr<WorkspaceSyncRequest>(this)));
  }
  bool Wait() { return done_.TimedWait(timeout_); }
  bool aborted() const { return aborted_.load(); }

  struct Result {
    int code = 0;
    int net_error = net::ERR_FAILED;
    std::string etag;
    std::optional<std::string> body;
  };
  Result TakeResult() {
    base::AutoLock hold(lock_);
    return std::move(result_);
  }

 private:
  friend class base::RefCountedDeleteOnSequence<WorkspaceSyncRequest>;
  friend class base::DeleteHelper<WorkspaceSyncRequest>;
  ~WorkspaceSyncRequest() = default;

  void CancelOnUI() { loader_.reset(); }
  void Complete(bool success) {
    scoped_refptr<WorkspaceSyncRequest> keep_alive(this);
    DCHECK(runner_->RunsTasksInCurrentSequence());
    {
      base::AutoLock hold(lock_);
      result_.net_error = success ? loader_->NetError() : net::ERR_FAILED;
      if (loader_->ResponseInfo() && loader_->ResponseInfo()->headers) {
        const auto& headers = loader_->ResponseInfo()->headers;
        result_.code = headers->response_code();
        result_.etag = headers->GetNormalizedHeader("ETag").value_or("");
      }
      if (success) {
        result_.body = std::move(body_);
      }
    }
    loader_.reset();
    done_.Signal();
  }

  const scoped_refptr<base::SequencedTaskRunner> runner_;
  std::unique_ptr<network::ResourceRequest> request_;
  std::vector<uint8_t> upload_;
  const size_t limit_;
  const base::TimeDelta timeout_;
  std::unique_ptr<network::SimpleURLLoader> loader_;
  std::string body_;
  std::atomic_bool aborted_{false};
  base::WaitableEvent done_{base::WaitableEvent::ResetPolicy::MANUAL,
                            base::WaitableEvent::InitialState::NOT_SIGNALED};
  base::Lock lock_;
  Result result_ GUARDED_BY(lock_);
};

WorkspaceSyncNetwork::WorkspaceSyncNetwork(
    scoped_refptr<network::SharedURLLoaderFactory> factory)
    : base::RefCountedDeleteOnSequence<WorkspaceSyncNetwork>(
          base::SequencedTaskRunner::GetCurrentDefault()),
      task_runner_(base::SequencedTaskRunner::GetCurrentDefault()),
      factory_(std::move(factory)) {}
WorkspaceSyncNetwork::~WorkspaceSyncNetwork() = default;
void WorkspaceSyncNetwork::Start(scoped_refptr<WorkspaceSyncRequest> request) {
  request->Start(factory_.get());
}

WorkspaceSyncTransport::WorkspaceSyncTransport(
    GURL origin,
    std::string vault_id,
    std::string token,
    scoped_refptr<WorkspaceSyncNetwork> network,
    base::TimeTicks deadline)
    : origin_(std::move(origin)),
      vault_id_(std::move(vault_id)),
      token_(std::move(token)),
      network_(std::move(network)),
      deadline_(deadline) {}
WorkspaceSyncTransport::~WorkspaceSyncTransport() = default;
void WorkspaceSyncTransport::BeginOperation(base::TimeTicks deadline) {
  deadline_ = deadline;
  requests_remaining_ = kMaximumRequestsPerOperation;
  bytes_remaining_ = kMaximumBytesPerOperation;
}
void WorkspaceSyncTransport::SetCancelationSignal(
    syncer::CancelationSignal* signal) {
  signal_ = signal;
}

base::expected<WorkspaceSyncTransport::Response, Error>
WorkspaceSyncTransport::Request(const std::string& object_id,
                                bool write,
                                std::optional<std::string> revision,
                                std::vector<uint8_t> data,
                                size_t maximum_bytes) {
  DCHECK(!network_->task_runner()->RunsTasksInCurrentSequence());
  if (signal_ && signal_->IsSignalled()) {
    return base::unexpected(Error::kAborted);
  }
  if (base::TimeTicks::Now() >= deadline_ || requests_remaining_ == 0) {
    return base::unexpected(Error::kTemporaryError);
  }
  if (data.size() >= bytes_remaining_) {
    return base::unexpected(Error::kTemporaryError);
  }
  bytes_remaining_ -= data.size();
  maximum_bytes = std::min(maximum_bytes, bytes_remaining_);
  --requests_remaining_;
  auto url = WorkspaceSyncObjectURL(origin_, vault_id_, object_id);
  if (!url.has_value() || !IsWorkspaceSyncToken(token_) ||
      (revision && !IsWorkspaceSyncStrongETag(*revision)) ||
      data.size() > noventra_sync::kSyncVaultMaximumEncryptedObjectBytes ||
      maximum_bytes > noventra_sync::kSyncVaultMaximumEncryptedObjectBytes) {
    return base::unexpected(Error::kInvalidData);
  }
  auto resource = std::make_unique<network::ResourceRequest>();
  resource->url = *url;
  resource->method = write ? "PUT" : "GET";
  resource->credentials_mode = network::mojom::CredentialsMode::kOmit;
  resource->redirect_mode = network::mojom::RedirectMode::kError;
  resource->do_not_prompt_for_login = true;
  resource->load_flags = net::LOAD_DISABLE_CACHE | net::LOAD_BYPASS_CACHE;
  if (origin_.SchemeIs("http")) {
    // A proxy must not carry a loopback-only plaintext bearer token elsewhere.
    resource->load_flags |= net::LOAD_BYPASS_PROXY;
  }
  resource->headers.SetHeader("Authorization", "Bearer " + token_);
  resource->headers.SetHeader("Accept", "application/octet-stream");
  if (write) {
    resource->headers.SetHeader(revision ? "If-Match" : "If-None-Match",
                                revision.value_or("*"));
  }
  const auto timeout =
      std::min(kRequestTimeout, deadline_ - base::TimeTicks::Now());
  if (timeout <= base::TimeDelta()) {
    return base::unexpected(Error::kTemporaryError);
  }
  auto request = base::MakeRefCounted<WorkspaceSyncRequest>(
      network_->task_runner(), std::move(resource), std::move(data),
      maximum_bytes, timeout);
  if (signal_ && !signal_->TryRegisterHandler(request.get())) {
    return base::unexpected(Error::kAborted);
  }
  const bool posted = network_->task_runner()->PostTask(
      FROM_HERE,
      base::BindOnce(&WorkspaceSyncNetwork::Start, network_, request));
  const bool completed = posted && request->Wait();
  if (!completed) {
    request->Cancel();
  }
  if (signal_) {
    signal_->UnregisterHandler(request.get());
  }
  if (signal_ && signal_->IsSignalled()) {
    return base::unexpected(Error::kAborted);
  }
  if (!completed || request->aborted()) {
    return base::unexpected(Error::kNetworkError);
  }
  auto response = request->TakeResult();
  if (response.net_error == net::ERR_INSUFFICIENT_RESOURCES) {
    return base::unexpected(Error::kInvalidData);
  }
  if (response.net_error != net::OK || !response.body) {
    return base::unexpected(Error::kNetworkError);
  }
  bytes_remaining_ -= response.body->size();
  return Response{response.code, std::move(response.etag),
                  std::move(*response.body)};
}

noventra_sync::SyncVaultTransport::ReadResult WorkspaceSyncTransport::ReadObject(
    const std::string& object_id,
    size_t maximum_bytes) {
  auto result = Request(object_id, false, std::nullopt, {}, maximum_bytes);
  if (!result.has_value()) {
    return base::unexpected(result.error());
  }
  if (result->code == 404) {
    return std::nullopt;
  }
  if (result->code != 200) {
    return base::unexpected(HttpError(result->code));
  }
  if (!IsWorkspaceSyncStrongETag(result->etag)) {
    return base::unexpected(Error::kInvalidData);
  }
  return noventra_sync::OpaqueSyncVaultObject{
      std::vector<uint8_t>(result->body.begin(), result->body.end()),
      std::move(result->etag)};
}

noventra_sync::SyncVaultTransport::WriteResult WorkspaceSyncTransport::WriteObject(
    const std::string& object_id,
    std::optional<std::string> expected_revision,
    std::vector<uint8_t> data) {
  auto result = Request(object_id, true, std::move(expected_revision),
                        std::move(data), 4096);
  if (!result.has_value()) {
    return base::unexpected(result.error());
  }
  if (result->code != 200 && result->code != 201 && result->code != 204) {
    return base::unexpected(HttpError(result->code));
  }
  if (!IsWorkspaceSyncStrongETag(result->etag)) {
    return base::unexpected(Error::kInvalidData);
  }
  return std::move(result->etag);
}

void WorkspaceSyncTransport::CollectEpochsBefore(
    uint32_t minimum_retained_epoch) {
  // v1 deliberately retains opaque ciphertext. Never infer retention from
  // object IDs on the server. See the sync-core handoff for storage growth.
}
}  // namespace noventra
