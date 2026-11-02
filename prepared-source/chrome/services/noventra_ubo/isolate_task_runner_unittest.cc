// SPDX-License-Identifier: GPL-3.0-or-later
#include "base/task/single_thread_task_runner.h"
#include <algorithm>
#include "chrome/services/noventra_ubo/isolate_task_runner.h"
#include <mutex>
#include <thread>
#include <vector>
#include "base/at_exit.h"
#include "base/command_line.h"
#include "base/task/thread_pool/thread_pool_instance.h"
#include "gin/array_buffer.h"
#include "gin/public/isolate_holder.h"
#include "gin/public/v8_platform.h"
#include "gin/v8_initializer.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "v8/include/v8.h"
namespace noventra_ubo {
namespace {
// Deterministic backing queue; callbacks still go through the real Gin runner.
class ManualRunner final : public base::SingleThreadTaskRunner {
 public:
  bool PostDelayedTask(const base::Location&, base::OnceClosure task, base::TimeDelta delay) override {
    std::lock_guard lock(mutex_); queue_.push_back({std::move(task), delay.is_positive()}); return true;
  }
  bool PostNonNestableDelayedTask(const base::Location& location, base::OnceClosure task, base::TimeDelta delay) override {
    return PostDelayedTask(location, std::move(task), delay);
  }
  bool RunsTasksInCurrentSequence() const override { return std::this_thread::get_id() == owner_; }
  void Drain(bool delayed = false) {
    for (;;) {
      base::OnceClosure task;
      {
        std::lock_guard lock(mutex_);
        auto it = std::find_if(queue_.begin(), queue_.end(), [delayed](const Entry& e) { return delayed || !e.delayed; });
        if (it == queue_.end()) break;
        task = std::move(it->task); queue_.erase(it);
      }
      std::move(task).Run();
    }
  }
 private:
  ~ManualRunner() override = default;
  struct Entry { base::OnceClosure task; bool delayed; };
  const std::thread::id owner_ = std::this_thread::get_id();
  std::mutex mutex_;
  std::vector<Entry> queue_;
};
class CountTask final : public v8::Task {
 public:
  CountTask(int* runs, int* destroyed) : runs_(runs), destroyed_(destroyed) {}
  ~CountTask() override { ++*destroyed_; }
  void Run() override { ++*runs_; }
 private:
  int* runs_;
  int* destroyed_;
};
class IsolateTaskTest : public testing::Test {
 protected:
  void SetUp() override {
#ifdef V8_USE_EXTERNAL_STARTUP_DATA
    gin::V8Initializer::LoadV8Snapshot();
#endif
    gin::IsolateHolder::Initialize(gin::IsolateHolder::kNonStrictMode,
                                  gin::ArrayBufferAllocator::SharedInstance());
    backing_ = base::MakeRefCounted<ManualRunner>();
    current_ = std::make_unique<base::SingleThreadTaskRunner::CurrentDefaultHandle>(backing_);
    runner_ = base::MakeRefCounted<IsolateTaskRunner>(backing_);
    holder_ = std::make_unique<gin::IsolateHolder>(runner_, gin::IsolateHolder::kUseLocker,
                                                 gin::IsolateHolder::IsolateType::kUtility);
  }
  void Dispose() { runner_->Shutdown(); holder_.reset(); }
  void TearDown() override { if (holder_) Dispose(); }
  scoped_refptr<ManualRunner> backing_;
  std::unique_ptr<base::SingleThreadTaskRunner::CurrentDefaultHandle> current_;
  scoped_refptr<IsolateTaskRunner> runner_;
  std::unique_ptr<gin::IsolateHolder> holder_;
};
TEST_F(IsolateTaskTest, RunsWhileAlive) {
  int runs = 0, destroyed = 0;
  auto foreground = gin::V8Platform::Get()->GetForegroundTaskRunner(holder_->isolate(), v8::TaskPriority::kUserVisible);
  foreground->PostTask(std::make_unique<CountTask>(&runs, &destroyed));
  backing_->Drain();
  EXPECT_EQ(runs, 1); EXPECT_EQ(destroyed, 1);
}
TEST_F(IsolateTaskTest, QueuedWrappersCannotEnterDisposedIsolate) {
  int runs = 0, destroyed = 0;
  auto foreground = gin::V8Platform::Get()->GetForegroundTaskRunner(holder_->isolate(), v8::TaskPriority::kUserVisible);
  foreground->PostTask(std::make_unique<CountTask>(&runs, &destroyed));
  foreground->PostDelayedTask(std::make_unique<CountTask>(&runs, &destroyed), 5);
  Dispose();
  backing_->Drain(true);
  EXPECT_EQ(runs, 0); EXPECT_EQ(destroyed, 2);
}
TEST_F(IsolateTaskTest, PendingWasmCompileCanBeDisposed) {
  auto* isolate = holder_->isolate();
  {
    v8::Locker lock(isolate); v8::Isolate::Scope entered(isolate);
    v8::HandleScope handles(isolate);
    auto context = v8::Context::New(isolate); v8::Context::Scope scope(context);
    auto source = v8::String::NewFromUtf8Literal(isolate,
        "WebAssembly.compile(new Uint8Array([0,97,115,109,1,0,0,0]))");
    auto result = v8::Script::Compile(context, source).ToLocalChecked()->Run(context).ToLocalChecked();
    ASSERT_TRUE(result->IsPromise());
    ASSERT_EQ(result.As<v8::Promise>()->State(), v8::Promise::kPending);
  }
  Dispose();
  backing_->Drain(true);
}
}  // namespace
}  // namespace noventra_ubo
int main(int argc, char** argv) {
  base::AtExitManager at_exit;
  base::CommandLine::Init(argc, argv);
  base::ThreadPoolInstance::CreateAndStartWithDefaultParams("UboTaskTest");
  testing::InitGoogleTest(&argc, argv);
  int result = RUN_ALL_TESTS();
  base::ThreadPoolInstance::Get()->Shutdown();
  return result;
}
