// A coroutine that nobody awaits: it starts at once and ends by itself. The channels use it for the writes that
// the client does not wait for (presence). Rails: ActiveRecord callbacks that run inside `subscribed`.
#pragma once

#include <coroutine>
#include <exception>
#include <utility>

#include "core/log.hpp"
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

// Runs `task` on the calling thread until it waits, then lets it go on without an owner. The frame of `task` stays
// alive until it ends: do not stop the worker thread before the writer answers (the app stops the server first,
// then the database).
inline Detached run_detached(Task<void> task) {
  co_await std::move(task);
}

}  // namespace campfire::app::channels
