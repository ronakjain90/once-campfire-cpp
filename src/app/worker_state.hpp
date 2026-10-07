// State that one worker owns: its read connection, its session cache and its fragment cache adapter.
// Design: docs/architecture.md section 3 ("Worker" row) and section 6.
#pragma once

#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>

#include "app/app.hpp"
#include "app/fragment_cache.hpp"
#include "app/session_cache.hpp"
#include "db/connection.hpp"

namespace campfire::app {

class WorkerState {
 public:
  // Opens the reader connection and registers the inbox of the worker. Aborts the process if the
  // database cannot open: a worker cannot serve without it.
  explicit WorkerState(const App& app);
  WorkerState(const WorkerState&) = delete;
  WorkerState& operator=(const WorkerState&) = delete;
  ~WorkerState();

  [[nodiscard]] db::Connection& reader() noexcept { return reader_; }
  // Applies the changes that the writer posted, then gives the cache. Call it before each lookup.
  [[nodiscard]] SessionCache& sessions();
  [[nodiscard]] WorkerFragmentCache& fragment_cache() noexcept { return fragment_cache_; }
  [[nodiscard]] ChangeInbox& inbox() noexcept { return inbox_; }
  // The result of a pure function of the key text (a signed id, a signed stream name). It runs `make` on a
  // miss. The table is empty again when it holds 4096 entries. Only the thread of the worker calls it.
  [[nodiscard]] const std::string& memo(std::string_view key, const std::function<std::string()>& make);

 private:
  struct KeyHash {
    using is_transparent = void;
    std::size_t operator()(std::string_view key) const noexcept { return std::hash<std::string_view>{}(key); }
  };
  std::unordered_map<std::string, std::string, KeyHash, std::equal_to<>> memo_;
  std::shared_ptr<ChangeHub> hub_;
  std::shared_ptr<SharedFragmentCache> fragments_;
  db::Connection reader_;
  ChangeInbox inbox_;
  SessionCache sessions_;
  WorkerFragmentCache fragment_cache_;
};

// The state of the calling thread. The first call on a thread builds it from `app()` and installs its
// fragment cache with `views::set_fragment_cache`.
[[nodiscard]] WorkerState& worker_state();

}  // namespace campfire::app
