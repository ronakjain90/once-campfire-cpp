// Scheduler interface for coroutines, and a queue scheduler for tests and tools.
// Design: plans/architecture.md section 4 ("Handlers are coroutines").
#pragma once

#include <chrono>
#include <condition_variable>
#include <coroutine>
#include <cstddef>
#include <deque>
#include <mutex>
#include <thread>

namespace campfire {

// The owner of coroutines: one worker thread. T5 gives the real one (an epoll loop). A coroutine
// runs only on the thread of its scheduler. Code on any other thread must not resume a handle.
// It posts the handle back with `post`.
class Scheduler {
 public:
  virtual ~Scheduler() = default;

  // Queues `handle` to resume on the thread of the scheduler. Any thread can call it. The
  // scheduler resumes the handle once, later, and never inside this call.
  virtual void post(std::coroutine_handle<> handle) = 0;

  // True if the calling thread is the thread of this scheduler.
  [[nodiscard]] virtual bool on_owner_thread() const noexcept = 0;
};

// A scheduler with a mutex-protected queue. The thread that calls `run_*` is the owner. Use it in
// tests, and in tools that need no event loop.
class QueueScheduler final : public Scheduler {
 public:
  // The calling thread becomes the owner.
  QueueScheduler() : owner_(std::this_thread::get_id()) {}

  void post(std::coroutine_handle<> handle) override {
    // Notify while the lock is held: the owner can destroy this scheduler as soon as it sees the
    // handle, and a notify after the unlock would then touch a destroyed condition variable.
    const std::lock_guard lock(mutex_);
    queue_.push_back(handle);
    ready_.notify_one();
  }

  [[nodiscard]] bool on_owner_thread() const noexcept override { return std::this_thread::get_id() == owner_; }

  // Resumes the queued handles that exist now. Returns the number resumed. Owner thread only.
  std::size_t run_pending() {
    std::size_t count = 0;
    while (true) {
      std::coroutine_handle<> handle;
      {
        const std::lock_guard lock(mutex_);
        if (queue_.empty()) {
          return count;
        }
        handle = queue_.front();
        queue_.pop_front();
      }
      handle.resume();
      ++count;
    }
  }

  // Waits until a handle is queued, then resumes it. Returns true if it resumed a handle, false
  // if `timeout` passed first. Owner thread only.
  template <class Rep, class Period>
  bool run_one_for(std::chrono::duration<Rep, Period> timeout) {
    std::coroutine_handle<> handle;
    {
      std::unique_lock lock(mutex_);
      if (!ready_.wait_for(lock, timeout, [this] { return !queue_.empty(); })) {
        return false;
      }
      handle = queue_.front();
      queue_.pop_front();
    }
    handle.resume();
    return true;
  }

 private:
  std::thread::id owner_;
  std::mutex mutex_;
  std::condition_variable ready_;
  std::deque<std::coroutine_handle<>> queue_;
};

}  // namespace campfire
