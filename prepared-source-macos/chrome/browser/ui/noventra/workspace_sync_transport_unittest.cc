// Copyright 2026 The Noventra Authors. All rights reserved.
#include "chrome/browser/ui/noventra/workspace_sync_transport.h"

#include "base/base64url.h"
#include "base/functional/bind.h"
#include "base/task/thread_pool.h"
#include "base/test/task_environment.h"
#include "base/test/test_future.h"
#include "services/network/public/cpp/weak_wrapper_shared_url_loader_factory.h"
#include "services/network/test/test_url_loader_factory.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace noventra {
namespace {
constexpr char kVault[] = "c65f6051-1089-4518-9eb1-8fb140a9bb59";

TEST(WorkspaceSyncTransportTest, OriginAllowsHttpsAndExplicitHttpLoopbackOnly) {
  for (const auto* origin : {"https://example.com", "https://example.com:8443/",
                             "http://localhost:8765", "http://127.0.0.1:8765/",
                             "http://[::1]:8765"}) {
    EXPECT_TRUE(ValidateWorkspaceSyncOrigin(origin).has_value()) << origin;
  }
  for (const auto* origin :
       {"http://example.com", "http://127.1", "http://2130706433",
        "http://127.0.0.2", "http://localhost.", "http://[0:0:0:0:0:0:0:1]",
        "ftp://example.com", "https://user:password@example.com",
        "https://@example.com", "https://example.com/path",
        "https://example.com/./", "https://example.com?query",
        "https://example.com#fragment", " https://example.com",
        "https://example.com\\"}) {
    EXPECT_FALSE(ValidateWorkspaceSyncOrigin(origin).has_value()) << origin;
  }
}

TEST(WorkspaceSyncTransportTest,
     TokenIsBoundedPrintableAsciiWithoutWhitespace) {
  EXPECT_TRUE(IsWorkspaceSyncToken(std::string(32, 'a')));
  EXPECT_TRUE(IsWorkspaceSyncToken(std::string(1024, 'a')));
  EXPECT_FALSE(IsWorkspaceSyncToken(std::string(31, 'a')));
  EXPECT_FALSE(IsWorkspaceSyncToken(std::string(1025, 'a')));
  EXPECT_FALSE(IsWorkspaceSyncToken(std::string(32, 'a') + " internal space"));
  EXPECT_FALSE(
      IsWorkspaceSyncToken(std::string(32, 'a') + "\r\nX-Injected: yes"));
}

TEST(WorkspaceSyncTransportTest, StrongEtagsAreExactSingleQuotedOpaqueValues) {
  EXPECT_TRUE(
      IsWorkspaceSyncStrongETag("\"0123456789abcdef0123456789abcdef\""));
  EXPECT_FALSE(IsWorkspaceSyncStrongETag("W/\"weak\""));
  EXPECT_FALSE(IsWorkspaceSyncStrongETag("unquoted"));
  EXPECT_FALSE(IsWorkspaceSyncStrongETag("\"\""));
  EXPECT_FALSE(IsWorkspaceSyncStrongETag("\"one\", \"two\""));
  EXPECT_FALSE(IsWorkspaceSyncStrongETag("\"line\r\nbreak\""));
}

TEST(WorkspaceSyncTransportTest,
     ObjectIdIsOpaqueUtf8InOneBase64urlPathSegment) {
  const std::string object = "namespace/../head ?#% \xe2\x98\x83";
  auto url =
      WorkspaceSyncObjectURL(GURL("https://example.com/"), kVault, object);
  ASSERT_TRUE(url.has_value());
  EXPECT_FALSE(url->has_query());
  EXPECT_FALSE(url->has_ref());
  const auto segment = url->path().substr(url->path().find_last_of('/') + 1);
  EXPECT_EQ(segment.find('='), std::string::npos);
  std::string decoded;
  ASSERT_TRUE(base::Base64UrlDecode(
      segment, base::Base64UrlDecodePolicy::DISALLOW_PADDING, &decoded));
  EXPECT_EQ(decoded, object);
}

TEST(WorkspaceSyncTransportTest, ObjectAndVaultBoundsRejectBeforeNetwork) {
  const GURL origin("http://127.0.0.1:8765/");
  EXPECT_TRUE(WorkspaceSyncObjectURL(origin, kVault, std::string(4096, 'x'))
                  .has_value());
  EXPECT_FALSE(WorkspaceSyncObjectURL(origin, kVault, std::string(4097, 'x'))
                   .has_value());
  EXPECT_FALSE(WorkspaceSyncObjectURL(origin, kVault, "").has_value());
  EXPECT_FALSE(WorkspaceSyncObjectURL(origin, kVault, std::string(1, '\xff'))
                   .has_value());
  EXPECT_FALSE(WorkspaceSyncObjectURL(
                   origin, "C65F6051-1089-4518-9EB1-8FB140A9BB59", "head")
                   .has_value());
}

TEST(WorkspaceSyncTransportTest, OversizedStreamAbortsBeforeLoaderCompletion) {
  base::test::TaskEnvironment task_environment;
  network::TestURLLoaderFactory factory;
  auto network =
      base::MakeRefCounted<WorkspaceSyncNetwork>(factory.GetSafeWeakWrapper());
  const GURL origin("http://127.0.0.1:8765/");
  auto url = WorkspaceSyncObjectURL(origin, kVault, "head");
  ASSERT_TRUE(url.has_value());
  // Even a completed fake response delivers the stream data before the loader
  // finishes. The old overflow path DCHECKed inside ResponseInfo() here.
  factory.AddResponse(url->spec(), std::string(4096, 'x'));
  base::test::TestFuture<noventra_sync::SyncVaultTransport::ReadResult> result;
  base::ThreadPool::PostTaskAndReplyWithResult(
      FROM_HERE, {base::MayBlock()},
      base::BindOnce(
          [](scoped_refptr<WorkspaceSyncNetwork> network, GURL origin) {
            WorkspaceSyncTransport transport(
                std::move(origin), kVault, std::string(32, 't'),
                std::move(network), base::TimeTicks::Now() + base::Seconds(5));
            return transport.ReadObject("head", 32);
          },
          network, origin),
      result.GetCallback());
  auto response = result.Take();
  ASSERT_FALSE(response.has_value());
  EXPECT_EQ(response.error(), noventra_sync::SyncVaultTransportError::kInvalidData);
  task_environment.RunUntilIdle();
}
}  // namespace
}  // namespace noventra
