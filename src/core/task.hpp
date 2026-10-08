// Task<T>: the coroutine type of request handlers. Design: docs/architecture.md section 4.
#pragma once

#include <atomic>
#include <cassert>
#include <chrono>
#include <coroutine>
#include <exception>
#include <memory>
#include <optional>
#include <type_traits>
#include <utility>

#include "core/scheduler.hpp"

namespace campfire {

// A lazy, move-only, single-threaded coroutine. The coroutine does not run until someone starts
// it: `co_await task` (inside another Task), or `start()` (the root of a chain).
//
// Rules:
// - A Task and all Tasks that it awaits run on one thread, the thread of its Scheduler. A
//   `co_await` of a Task does not change the thread (symmetric transfer, no queue).
// - A thread change happens only in `Completion<T>`. It posts the handle back to the Scheduler.
// - An exception in the coroutine goes to the awaiter. For the root, `result()` throws it.
// - The Task owns the coroutine frame. The destructor of the Task destroys the frame. Do not
//   destroy a Task while it waits for a `Completion` that is not done: the other thread would
//   resume a dead frame. T5 owns this rule for connections.
template <class T>
class Task;

// Work that a thread does when a coroutine on it waits for other work, and when the coroutine
// continues on the same thread. A worker uses it to end its read transaction (src/app/worker_state.cpp).
// `suspend` gives a state, and `resume` gets the same state back.
// `Completion` calls the hook on each `co_await`, also when the result is already there: the other
// thread did work (for example, a COMMIT), so a snapshot from before the `co_await` can be stale.
// `Yield` calls it too. An awaiter that moves a coroutine to a different thread does not call it.
struct SuspendHook {
  void* context = nullptr;
  bool (*suspend)(void* context) noexcept = nullptr;
  void (*resume)(void* context, bool state) noexcept = nullptr;
};
inline thread_local SuspendHook suspend_hook;

template <class T>
class Task;

namespace detail {

inline bool before_suspend() noexcept {
  const SuspendHook& hook = suspend_hook;
  return hook.suspend != nullptr && hook.suspend(hook.context);
}
inline void after_resume(bool state) noexcept {
  const SuspendHook& hook = suspend_hook;
  if (hook.resume != nullptr) {
    hook.resume(hook.context, state);
  }
}

class PromiseBase {
 public:
  std::suspend_always initial_suspend() noexcept { return {}; }

  struct FinalAwaiter {
    bool await_ready() noexcept { return false; }
    template <class P>
    std::coroutine_handle<> await_suspend(std::coroutine_handle<P> self) noexcept {
      // Symmetric transfer: continue the awaiter with no growth of the stack.
      std::coroutine_handle<> next = self.promise().continuation_;
      return next ? next : std::noop_coroutine();
    }
    void await_resume() noexcept {}
  };
  FinalAwaiter final_suspend() noexcept { return {}; }

  void unhandled_exception() noexcept { exception_ = std::current_exception(); }

  void set_continuation(std::coroutine_handle<> next) noexcept { continuation_ = next; }

 protected:
  void rethrow_if_failed() const {
    if (exception_) {
      std::rethrow_exception(exception_);
    }
  }

 private:
  std::coroutine_handle<> continuation_;
  std::exception_ptr exception_;
};

template <class T>
class Promise final : public PromiseBase {
 public:
  Task<T> get_return_object() noexcept;
  template <class U>
  void return_value(U&& value) {
    value_.emplace(std::forward<U>(value));
  }
  T take() {
    rethrow_if_failed();
    assert(value_.has_value());
    return std::move(*value_);
  }

 private:
  std::optional<T> value_;
};

template <>
class Promise<void> final : public PromiseBase {
 public:
  Task<void> get_return_object() noexcept;
  void return_void() noexcept {}
  void take() { rethrow_if_failed(); }
};

}  // namespace detail

template <class T>
class [[nodiscard]] Task {
 public:
  using promise_type = detail::Promise<T>;
  using Handle = std::coroutine_handle<promise_type>;

  Task() noexcept = default;
  explicit Task(Handle handle) noexcept : handle_(handle) {}
  Task(const Task&) = delete;
  Task& operator=(const Task&) = delete;
  Task(Task&& other) noexcept : handle_(std::exchange(other.handle_, {})) {}
  Task& operator=(Task&& other) noexcept {
    if (this != &other) {
      reset();
      handle_ = std::exchange(other.handle_, {});
    }
    return *this;
  }
  ~Task() { reset(); }

  [[nodiscard]] bool valid() const noexcept { return static_cast<bool>(handle_); }
  [[nodiscard]] bool done() const noexcept { return handle_ && handle_.done(); }

  // Runs the coroutine from the start, on the calling thread (the owner), until it finishes or
  // waits. Use it on the root Task only. Do not call it on a Task that another Task awaits.
  void start() {
    assert(handle_ && !handle_.done());
    handle_.resume();
  }

  // The result of a finished root Task. Throws the exception of the coroutine, if there was one.
  T result() {
    assert(done());
    return handle_.promise().take();
  }

  // `co_await std::move(task)` or `co_await function_that_returns_a_task()`.
  auto operator co_await() && noexcept {
    struct Awaiter {
      Handle handle;
      bool await_ready() const noexcept { return !handle || handle.done(); }
      std::coroutine_handle<> await_suspend(std::coroutine_handle<> awaiting) noexcept {
        handle.promise().set_continuation(awaiting);
        return handle;
      }
      T await_resume() { return handle.promise().take(); }
    };
    return Awaiter{handle_};
  }

 private:
  void reset() noexcept {
    if (handle_) {
      handle_.destroy();
      handle_ = {};
    }
  }
  Handle handle_;
};

namespace detail {
template <class T>
Task<T> Promise<T>::get_return_object() noexcept {
  return Task<T>(std::coroutine_handle<Promise<T>>::from_promise(*this));
}
inline Task<void> Promise<void>::get_return_object() noexcept {
  return Task<void>(std::coroutine_handle<Promise<void>>::from_promise(*this));
}

// The state that a `Completion` and its `Promise` share.
template <class T>
struct CompletionState {
  explicit CompletionState(Scheduler& owner) : scheduler(&owner) {}
  Scheduler* scheduler;
  // 0: nobody yet. 1: a coroutine waits (`waiter` is set). 2: the result is set.
  std::atomic<int> phase{0};
  std::coroutine_handle<> waiter;
  std::optional<T> value;
  std::exception_ptr error;
};
template <>
struct CompletionState<void> {
  explicit CompletionState(Scheduler& owner) : scheduler(&owner) {}
  Scheduler* scheduler;
  std::atomic<int> phase{0};
  std::coroutine_handle<> waiter;
  std::exception_ptr error;
};
}  // namespace detail

template <class T>
class Completion;
template <class T>
class CompletionSetter;
template <class T>
[[nodiscard]] std::pair<Completion<T>, CompletionSetter<T>> make_completion(Scheduler& owner);

// The awaitable end of a hand-off between threads. A coroutine on the owner thread does
// `T value = co_await completion;`. Another thread (a job pool, the writer) calls
// `set_value` or `set_exception` on the matching `CompletionSetter`. Then the Scheduler resumes
// the coroutine on the owner thread. If the result is set before the `co_await`, the coroutine
// does not suspend.
template <class T>
class Completion {
 public:
  Completion(const Completion&) = delete;
  Completion& operator=(const Completion&) = delete;
  Completion(Completion&&) noexcept = default;
  Completion& operator=(Completion&&) noexcept = default;

  // Calls the suspend hook first, also if the result is ready and the coroutine does not suspend.
  bool await_ready() noexcept {
    hook_state_ = detail::before_suspend();
    return state_->phase.load(std::memory_order_acquire) == 2;
  }

  bool await_suspend(std::coroutine_handle<> awaiting) noexcept {
    state_->waiter = awaiting;
    int expected = 0;
    // If the result arrived between `await_ready` and now, resume at once (return false).
    return state_->phase.compare_exchange_strong(expected, 1, std::memory_order_acq_rel, std::memory_order_acquire);
  }

  T await_resume() {
    assert(state_->scheduler->on_owner_thread());
    detail::after_resume(hook_state_);
    if (state_->error) {
      std::rethrow_exception(state_->error);
    }
    if constexpr (!std::is_void_v<T>) {
      return std::move(*state_->value);
    }
  }

 private:
  template <class U>
  friend std::pair<Completion<U>, CompletionSetter<U>> make_completion(Scheduler&);
  explicit Completion(std::shared_ptr<detail::CompletionState<T>> state) : state_(std::move(state)) {}
  std::shared_ptr<detail::CompletionState<T>> state_;
  bool hook_state_ = false;  // set by `await_ready`: the state for the suspend hook
};

// The producer end of a `Completion`. Any thread can use it, once. It is copyable and holds the
// shared state alive.
template <class T>
class CompletionSetter {
 public:
  template <class U = T>
    requires(!std::is_void_v<T>)
  void set_value(U&& value) const {
    state_->value.emplace(std::forward<U>(value));
    finish();
  }
  void set_value() const
    requires std::is_void_v<T>
  {
    finish();
  }
  void set_exception(std::exception_ptr error) const {
    state_->error = std::move(error);
    finish();
  }

 private:
  template <class U>
  friend std::pair<Completion<U>, CompletionSetter<U>> make_completion(Scheduler&);
  explicit CompletionSetter(std::shared_ptr<detail::CompletionState<T>> state) : state_(std::move(state)) {}

  void finish() const {
    // The release makes the value visible to the owner thread.
    const int before = state_->phase.exchange(2, std::memory_order_acq_rel);
    if (before == 1) {
      // `waiter` was set before phase 1 (release), and this exchange acquires it. Copy the
      // handle first: after `post` the owner can finish and free everything.
      const std::coroutine_handle<> waiter = state_->waiter;
      Scheduler* scheduler = state_->scheduler;
      scheduler->post(waiter);
    }
  }
  std::shared_ptr<detail::CompletionState<T>> state_;
};

// Makes a hand-off. `owner` is the Scheduler of the coroutine that will await the `Completion`.
template <class T>
[[nodiscard]] std::pair<Completion<T>, CompletionSetter<T>> make_completion(Scheduler& owner) {
  auto state = std::make_shared<detail::CompletionState<T>>(owner);
  return {Completion<T>(state), CompletionSetter<T>(state)};
}

// An awaitable that gives the thread back to the Scheduler: the coroutine resumes later, from
// the queue of `scheduler`. Use it to let other work run.
class Yield {
 public:
  explicit Yield(Scheduler& scheduler) noexcept : scheduler_(&scheduler) {}
  bool await_ready() const noexcept { return false; }
  void await_suspend(std::coroutine_handle<> awaiting) {
    hook_state_ = detail::before_suspend();
    scheduler_->post(awaiting);
  }
  void await_resume() const noexcept { detail::after_resume(hook_state_); }

 private:
  Scheduler* scheduler_;
  bool hook_state_ = false;
};

}  // namespace campfire
