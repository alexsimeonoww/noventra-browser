// Copyright 2026 The Noventra Authors. All rights reserved.
#ifndef CHROME_BROWSER_UI_NOVENTRA_WORKSPACE_SYNC_TRANSPORT_H_
#define CHROME_BROWSER_UI_NOVENTRA_WORKSPACE_SYNC_TRANSPORT_H_

#include <cstddef>
#include <string>
#include <string_view>

#include "base/memory/raw_ptr.h"
#include "base/memory/ref_counted_delete_on_sequence.h"
#include "base/memory/scoped_refptr.h"
#include "base/time/time.h"
#include "components/noventra_sync_vault/sync_vault_transport.h"
#include "services/network/public/cpp/shared_url_loader_factory.h"
#include "url/gurl.h"

namespace noventra {
class WorkspaceSyncRequest;

base::expected<GURL, std::string> ValidateWorkspaceSyncOrigin(
    const std::string& origin);
bool IsWorkspaceSyncToken(std::string_view token);
bool IsWorkspaceSyncStrongETag(std::string_view value);
base::expected<GURL, std::string> WorkspaceSyncObjectURL(
    const GURL& origin,
    const std::string& vault_id,
    const std::string& object_id);

// Construct on UI. The profile factory and all loaders stay on that sequence;
// only this thread-safe reference crosses to the blocking store worker.
class WorkspaceSyncNetwork
    : public base::RefCountedDeleteOnSequence<WorkspaceSyncNetwork> {
 public:
  explicit WorkspaceSyncNetwork(
      scoped_refptr<network::SharedURLLoaderFactory> factory);
  void Start(scoped_refptr<WorkspaceSyncRequest> request);
  scoped_refptr<base::SequencedTaskRunner> task_runner() const {
    return task_runner_;
  }

 private:
  friend class base::RefCountedDeleteOnSequence<WorkspaceSyncNetwork>;
  friend class base::DeleteHelper<WorkspaceSyncNetwork>;
  ~WorkspaceSyncNetwork();
  const scoped_refptr<base::SequencedTaskRunner> task_runner_;
  const scoped_refptr<network::SharedURLLoaderFactory> factory_;
};

// Synchronous only on a MayBlock worker, never UI. Each request has a 30s
// timeout; the entire operation also has a deadline and a request budget.
class WorkspaceSyncTransport final : public noventra_sync::SyncVaultTransport {
 public:
  static constexpr size_t kMaximumRequestsPerOperation = 512;
  static constexpr size_t kMaximumBytesPerOperation = 32 * 1024 * 1024;
  WorkspaceSyncTransport(GURL origin,
                         std::string vault_id,
                         std::string token,
                         scoped_refptr<WorkspaceSyncNetwork> network,
                         base::TimeTicks deadline);
  ~WorkspaceSyncTransport() override;
  // Worker-sequence boundary between bounded operations. The store may retain
  // its authenticated read cursor while this transport receives a new budget.
  void BeginOperation(base::TimeTicks deadline);
  void SetCancelationSignal(syncer::CancelationSignal* signal) override;
  ReadResult ReadObject(const std::string& object_id,
                        size_t maximum_bytes) override;
  WriteResult WriteObject(const std::string& object_id,
                          std::optional<std::string> expected_revision,
                          std::vector<uint8_t> data) override;
  void CollectEpochsBefore(uint32_t minimum_retained_epoch) override;

 private:
  struct Response {
    int code = 0;
    std::string etag;
    std::string body;
  };
  base::expected<Response, noventra_sync::SyncVaultTransportError> Request(
      const std::string& object_id,
      bool write,
      std::optional<std::string> revision,
      std::vector<uint8_t> data,
      size_t maximum_bytes);

  const GURL origin_;
  const std::string vault_id_;
  const std::string token_;
  const scoped_refptr<WorkspaceSyncNetwork> network_;
  base::TimeTicks deadline_;
  size_t requests_remaining_ = kMaximumRequestsPerOperation;
  size_t bytes_remaining_ = kMaximumBytesPerOperation;
  raw_ptr<syncer::CancelationSignal> signal_ = nullptr;
};
}  // namespace noventra
#endif  // CHROME_BROWSER_UI_NOVENTRA_WORKSPACE_SYNC_TRANSPORT_H_
