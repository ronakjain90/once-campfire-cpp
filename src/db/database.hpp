// The database: one writer thread with group commit, one checkpointer thread, reader connections.
// Rails: config/database.yml (immediate transactions, 5000 ms timeout), ActiveRecord after_commit.
// Rust: crates/db/src/database.rs. Design: docs/architecture.md section 5.
#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <span>
#include <string>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

#include "core/clock.hpp"
#include "core/error.hpp"
#include "core/log.hpp"
#include "core/scheduler.hpp"
#include "core/task.hpp"
#include "db/connection.hpp"
#include "db/schema.gen.hpp"

namespace campfire::db {

// A row that a write changed. The writer publishes these after the commit.
struct Change {
  schema::Table table;
  std::int64_t id;
  [[nodiscard]] bool operator==(const Change&) const = default;
};

// A write in progress, on the writer thread. A write does SQL only: it must not render, do I/O or
// wait.
class Tx {
 public:
  Tx(Connection& conn, const Clock& clock) noexcept : conn_(&conn), clock_(&clock) {}

  [[nodiscard]] Connection& conn() noexcept { return *conn_; }
  // `Time.current`, at the microseconds that a `datetime(6)` column keeps.
  [[nodiscard]] Timestamp now() const { return clock_->now(); }
  // The same time as the text that Rails writes to the database.
  [[nodiscard]] std::string now_db() const;

  // Queues `fn`. It runs on the worker that sent the write, after the COMMIT, in the order of the
  // calls. If the write fails, the queue is dropped.
  void after_commit(std::function<void()> fn) { after_commit_.push_back(std::move(fn)); }
  // Records a change. The writer publishes it after the COMMIT.
  void changed(schema::Table table, std::int64_t id) { changes_.push_back({table, id}); }

 private:
  friend class Database;
  Connection* conn_;
  const Clock* clock_;
  std::vector<std::function<void()>> after_commit_;
  std::vector<Change> changes_;
};

struct DatabaseOptions {
  // Group commit: at most this many writes in one transaction.
  std::size_t group_size = 64;
  // The WAL size that wakes the checkpointer, for each step of growth (SQLite default).
  int autocheckpoint_pages = 1000;
  // The WAL size at which the writer runs a RESTART checkpoint itself (Rust: WAL_LIMIT_PAGES).
  int wal_limit_pages = 10'000;
  // The source of `Time.current`. Empty means the system clock.
  SharedClock clock;
};

struct DatabaseStats {
  std::uint64_t commits = 0;  // COMMIT statements of the writer
  std::uint64_t writes = 0;   // writes that were run, failed or not
  std::uint64_t passive_checkpoints = 0;
  std::uint64_t restart_checkpoints = 0;
};

using ChangeSubscriber = std::function<void(std::span<const Change>)>;

namespace detail {
template <class T>
struct ResultValue {
  using type = T;
  static constexpr bool kIsResult = false;
};
template <class T>
struct ResultValue<std::expected<T, Error>> {
  using type = T;
  static constexpr bool kIsResult = true;
};
}  // namespace detail

class Database {
 public:
  // Opens the file (it is created if it is not there), starts the writer and the checkpointer.
  [[nodiscard]] static Result<std::unique_ptr<Database>> open(const std::string& path, DatabaseOptions options = {});
  Database(const Database&) = delete;
  Database& operator=(const Database&) = delete;
  ~Database();

  // A new `query_only` connection. A worker owns it.
  [[nodiscard]] Result<Connection> open_reader() const { return Connection::open(path_, Role::Reader); }
  [[nodiscard]] const std::string& path() const noexcept { return path_; }

  // Runs `fn(Tx&)` on the writer thread, in a SAVEPOINT of a group transaction. `fn` returns a
  // value, or a `Result<value>`. An error (or an exception) rolls back this write only. The task
  // gives the value of `fn` on the thread of `scheduler`, and then runs the after-commit queue
  // there. `fn` must stay valid until the task ends. A write never blocks the calling thread.
  template <class F>
  [[nodiscard]] auto write(Scheduler& scheduler, F fn)
      -> Task<Result<typename detail::ResultValue<std::invoke_result_t<F&, Tx&>>::type>>;

  // Subscribers run on the writer thread, after the COMMIT, before the writes finish.
  // They must be fast and must not call `subscribe` or `unsubscribe`.
  std::uint64_t subscribe(ChangeSubscriber fn);
  void unsubscribe(std::uint64_t id);

  [[nodiscard]] DatabaseStats stats() const noexcept;

 private:
  struct Job {
    std::function<bool(Tx&)> run;  // false: roll back this write
    // Gets the commit error (if any) and the after-commit queue.
    std::function<void(std::optional<Error>, std::vector<std::function<void()>>)> finish;
  };

  Database(std::string path, DatabaseOptions options, Connection writer, Connection checkpointer);
  void submit(Job job);
  void writer_loop();
  void checkpointer_loop();
  void after_batch();
  void publish(std::span<const Change> changes);
  void run_checkpoint(Connection& conn, const char* sql);
  static int wal_hook(void* self, sqlite3*, const char*, int pages) noexcept;

  std::string path_;
  DatabaseOptions options_;
  SharedClock clock_;
  Connection writer_;
  Connection checkpointer_;

  std::mutex queue_mutex_;
  std::condition_variable queue_ready_;
  std::deque<Job> queue_;
  bool stopping_ = false;

  std::mutex checkpoint_mutex_;
  std::condition_variable checkpoint_ready_;
  bool checkpoint_due_ = false;
  bool checkpoint_stop_ = false;
  std::mutex running_mutex_;  // held while a checkpoint runs: one at a time

  int wal_pages_ = 0;  // writer thread only (set by the WAL hook)
  int woken_at_ = 0;   // writer thread only

  std::shared_mutex subscribers_mutex_;
  std::vector<std::pair<std::uint64_t, ChangeSubscriber>> subscribers_;
  std::uint64_t next_subscriber_ = 1;

  std::atomic<std::uint64_t> commits_{0};
  std::atomic<std::uint64_t> writes_{0};
  std::atomic<std::uint64_t> passive_{0};
  std::atomic<std::uint64_t> restart_{0};

  std::thread writer_thread_;
  std::thread checkpointer_thread_;
};

template <class F>
auto Database::write(Scheduler& scheduler, F fn)
    -> Task<Result<typename detail::ResultValue<std::invoke_result_t<F&, Tx&>>::type>> {
  using Raw = std::invoke_result_t<F&, Tx&>;
  using Value = typename detail::ResultValue<Raw>::type;
  using Out = Result<Value>;
  using AfterQueue = std::vector<std::function<void()>>;

  std::optional<Out> result;
  auto [completion, setter] = make_completion<AfterQueue>(scheduler);
  // The frame of this coroutine keeps `fn` and `result` alive: the task waits for the writer.
  submit(Job{[&fn, &result](Tx& tx) -> bool {
               try {
                 if constexpr (std::is_void_v<Raw>) {
                   fn(tx);
                   result.emplace();
                 } else {
                   result.emplace(fn(tx));
                 }
                 return result->has_value();
               } catch (const std::exception& e) {
                 result.emplace(std::unexpected(Error{Errc::Internal, std::string("write threw: ") + e.what()}));
               } catch (...) {
                 result.emplace(std::unexpected(Error{Errc::Internal, "write threw"}));
               }
               return false;
             },
             [&result, setter](std::optional<Error> error, AfterQueue after) {
               if (error) {
                 result.emplace(std::unexpected(std::move(*error)));
               }
               setter.set_value(std::move(after));
             }});
  AfterQueue after = co_await std::move(completion);
  for (auto& item : after) {
    try {
      item();
    } catch (const std::exception& e) {
      log_error("after_commit failed: {}", e.what());
    }
  }
  co_return std::move(*result);
}

}  // namespace campfire::db
