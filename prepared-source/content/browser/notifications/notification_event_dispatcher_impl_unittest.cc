// Copyright 2018 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "content/browser/notifications/notification_event_dispatcher_impl.h"

#include <stdint.h>

#include <memory>
#include <utility>
#include <vector>

#include "base/functional/callback_helpers.h"
#include "base/test/task_environment.h"
#include "base/test/test_future.h"
#include "base/test/test_simple_task_runner.h"
#include "content/browser/notifications/notification_id_generator.h"
#include "content/browser/notifications/platform_notification_context_impl.h"
#include "content/browser/renderer_host/render_frame_host_impl.h"
#include "content/public/browser/render_process_host.h"
#include "content/public/browser/storage_partition.h"
#include "content/public/browser/storage_partition_config.h"
#include "content/public/common/persistent_notification_status.h"
#include "content/public/test/test_browser_context.h"
#include "content/public/test/test_renderer_host.h"
#include "mojo/public/cpp/bindings/pending_remote.h"
#include "mojo/public/cpp/bindings/receiver.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "third_party/blink/public/mojom/notifications/notification_service.mojom.h"

namespace content {

namespace {

const char kPrimaryUniqueId[] = "this_should_be_a_unique_id";
const char kSomeOtherUniqueId[] = "and_this_one_is_different_and_also_unique";

class TestNotificationListener
    : public blink::mojom::NonPersistentNotificationListener {
 public:
  TestNotificationListener() = default;

  TestNotificationListener(const TestNotificationListener&) = delete;
  TestNotificationListener& operator=(const TestNotificationListener&) = delete;

  ~TestNotificationListener() override = default;

  // Closes the bindings associated with this listener.
  void Close() { receiver_.reset(); }

  // Returns an InterfacePtr to this listener.
  mojo::PendingRemote<blink::mojom::NonPersistentNotificationListener>
  GetRemote() {
    mojo::PendingRemote<blink::mojom::NonPersistentNotificationListener> remote;
    receiver_.Bind(remote.InitWithNewPipeAndPassReceiver());
    return remote;
  }

  // Returns the number of OnShow events received by this listener.
  int on_show_count() const { return on_show_count_; }

  // Returns the number of OnClick events received by this listener.
  int on_click_count() const { return on_click_count_; }

  // Returns the number of OnClose events received by this listener.
  int on_close_count() const { return on_close_count_; }

  // blink::mojom::NonPersistentNotificationListener implementation.
  void OnShow() override { on_show_count_++; }
  void OnClick(OnClickCallback completed_closure) override {
    on_click_count_++;
    std::move(completed_closure).Run();
  }
  void OnClose(OnCloseCallback completed_closure) override {
    on_close_count_++;
    std::move(completed_closure).Run();
  }

 private:
  int on_show_count_ = 0;
  int on_click_count_ = 0;
  int on_close_count_ = 0;
  mojo::Receiver<blink::mojom::NonPersistentNotificationListener> receiver_{
      this};
};

}  // anonymous namespace

class NotificationEventDispatcherImplTest : public RenderViewHostTestHarness {
 public:
  NotificationEventDispatcherImplTest()
      : dispatcher_(new NotificationEventDispatcherImpl(), Deleter) {}

  NotificationEventDispatcherImplTest(
      const NotificationEventDispatcherImplTest&) = delete;
  NotificationEventDispatcherImplTest& operator=(
      const NotificationEventDispatcherImplTest&) = delete;

  ~NotificationEventDispatcherImplTest() override = default;

  // Waits until the task runner managing the Mojo connection has finished.
  void WaitForMojoTasksToComplete() { task_environment()->RunUntilIdle(); }

  static void Deleter(NotificationEventDispatcherImpl* dispatcher) {
    delete dispatcher;
  }

 protected:
  struct CreatorTypeTestData {
    RenderProcessHost::NotificationServiceCreatorType creator_type;
    bool is_document_pointer_empty;
    bool is_show_event_dispatched;
    bool is_click_event_dispatched;
    bool is_close_event_dispatched;
  };

  std::unique_ptr<NotificationEventDispatcherImpl, decltype(&Deleter)>
      dispatcher_;
};

TEST_F(NotificationEventDispatcherImplTest, ResolvesUnloadedAccountPartition) {
  const auto config = StoragePartitionConfig::Create(
      browser_context(), "noventra-account-spaces",
      "11111111-1111-4111-8111-111111111111", false);
  const GURL origin("https://example.com");
  NotificationIdGenerator generator(config);
  const auto id =
      generator.GenerateForPersistentNotification(origin, "same-tag", false, 1);
  ASSERT_EQ(nullptr, browser_context()->GetStoragePartition(
                         config, /*can_create=*/false));
  auto* partition =
      NotificationEventDispatcher::GetStoragePartitionForNotification(
          browser_context(), id, origin);
  ASSERT_NE(nullptr, partition);
  EXPECT_EQ(config, partition->GetConfig());
  EXPECT_NE(browser_context()->GetDefaultStoragePartition(), partition);
  EXPECT_EQ(partition, browser_context()->GetStoragePartition(config));
  // The real storage partition must initialize its generator from its own
  // worker context, not the profile's default partition or an active tab.
  auto* context = static_cast<PlatformNotificationContextImpl*>(
      partition->GetPlatformNotificationContext());
  EXPECT_EQ(
      id,
      context->notification_id_generator()->GenerateForPersistentNotification(
          origin, "same-tag", false, 1));
  EXPECT_EQ(browser_context()->GetDefaultStoragePartition(),
            NotificationEventDispatcher::GetStoragePartitionForNotification(
                browser_context(), "p#https://example.com/#1same-tag", origin));
}

TEST_F(NotificationEventDispatcherImplTest, RejectsMalformedAccountIdentity) {
  const GURL origin("https://example.com");
  const char* invalid_ids[] = {
      "ps0../profile#https://example.com/#1tag",
      "ps0not-a-uuid#https://example.com/#1tag",
      "ps011111111-1111-4111-8111-111111111111extra#https://example.com/#1tag",
      "ps211111111-1111-4111-8111-111111111111#https://example.com/#1tag",
      "ps0AAAAAAAA-AAAA-4AAA-8AAA-AAAAAAAAAAAA#https://example.com/#1tag",
      "ps011111111-1111-4111-8111-111111111111#https://other.com/#1tag",
      "ps011111111-1111-4111-8111-111111111111#https://example.com/path#1tag",
      "ps011111111-1111-4111-8111-111111111111#https://example.com/#1",
      "ps011111111-1111-4111-8111-111111111111#https://example.com/#2tag",
      "ps",
      "",
      "n#https://example.com#token"};
  for (const auto* id : invalid_ids) {
    SCOPED_TRACE(id);
    EXPECT_EQ(nullptr,
              NotificationEventDispatcher::GetStoragePartitionForNotification(
                  browser_context(), id, origin));
    base::test::TestFuture<PersistentNotificationStatus> click;
    dispatcher_->DispatchNotificationClickEvent(browser_context(), id, origin,
                                                std::nullopt, std::nullopt,
                                                click.GetCallback());
    EXPECT_EQ(PersistentNotificationStatus::kDatabaseError, click.Get());
    base::test::TestFuture<PersistentNotificationStatus> close;
    dispatcher_->DispatchNotificationCloseEvent(browser_context(), id, origin,
                                                true, close.GetCallback());
    EXPECT_EQ(PersistentNotificationStatus::kDatabaseError, close.Get());
  }
}

TEST_F(NotificationEventDispatcherImplTest,
       AccountMemoryIdentityCannotCrossOTR) {
  const GURL origin("https://example.com");
  const auto regular_config = StoragePartitionConfig::Create(
      browser_context(), "noventra-account-spaces",
      "11111111-1111-4111-8111-111111111111", false);
  TestBrowserContext otr;
  otr.set_is_off_the_record(true);
  const auto memory_config = StoragePartitionConfig::Create(
      &otr, "noventra-account-spaces", regular_config.partition_name(), true);
  NotificationIdGenerator regular(regular_config);
  NotificationIdGenerator memory(memory_config);
  const auto regular_id =
      regular.GenerateForPersistentNotification(origin, "same-tag", false, 1);
  const auto memory_id =
      memory.GenerateForPersistentNotification(origin, "same-tag", false, 1);
  EXPECT_EQ(nullptr,
            NotificationEventDispatcher::GetStoragePartitionForNotification(
                &otr, regular_id, origin));
  EXPECT_EQ(nullptr,
            NotificationEventDispatcher::GetStoragePartitionForNotification(
                browser_context(), memory_id, origin));
  auto* partition =
      NotificationEventDispatcher::GetStoragePartitionForNotification(
          &otr, memory_id, origin);
  ASSERT_NE(nullptr, partition);
  EXPECT_EQ(memory_config, partition->GetConfig());
  EXPECT_TRUE(partition->GetConfig().in_memory());
}

TEST_F(NotificationEventDispatcherImplTest,
       StaleAccountNotificationFailsWithoutDefaultFallback) {
  const auto config = StoragePartitionConfig::Create(
      browser_context(), "noventra-account-spaces",
      "33333333-3333-4333-8333-333333333333", false);
  NotificationIdGenerator generator(config);
  const GURL origin("https://example.com");
  const auto id = generator.GenerateForPersistentNotification(
      origin, "stale-tag", false, 1);
  base::test::TestFuture<PersistentNotificationStatus> click;
  dispatcher_->DispatchNotificationClickEvent(browser_context(), id, origin,
                                              std::nullopt, std::nullopt,
                                              click.GetCallback());
  EXPECT_EQ(PersistentNotificationStatus::kDatabaseError, click.Get());
  base::test::TestFuture<PersistentNotificationStatus> close;
  dispatcher_->DispatchNotificationCloseEvent(browser_context(), id, origin,
                                              true, close.GetCallback());
  EXPECT_EQ(PersistentNotificationStatus::kDatabaseError, close.Get());
  // The ID selected the account partition even with no tabs or database row.
  auto* partition = browser_context()->GetStoragePartition(config, false);
  ASSERT_NE(nullptr, partition);
  EXPECT_NE(browser_context()->GetDefaultStoragePartition(), partition);
}

TEST_F(NotificationEventDispatcherImplTest,
       DispatchNonPersistentShowEvent_NotifiesCorrectRegisteredListener) {
  auto listener = std::make_unique<TestNotificationListener>();
  dispatcher_->RegisterNonPersistentNotificationListener(
      kPrimaryUniqueId, listener->GetRemote(), main_rfh()->GetWeakDocumentPtr(),
      RenderProcessHost::NotificationServiceCreatorType::kDocument);
  auto other_listener = std::make_unique<TestNotificationListener>();
  dispatcher_->RegisterNonPersistentNotificationListener(
      kSomeOtherUniqueId, other_listener->GetRemote(),
      main_rfh()->GetWeakDocumentPtr(),
      RenderProcessHost::NotificationServiceCreatorType::kDocument);

  dispatcher_->DispatchNonPersistentShowEvent(kPrimaryUniqueId);

  WaitForMojoTasksToComplete();

  EXPECT_EQ(listener->on_show_count(), 1);
  EXPECT_EQ(other_listener->on_show_count(), 0);

  dispatcher_->DispatchNonPersistentShowEvent(kSomeOtherUniqueId);

  WaitForMojoTasksToComplete();

  EXPECT_EQ(listener->on_show_count(), 1);
  EXPECT_EQ(other_listener->on_show_count(), 1);
}

TEST_F(NotificationEventDispatcherImplTest,
       DispatchNonPersistentEvent_RegisterListenerWithDifferentCreatorTypes) {
  // For `kDocument` and `kDedicatedWorker`, if the document pointer is empty,
  // then all the non persistent notification event should not be dispatched.
  // For `kSharedWorker`, the document pointer should always be empty and the
  // event will always be dispatched.
  // Since it's not possible for `kServiceWorker` to create non persistent
  // notification events, the test cases for those two creator types are not
  // added.
  std::vector<CreatorTypeTestData> creator_type_tests{
      {.creator_type =
           RenderProcessHost::NotificationServiceCreatorType::kDocument,
       .is_document_pointer_empty = true,
       .is_show_event_dispatched = false,
       .is_click_event_dispatched = false,
       .is_close_event_dispatched = false},
      {.creator_type =
           RenderProcessHost::NotificationServiceCreatorType::kDocument,
       .is_document_pointer_empty = false,
       .is_show_event_dispatched = true,
       .is_click_event_dispatched = true,
       .is_close_event_dispatched = true},
      {.creator_type =
           RenderProcessHost::NotificationServiceCreatorType::kDedicatedWorker,
       .is_document_pointer_empty = true,
       .is_show_event_dispatched = false,
       .is_click_event_dispatched = false,
       .is_close_event_dispatched = false},
      {.creator_type =
           RenderProcessHost::NotificationServiceCreatorType::kDedicatedWorker,
       .is_document_pointer_empty = false,
       .is_show_event_dispatched = true,
       .is_click_event_dispatched = true,
       .is_close_event_dispatched = true},
      {.creator_type =
           RenderProcessHost::NotificationServiceCreatorType::kSharedWorker,
       .is_document_pointer_empty = true,
       .is_show_event_dispatched = true,
       .is_click_event_dispatched = true,
       .is_close_event_dispatched = true},
  };

  for (auto t : creator_type_tests) {
    int expected_show_count = t.is_show_event_dispatched ? 1 : 0;
    int expected_click_count = t.is_click_event_dispatched ? 1 : 0;
    int expected_close_count = t.is_close_event_dispatched ? 1 : 0;

    auto listener = std::make_unique<TestNotificationListener>();
    dispatcher_->RegisterNonPersistentNotificationListener(
        kPrimaryUniqueId, listener->GetRemote(),
        t.is_document_pointer_empty ? WeakDocumentPtr()
                                    : main_rfh()->GetWeakDocumentPtr(),
        t.creator_type);

    dispatcher_->DispatchNonPersistentShowEvent(kPrimaryUniqueId);

    WaitForMojoTasksToComplete();

    EXPECT_EQ(listener->on_show_count(), expected_show_count);
    EXPECT_EQ(listener->on_click_count(), 0);
    EXPECT_EQ(listener->on_close_count(), 0);

    dispatcher_->DispatchNonPersistentClickEvent(kPrimaryUniqueId,
                                                 base::DoNothing());

    WaitForMojoTasksToComplete();

    EXPECT_EQ(listener->on_show_count(), expected_show_count);
    EXPECT_EQ(listener->on_click_count(), expected_click_count);
    EXPECT_EQ(listener->on_close_count(), 0);

    dispatcher_->DispatchNonPersistentCloseEvent(kPrimaryUniqueId,
                                                 base::DoNothing());

    WaitForMojoTasksToComplete();

    EXPECT_EQ(listener->on_show_count(), expected_show_count);
    EXPECT_EQ(listener->on_click_count(), expected_click_count);
    EXPECT_EQ(listener->on_close_count(), expected_close_count);
  }
}

TEST_F(NotificationEventDispatcherImplTest,
       DispatchNonPersistentEvent_DocumentInBFCache) {
  auto listener = std::make_unique<TestNotificationListener>();
  const WeakDocumentPtr document = main_rfh()->GetWeakDocumentPtr();
  RenderFrameHostImpl* rfh =
      static_cast<RenderFrameHostImpl*>(document.AsRenderFrameHostIfValid());
  EXPECT_TRUE(rfh);
  // The rfh should be initially in active lifecycle state.
  EXPECT_TRUE(
      rfh->IsInLifecycleState(RenderFrameHost::LifecycleState::kActive));
  dispatcher_->RegisterNonPersistentNotificationListener(
      kPrimaryUniqueId, listener->GetRemote(), document,
      RenderProcessHost::NotificationServiceCreatorType::kDocument);

  dispatcher_->DispatchNonPersistentShowEvent(kPrimaryUniqueId);

  WaitForMojoTasksToComplete();

  // After dispatching the show and click events when the rfh is active,
  // the counter should increment as expected.
  EXPECT_EQ(listener->on_show_count(), 1);
  EXPECT_EQ(listener->on_click_count(), 0);
  EXPECT_EQ(listener->on_close_count(), 0);

  dispatcher_->DispatchNonPersistentClickEvent(kPrimaryUniqueId,
                                               base::DoNothing());

  WaitForMojoTasksToComplete();

  EXPECT_EQ(listener->on_show_count(), 1);
  EXPECT_EQ(listener->on_click_count(), 1);
  EXPECT_EQ(listener->on_close_count(), 0);

  dispatcher_->DispatchNonPersistentShowEvent(kPrimaryUniqueId);

  WaitForMojoTasksToComplete();

  EXPECT_EQ(listener->on_show_count(), 2);
  EXPECT_EQ(listener->on_click_count(), 1);
  EXPECT_EQ(listener->on_close_count(), 0);

  // Simulate the scenario where the rfh is put into the back/forward cache
  // by setting the lifecycle state explicitly.
  rfh->SetLifecycleState(
      RenderFrameHostLifecycleStateImpl::kInBackForwardCache);
  EXPECT_TRUE(rfh->IsInLifecycleState(
      RenderFrameHost::LifecycleState::kInBackForwardCache));

  dispatcher_->DispatchNonPersistentClickEvent(kPrimaryUniqueId,
                                               base::DoNothing());

  WaitForMojoTasksToComplete();

  dispatcher_->DispatchNonPersistentCloseEvent(kPrimaryUniqueId,
                                               base::DoNothing());

  WaitForMojoTasksToComplete();

  // Now the dispatched click and close events should not invoke the listener.
  EXPECT_EQ(listener->on_show_count(), 2);
  EXPECT_EQ(listener->on_click_count(), 1);
  EXPECT_EQ(listener->on_close_count(), 0);

  // Simulate the scenario where the rfh is back to active state and
  // dispatch a close event.
  rfh->SetLifecycleState(RenderFrameHostLifecycleStateImpl::kActive);
  EXPECT_FALSE(rfh->IsInLifecycleState(
      RenderFrameHost::LifecycleState::kInBackForwardCache));

  dispatcher_->DispatchNonPersistentCloseEvent(kPrimaryUniqueId,
                                               base::DoNothing());

  WaitForMojoTasksToComplete();

  // Since the rfh is active, the dispatched close event should result in
  // the increment of the close counter.
  EXPECT_EQ(listener->on_show_count(), 2);
  EXPECT_EQ(listener->on_click_count(), 1);
  EXPECT_EQ(listener->on_close_count(), 1);
}

TEST_F(NotificationEventDispatcherImplTest,
       RegisterNonPersistentListener_FirstListenerGetsOnClose) {
  auto original_listener = std::make_unique<TestNotificationListener>();
  dispatcher_->RegisterNonPersistentNotificationListener(
      kPrimaryUniqueId, original_listener->GetRemote(),
      main_rfh()->GetWeakDocumentPtr(),
      RenderProcessHost::NotificationServiceCreatorType::kDocument);

  dispatcher_->DispatchNonPersistentShowEvent(kPrimaryUniqueId);

  ASSERT_EQ(original_listener->on_close_count(), 0);

  auto replacement_listener = std::make_unique<TestNotificationListener>();
  dispatcher_->RegisterNonPersistentNotificationListener(
      kPrimaryUniqueId, replacement_listener->GetRemote(),
      main_rfh()->GetWeakDocumentPtr(),
      RenderProcessHost::NotificationServiceCreatorType::kDocument);

  WaitForMojoTasksToComplete();

  EXPECT_EQ(original_listener->on_close_count(), 1);
  EXPECT_EQ(replacement_listener->on_close_count(), 0);
}

TEST_F(NotificationEventDispatcherImplTest,
       RegisterNonPersistentListener_SecondListenerGetsOnShow) {
  auto original_listener = std::make_unique<TestNotificationListener>();
  dispatcher_->RegisterNonPersistentNotificationListener(
      kPrimaryUniqueId, original_listener->GetRemote(),
      main_rfh()->GetWeakDocumentPtr(),
      RenderProcessHost::NotificationServiceCreatorType::kDocument);

  dispatcher_->DispatchNonPersistentShowEvent(kPrimaryUniqueId);

  WaitForMojoTasksToComplete();

  ASSERT_EQ(original_listener->on_show_count(), 1);

  auto replacement_listener = std::make_unique<TestNotificationListener>();
  dispatcher_->RegisterNonPersistentNotificationListener(
      kPrimaryUniqueId, replacement_listener->GetRemote(),
      main_rfh()->GetWeakDocumentPtr(),
      RenderProcessHost::NotificationServiceCreatorType::kDocument);

  dispatcher_->DispatchNonPersistentShowEvent(kPrimaryUniqueId);

  WaitForMojoTasksToComplete();

  ASSERT_EQ(original_listener->on_show_count(), 1);
  ASSERT_EQ(replacement_listener->on_show_count(), 1);
}

TEST_F(NotificationEventDispatcherImplTest,
       RegisterNonPersistentListener_ReplacedListenerGetsOnClick) {
  auto original_listener = std::make_unique<TestNotificationListener>();
  dispatcher_->RegisterNonPersistentNotificationListener(
      kPrimaryUniqueId, original_listener->GetRemote(),
      main_rfh()->GetWeakDocumentPtr(),
      RenderProcessHost::NotificationServiceCreatorType::kDocument);

  dispatcher_->DispatchNonPersistentShowEvent(kPrimaryUniqueId);

  auto replacement_listener = std::make_unique<TestNotificationListener>();
  dispatcher_->RegisterNonPersistentNotificationListener(
      kPrimaryUniqueId, replacement_listener->GetRemote(),
      main_rfh()->GetWeakDocumentPtr(),
      RenderProcessHost::NotificationServiceCreatorType::kDocument);

  WaitForMojoTasksToComplete();

  dispatcher_->DispatchNonPersistentClickEvent(kPrimaryUniqueId,
                                               base::DoNothing());

  WaitForMojoTasksToComplete();

  EXPECT_EQ(original_listener->on_click_count(), 0);
  EXPECT_EQ(original_listener->on_close_count(), 1);
  EXPECT_EQ(replacement_listener->on_click_count(), 1);
  EXPECT_EQ(replacement_listener->on_close_count(), 0);
}

TEST_F(NotificationEventDispatcherImplTest,
       DispatchNonPersistentClickEvent_NotifiesCorrectRegisteredListener) {
  auto listener = std::make_unique<TestNotificationListener>();
  dispatcher_->RegisterNonPersistentNotificationListener(
      kPrimaryUniqueId, listener->GetRemote(), main_rfh()->GetWeakDocumentPtr(),
      RenderProcessHost::NotificationServiceCreatorType::kDocument);
  auto other_listener = std::make_unique<TestNotificationListener>();
  dispatcher_->RegisterNonPersistentNotificationListener(
      kSomeOtherUniqueId, other_listener->GetRemote(),
      main_rfh()->GetWeakDocumentPtr(),
      RenderProcessHost::NotificationServiceCreatorType::kDocument);

  dispatcher_->DispatchNonPersistentClickEvent(kPrimaryUniqueId,
                                               base::DoNothing());

  WaitForMojoTasksToComplete();

  EXPECT_EQ(listener->on_click_count(), 1);
  EXPECT_EQ(other_listener->on_click_count(), 0);

  dispatcher_->DispatchNonPersistentClickEvent(kSomeOtherUniqueId,
                                               base::DoNothing());

  WaitForMojoTasksToComplete();

  EXPECT_EQ(listener->on_click_count(), 1);
  EXPECT_EQ(other_listener->on_click_count(), 1);
}

TEST_F(NotificationEventDispatcherImplTest,
       DispatchNonPersistentCloseEvent_NotifiesCorrectRegisteredListener) {
  auto listener = std::make_unique<TestNotificationListener>();
  dispatcher_->RegisterNonPersistentNotificationListener(
      kPrimaryUniqueId, listener->GetRemote(), main_rfh()->GetWeakDocumentPtr(),
      RenderProcessHost::NotificationServiceCreatorType::kDocument);
  auto other_listener = std::make_unique<TestNotificationListener>();
  dispatcher_->RegisterNonPersistentNotificationListener(
      kSomeOtherUniqueId, other_listener->GetRemote(),
      main_rfh()->GetWeakDocumentPtr(),
      RenderProcessHost::NotificationServiceCreatorType::kDocument);

  dispatcher_->DispatchNonPersistentCloseEvent(kPrimaryUniqueId,
                                               base::DoNothing());

  WaitForMojoTasksToComplete();

  EXPECT_EQ(listener->on_close_count(), 1);
  EXPECT_EQ(other_listener->on_close_count(), 0);

  dispatcher_->DispatchNonPersistentCloseEvent(kSomeOtherUniqueId,
                                               base::DoNothing());

  WaitForMojoTasksToComplete();

  EXPECT_EQ(listener->on_close_count(), 1);
  EXPECT_EQ(other_listener->on_close_count(), 1);
}

TEST_F(NotificationEventDispatcherImplTest,
       DispatchMultipleNonPersistentEvents_StopsNotifyingAfterClose) {
  auto listener = std::make_unique<TestNotificationListener>();
  dispatcher_->RegisterNonPersistentNotificationListener(
      kPrimaryUniqueId, listener->GetRemote(), main_rfh()->GetWeakDocumentPtr(),
      RenderProcessHost::NotificationServiceCreatorType::kDocument);

  dispatcher_->DispatchNonPersistentShowEvent(kPrimaryUniqueId);
  dispatcher_->DispatchNonPersistentClickEvent(kPrimaryUniqueId,
                                               base::DoNothing());
  dispatcher_->DispatchNonPersistentCloseEvent(kPrimaryUniqueId,
                                               base::DoNothing());

  WaitForMojoTasksToComplete();

  EXPECT_EQ(listener->on_show_count(), 1);
  EXPECT_EQ(listener->on_click_count(), 1);
  EXPECT_EQ(listener->on_close_count(), 1);

  // Should not be counted as the notification was already closed.
  dispatcher_->DispatchNonPersistentClickEvent(kPrimaryUniqueId,
                                               base::DoNothing());

  WaitForMojoTasksToComplete();

  EXPECT_EQ(listener->on_click_count(), 1);
}
}  // namespace content
