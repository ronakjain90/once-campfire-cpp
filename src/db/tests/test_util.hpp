// Helpers for the database tests: a temporary database file, a test table, a Task runner.
#pragma once

#include <doctest.h>
#include <unistd.h>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <string>

#include "core/arena.hpp"
#include "core/scheduler.hpp"
#include "core/task.hpp"
#include "db/connection.hpp"
#include "db/database.hpp"

namespace campfire::db::testing {

// A directory that goes away with the object.
class TempDir {
 public:
  TempDir() {
    const char* base = std::getenv("CAMPFIRE_TEST_DB_DIR");
    path_ = std::filesystem::path(base != nullptr ? base : std::filesystem::temp_directory_path().string()) /
            ("cf-db-" + std::to_string(::getpid()) + "-" + std::to_string(counter_++));
    std::filesystem::create_directories(path_);
  }
  TempDir(const TempDir&) = delete;
  TempDir& operator=(const TempDir&) = delete;
  ~TempDir() {
    std::error_code ignored;
    std::filesystem::remove_all(path_, ignored);
  }
  [[nodiscard]] std::string file(const std::string& name) const { return (path_ / name).string(); }

 private:
  static inline int counter_ = 0;
  std::filesystem::path path_;
};

struct ItemRow {
  std::int64_t id;
  std::string_view name;
  std::optional<std::int64_t> n;
  static ItemRow read(RowReader& r) { return {r.i64(0), r.text(1), r.i64_opt(2)}; }
};

inline const Query<void(std::string_view, std::optional<std::int64_t>)> InsertItem{
    "INSERT INTO items (name, n) VALUES (?, ?)"};
inline const Query<void(std::int64_t, std::string_view)> BadInsertItem{"INSERT INTO items (id, name) VALUES (?, ?)"};
inline const Query<ItemRow(std::int64_t)> ItemById{"SELECT id, name, n FROM items WHERE id = ?"};
inline const Query<ItemRow(std::int64_t)> ItemsAbove{"SELECT id, name, n FROM items WHERE id > ? ORDER BY id"};
inline const Query<void(std::string_view, std::int64_t)> RenameItem{"UPDATE items SET name = ? WHERE id = ?"};
inline const Query<std::int64_t()> CountItems{"SELECT COUNT(*) FROM items"};

inline void create_items_table(Database& db) {
  Connection conn = *Connection::open(db.path(), Role::Writer);
  REQUIRE(conn.exec_sql("CREATE TABLE IF NOT EXISTS items (id INTEGER PRIMARY KEY, name TEXT NOT NULL, n INTEGER)"));
}

// Runs a root task on `scheduler` until it ends.
template <class T>
T run_task(QueueScheduler& scheduler, Task<T> task) {
  task.start();
  while (!task.done()) {
    scheduler.run_one_for(std::chrono::seconds(10));
  }
  return task.result();
}

}  // namespace campfire::db::testing
