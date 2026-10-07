// Rails: config/database.yml (immediate transactions), ActiveRecord after_commit.
// Rust: crates/db/src/database.rs (writer, checkpointer, WAL limits).
#include "db/database.hpp"

#include <algorithm>

#include "core/time_format.hpp"

namespace campfire::db {

namespace {

// A statement that has no result and no parameters. Runs on a connection that only one thread uses.
bool run_plain(Connection& conn, const char* sql) noexcept {
  return sqlite3_exec(conn.handle(), sql, nullptr, nullptr, nullptr) == SQLITE_OK;
}

// The statements of a batch (BEGIN, SAVEPOINT, RELEASE, COMMIT), prepared once. `sqlite3_exec` would parse each of them
// again for each write.
class PlainStatements {
 public:
  explicit PlainStatements(Connection& conn) noexcept : conn_(conn) {}
  PlainStatements(const PlainStatements&) = delete;
  PlainStatements& operator=(const PlainStatements&) = delete;
  ~PlainStatements() {
    for (const Entry& entry : entries_) sqlite3_finalize(entry.stmt);
  }

  // `sql` is a string literal: the pointer is the key.
  bool run(const char* sql) noexcept {
    sqlite3_stmt* stmt = nullptr;
    for (const Entry& entry : entries_) {
      if (entry.sql == sql) stmt = entry.stmt;
    }
    if (stmt == nullptr) {
      if (sqlite3_prepare_v3(conn_.handle(), sql, -1, SQLITE_PREPARE_PERSISTENT, &stmt, nullptr) != SQLITE_OK)
        return false;
      entries_.push_back({sql, stmt});
    }
    int rc = sqlite3_step(stmt);
    while (rc == SQLITE_ROW) rc = sqlite3_step(stmt);
    sqlite3_reset(stmt);
    return rc == SQLITE_DONE;
  }

 private:
  struct Entry {
    const char* sql;
    sqlite3_stmt* stmt;
  };
  Connection& conn_;
  std::vector<Entry> entries_;
};

}  // namespace

std::string Tx::now_db() const {
  return format_db(now());
}

Database::Database(std::string path, DatabaseOptions options, Connection writer, Connection checkpointer)
    : path_(std::move(path)),
      options_(std::move(options)),
      clock_(options_.clock ? options_.clock : std::make_shared<SystemClock>()),
      writer_(std::move(writer)),
      checkpointer_(std::move(checkpointer)) {}

Result<std::unique_ptr<Database>> Database::open(const std::string& path, DatabaseOptions options) {
  auto writer = Connection::open(path, Role::Writer);
  if (!writer) {
    return std::unexpected(writer.error());
  }
  auto checkpointer = Connection::open(path, Role::Checkpointer);
  if (!checkpointer) {
    return std::unexpected(checkpointer.error());
  }
  options.group_size = std::max<std::size_t>(options.group_size, 1);
  std::unique_ptr<Database> db(new Database(path, std::move(options), std::move(*writer), std::move(*checkpointer)));
  // Replaces the automatic checkpoint: the hook only notes the WAL size (Rust: `note_wal_size`).
  sqlite3_wal_hook(db->writer_.handle(), &Database::wal_hook, db.get());
  db->checkpointer_thread_ = std::thread([self = db.get()] { self->checkpointer_loop(); });
  db->writer_thread_ = std::thread([self = db.get()] { self->writer_loop(); });
  return db;
}

Database::~Database() {
  {
    const std::lock_guard lock(queue_mutex_);
    stopping_ = true;
  }
  queue_ready_.notify_all();
  writer_thread_.join();
  {
    const std::lock_guard lock(checkpoint_mutex_);
    checkpoint_stop_ = true;
  }
  checkpoint_ready_.notify_all();
  checkpointer_thread_.join();
}

int Database::wal_hook(void* self, sqlite3*, const char*, int pages) noexcept {
  static_cast<Database*>(self)->wal_pages_ = pages;
  return SQLITE_OK;
}

void Database::submit(Job job) {
  {
    std::unique_lock lock(queue_mutex_);
    if (!stopping_) {
      queue_.push_back(std::move(job));
      lock.unlock();
      queue_ready_.notify_one();
      return;
    }
  }
  job.finish(Error{Errc::Internal, "the database is closing"}, {});
}

std::uint64_t Database::subscribe(ChangeSubscriber fn) {
  const std::unique_lock lock(subscribers_mutex_);
  const std::uint64_t id = next_subscriber_++;
  subscribers_.emplace_back(id, std::move(fn));
  return id;
}

void Database::unsubscribe(std::uint64_t id) {
  const std::unique_lock lock(subscribers_mutex_);
  std::erase_if(subscribers_, [id](const auto& entry) { return entry.first == id; });
}

void Database::publish(std::span<const Change> changes) {
  if (changes.empty()) {
    return;
  }
  const std::shared_lock lock(subscribers_mutex_);
  for (const auto& entry : subscribers_) {
    entry.second(changes);
  }
}

DatabaseStats Database::stats() const noexcept {
  return {commits_.load(), writes_.load(), passive_.load(), restart_.load()};
}

void Database::run_checkpoint(Connection& conn, const char* sql) {
  // `PRAGMA wal_checkpoint` reports a checkpoint that could not finish in column 0 (`busy`),
  // not as an error.
  int busy = 0;
  const int rc = sqlite3_exec(
      conn.handle(), sql,
      [](void* out, int, char** values, char**) {
        *static_cast<int*>(out) = values[0] != nullptr ? std::atoi(values[0]) : 0;
        return 0;
      },
      &busy, nullptr);
  if (rc != SQLITE_OK) {
    log_warn("WAL checkpoint failed: {}", sqlite3_errmsg(conn.handle()));
  } else if (busy != 0) {
    log_warn("WAL checkpoint could not finish: {}", sql);
  }
}

void Database::checkpointer_loop() {
  while (true) {
    {
      std::unique_lock lock(checkpoint_mutex_);
      checkpoint_ready_.wait(lock, [this] { return checkpoint_due_ || checkpoint_stop_; });
      if (checkpoint_stop_) {
        return;
      }
      checkpoint_due_ = false;
    }
    const std::lock_guard running(running_mutex_);
    run_checkpoint(checkpointer_, "PRAGMA wal_checkpoint(PASSIVE)");
    passive_.fetch_add(1, std::memory_order_relaxed);
  }
}

// The checkpoint rules of the Rust port: wake the checkpointer for each 1,000 pages of growth,
// and RESTART on the writer connection at the WAL limit.
void Database::after_batch() {
  const int pages = std::exchange(wal_pages_, 0);
  if (pages == 0) {
    return;
  }
  if (pages >= options_.wal_limit_pages) {
    const std::lock_guard running(running_mutex_);
    run_checkpoint(writer_, "PRAGMA wal_checkpoint(RESTART)");
    restart_.fetch_add(1, std::memory_order_relaxed);
    woken_at_ = 0;
    return;
  }
  if (pages < woken_at_) {
    woken_at_ = 0;  // the WAL restarted
  }
  if (pages - woken_at_ >= options_.autocheckpoint_pages) {
    {
      const std::lock_guard lock(checkpoint_mutex_);
      checkpoint_due_ = true;
    }
    checkpoint_ready_.notify_one();
    woken_at_ = pages;
  }
}

void Database::writer_loop() {
  std::vector<Job> batch;
  std::vector<Tx> txs;
  std::vector<char> ok;
  std::vector<Change> changes;
  PlainStatements plain(writer_);
  while (true) {
    batch.clear();
    {
      std::unique_lock lock(queue_mutex_);
      queue_ready_.wait(lock, [this] { return !queue_.empty() || stopping_; });
      if (queue_.empty()) {
        return;  // stopping, and nothing is left
      }
      while (!queue_.empty() && batch.size() < options_.group_size) {
        batch.push_back(std::move(queue_.front()));
        queue_.pop_front();
      }
    }
    txs.clear();
    ok.assign(batch.size(), 0);
    changes.clear();
    std::optional<Error> batch_error;
    if (!plain.run("BEGIN IMMEDIATE")) {
      batch_error = sqlite_error(writer_.handle(), sqlite3_errcode(writer_.handle()), "BEGIN IMMEDIATE");
    }
    for (std::size_t i = 0; i < batch.size(); ++i) {
      txs.emplace_back(writer_, *clock_);
      if (batch_error) {
        continue;
      }
      writes_.fetch_add(1, std::memory_order_relaxed);
      if (i == 0) {
        // Nothing is in the transaction yet, so the transaction is the scope of the first write. A failure
        // rolls it back and starts it again. This saves the savepoint, and the full-text index flush that
        // each savepoint makes.
        ok[i] = batch[i].run(txs[i]) ? 1 : 0;
        if (ok[i] == 0 && plain.run("ROLLBACK") && !plain.run("BEGIN IMMEDIATE")) {
          batch_error = sqlite_error(writer_.handle(), sqlite3_errcode(writer_.handle()), "BEGIN IMMEDIATE");
        }
        continue;
      }
      plain.run("SAVEPOINT cf_write");
      ok[i] = batch[i].run(txs[i]) ? 1 : 0;
      if (ok[i] == 0) {
        plain.run("ROLLBACK TO cf_write");
      }
      plain.run("RELEASE cf_write");
    }
    if (!batch_error) {
      if (plain.run("COMMIT")) {
        commits_.fetch_add(1, std::memory_order_relaxed);
      } else {
        batch_error = sqlite_error(writer_.handle(), sqlite3_errcode(writer_.handle()), "COMMIT");
        run_plain(writer_, "ROLLBACK");
      }
    }
    // The changes of the writes that committed, in order. They go out before the writes finish,
    // so a worker never sees a result with a stale cache.
    if (!batch_error) {
      for (std::size_t i = 0; i < batch.size(); ++i) {
        if (ok[i] != 0) {
          changes.insert(changes.end(), txs[i].changes_.begin(), txs[i].changes_.end());
        }
      }
      publish(changes);
    }
    after_batch();
    for (std::size_t i = 0; i < batch.size(); ++i) {
      if (batch_error) {
        batch[i].finish(batch_error, {});
      } else if (ok[i] != 0) {
        batch[i].finish(std::nullopt, std::move(txs[i].after_commit_));
      } else {
        batch[i].finish(std::nullopt, {});
      }
    }
  }
}

}  // namespace campfire::db
