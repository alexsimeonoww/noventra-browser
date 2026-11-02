// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef CHROME_SERVICES_NOVENTRA_UBO_ISOLATE_TASK_RUNNER_H_
#define CHROME_SERVICES_NOVENTRA_UBO_ISOLATE_TASK_RUNNER_H_
#include <atomic>
#include <memory>
#include "base/check.h"
#include "base/functional/bind.h"
#include "base/task/single_thread_task_runner.h"
#include "base/time/time.h"
namespace noventra_ubo {
// Gin's Locker wrapper holds an unretained isolate and enters it before V8
// checks cancellation. Gate the entire wrapper before disposing that isolate.
class IsolateTaskRunner final : public base::SingleThreadTaskRunner {
 public:
  explicit IsolateTaskRunner(scoped_refptr<base::SingleThreadTaskRunner> backing)
      : backing_(std::move(backing)), alive_(std::make_shared<std::atomic<bool>>(true)) {}
  void Shutdown() {
    CHECK(RunsTasksInCurrentSequence());
    alive_->store(false, std::memory_order_release);
  }
  bool PostDelayedTask(const base::Location& location, base::OnceClosure task,
                       base::TimeDelta delay) override {
    return backing_->PostDelayedTask(location, Guard(std::move(task)), delay);
  }
  bool PostNonNestableDelayedTask(const base::Location& location, base::OnceClosure task,
                                  base::TimeDelta delay) override {
    return backing_->PostNonNestableDelayedTask(location, Guard(std::move(task)), delay);
  }
  bool RunsTasksInCurrentSequence() const override { return backing_->RunsTasksInCurrentSequence(); }
 private:
  ~IsolateTaskRunner() override = default;
  base::OnceClosure Guard(base::OnceClosure task) {
    return base::BindOnce([](std::shared_ptr<std::atomic<bool>> alive, base::OnceClosure task) {
      if (alive->load(std::memory_order_acquire)) std::move(task).Run();
    }, alive_, std::move(task));
  }
  const scoped_refptr<base::SingleThreadTaskRunner> backing_;
  const std::shared_ptr<std::atomic<bool>> alive_;
};
}  // namespace noventra_ubo
#endif
