// Request context. Rails: ActionController::Base (request, params, response). Design: architecture section 4.
#pragma once

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <string_view>
#include <type_traits>
#include <utility>

#include "core/arena.hpp"
#include "core/scheduler.hpp"
#include "core/task.hpp"
#include "net/http.hpp"
#include "net/response.hpp"
#include "net/thread_pool.hpp"

namespace campfire::net {

// The path parameters of a matched route. The values are views of the request path. They are
// raw: percent escapes are not decoded.
class PathParams {
 public:
  static constexpr std::size_t kMax = 8;

  // Adds a parameter. Returns false if the table is full.
  bool add(std::string_view name, std::string_view value) noexcept {
    if (count_ == kMax) return false;
    items_[count_++] = {name, value};
    return true;
  }
  // The value of `name`, or an empty view. `has` tells apart an empty value from no value.
  [[nodiscard]] std::string_view get(std::string_view name) const noexcept {
    for (std::size_t i = 0; i < count_; ++i) {
      if (items_[i].name == name) return items_[i].value;
    }
    return {};
  }
  [[nodiscard]] bool has(std::string_view name) const noexcept {
    for (std::size_t i = 0; i < count_; ++i) {
      if (items_[i].name == name) return true;
    }
    return false;
  }
  [[nodiscard]] std::size_t size() const noexcept { return count_; }
  [[nodiscard]] Header at(std::size_t i) const noexcept { return {items_[i].name, items_[i].value}; }
  void clear() noexcept { count_ = 0; }

 private:
  std::array<Header, kMax> items_{};
  std::size_t count_ = 0;
};

// One for each request. The connection owns it. It lives until the response is written.
class Ctx {
 public:
  Ctx(Scheduler& scheduler, Arena& arena, const Request& request) noexcept
      : scheduler_(&scheduler), arena_(&arena), request_(&request), started_(std::chrono::steady_clock::now()) {}
  Ctx(const Ctx&) = delete;
  Ctx& operator=(const Ctx&) = delete;

  [[nodiscard]] const Request& request() const noexcept { return *request_; }
  [[nodiscard]] Arena& arena() noexcept { return *arena_; }
  [[nodiscard]] Scheduler& scheduler() noexcept { return *scheduler_; }
  [[nodiscard]] std::pmr::memory_resource* resource() noexcept { return arena_->resource(); }

  PathParams params;

  // A new response in the arena of the request.
  [[nodiscard]] Response response(int status = 200) { return Response(resource(), status); }

  // Seconds since the request started. Rack::Runtime (x-runtime) reads it.
  [[nodiscard]] double elapsed_seconds() const noexcept {
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - started_).count();
  }

  // Extension slots. T7 puts the read connection of the worker in `db`. T6 puts the session in
  // `session`. The server never reads them.
  void* db = nullptr;
  void* session = nullptr;

  // Runs `fn` on a thread of `pool` and resumes this coroutine on the thread of the worker when
  // `fn` ends. `auto v = co_await ctx.offload(pool, [&] { return work(); });`. If `fn` throws, the
  // exception comes out of `co_await`. `fn` may use data of this request, because the coroutine
  // waits: do not touch the arena of the request from any other thread meanwhile.
  template <class F>
    requires std::invocable<F&>
  [[nodiscard]] auto offload(ThreadPool& pool, F fn) {
    using R = std::invoke_result_t<F&>;
    auto pair = make_completion<R>(*scheduler_);
    pool.submit([fn = std::move(fn), setter = pair.second]() mutable {
      try {
        if constexpr (std::is_void_v<R>) {
          fn();
          setter.set_value();
        } else {
          setter.set_value(fn());
        }
      } catch (...) {
        setter.set_exception(std::current_exception());
      }
    });
    return std::move(pair.first);
  }

 private:
  Scheduler* scheduler_;
  Arena* arena_;
  const Request* request_;
  std::chrono::steady_clock::time_point started_;
};

// A request handler. Handlers are coroutines.
using HandlerFn = Task<Response> (*)(Ctx&);

}  // namespace campfire::net
