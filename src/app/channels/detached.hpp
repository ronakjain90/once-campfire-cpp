// A coroutine that nobody awaits: it starts at once and ends by itself. The channels use it for the writes that
// the client does not wait for (presence). Rails: ActiveRecord callbacks that run inside `subscribed`.
#pragma once

#include <chrono>
#include <condition_variable>
#include <coroutine>
#include <deque>
#include <exception>
#include <mutex>
#include <thread>
#include <utility>

#include "core/log.hpp"
#include "core/scheduler.hpp"
#include "core/task.hpp"

namespace campfire::app::channels {

struct Detached {
  struct promise_type {
    Detached get_return_object() noexcept { return {}; }
    std::suspend_never initial_suspend() noexcept { return {}; }
    std::suspend_never final_suspend() noexcept { return {}; }
    void return_void() noexcept {}
    void unhandled_exception() noexcept {
      try {
        throw;
      } catch (const std::exception& error) {
        log_error("a detached task failed: {}", error.what());
      } catch (...) {
        log_error("a detached task failed");
      }
    }
  };
};

// The thread that finishes the detached tasks. A task that waits for the writer resumes here, not on a worker: the
// workers can be gone when the writer answers (the close of a socket at the stop of the server starts a write).
// `drain` waits for the tasks that still run. Call it before the hub and the database go away.
class DetachedRunner final : public Scheduler {
 public:
  DetachedRunner() : thread_([this] { loop(); }) {}
  ~DetachedRunner() override {
    drain();
    {
      const std::lock_guard lock(mutex_);
      stop_ = true;
    }
    changed_.notify_all();
    thread_.join();
  }
  DetachedRunner(const DetachedRunner&) = delete;
  DetachedRunner& operator=(const DetachedRunner&) = delete;

  void post(std::coroutine_handle<> handle) override {
    {
      const std::lock_guard lock(mutex_);
      queue_.push_back(handle);
    }
    changed_.notify_all();
  }
  [[nodiscard]] bool on_owner_thread() const noexcept override {
    return std::this_thread::get_id() == thread_.get_id();
  }

  // Starts `task` on the thread of the runner and lets it go without an owner. The frame of `task` stays alive until
  // it ends.
  void run(Task<void> task) {
    {
      const std::lock_guard lock(mutex_);
      ++active_;
    }
    run_counted(std::move(task));
  }

  // Waits until every task has ended (at most 10 seconds).
  void drain() {
    std::unique_lock lock(mutex_);
    changed_.wait_for(lock, std::chrono::seconds(10), [this] { return active_ == 0; });
  }

 private:
  struct Done {
    DetachedRunner* runner;
    ~Done() {
      {
        const std::lock_guard lock(runner->mutex_);
        --runner->active_;
      }
      runner->changed_.notify_all();
    }
  };

  // Moves the coroutine to the thread of the runner: a task resumes only on the thread of its scheduler.
  struct Hop {
    DetachedRunner* runner;
    [[nodiscard]] bool await_ready() const noexcept { return false; }
    void await_suspend(std::coroutine_handle<> handle) const { runner->post(handle); }
    void await_resume() const noexcept {}
  };

  Detached run_counted(Task<void> task) {
    const Done done{this};
    co_await Hop{this};
    co_await std::move(task);
  }

  void loop() {
    std::unique_lock lock(mutex_);
    while (true) {
      changed_.wait(lock, [this] { return stop_ || !queue_.empty(); });
      if (queue_.empty()) return;  // stop_ is set and nothing is left
      const std::coroutine_handle<> handle = queue_.front();
      queue_.pop_front();
      lock.unlock();
      handle.resume();
      lock.lock();
    }
  }

  std::mutex mutex_;
  std::condition_variable changed_;
  std::deque<std::coroutine_handle<>> queue_;
  std::size_t active_ = 0;
  bool stop_ = false;
  std::thread thread_;
};

}  // namespace campfire::app::channels
