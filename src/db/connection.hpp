// One SQLite connection with a lazily prepared statement array.
// Rust: crates/db/src/database.rs (open_connection), crates/db/src/schema.rs (configure_connection).
#pragma once

#include <sqlite3.h>

#include <cstdint>
#include <memory>
#include <memory_resource>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

#include "core/arena.hpp"
#include "core/error.hpp"
#include "db/row.hpp"
#include "db/scope.hpp"
#include "db/statement.hpp"

namespace campfire::db {

enum class Role : std::uint8_t {
  Writer,        // read and write; the writer thread owns it
  Reader,        // `query_only`; a worker owns it
  Checkpointer,  // only `PRAGMA wal_checkpoint`
};

namespace detail {
// Binds one value. The text and the blob stay valid until the statement finishes (SQLITE_STATIC).
int bind_value(sqlite3_stmt* s, int i, std::int64_t v, DependencyScope* d) noexcept;
int bind_value(sqlite3_stmt* s, int i, double v, DependencyScope* d) noexcept;
int bind_value(sqlite3_stmt* s, int i, std::string_view v, DependencyScope* d) noexcept;
int bind_value(sqlite3_stmt* s, int i, Blob v, DependencyScope* d) noexcept;
int bind_value(sqlite3_stmt* s, int i, std::nullopt_t, DependencyScope* d) noexcept;
inline int bind_value(sqlite3_stmt* s, int i, int v, DependencyScope* d) noexcept {
  return bind_value(s, i, static_cast<std::int64_t>(v), d);
}
inline int bind_value(sqlite3_stmt* s, int i, bool v, DependencyScope* d) noexcept {
  return bind_value(s, i, static_cast<std::int64_t>(v), d);
}
inline int bind_value(sqlite3_stmt* s, int i, const char* v, DependencyScope* d) noexcept {
  return bind_value(s, i, std::string_view(v), d);
}
inline int bind_value(sqlite3_stmt* s, int i, const std::string& v, DependencyScope* d) noexcept {
  return bind_value(s, i, std::string_view(v), d);
}
template <class T>
int bind_value(sqlite3_stmt* s, int i, const std::optional<T>& v, DependencyScope* d) noexcept {
  return v ? bind_value(s, i, *v, d) : bind_value(s, i, std::nullopt, d);
}
}  // namespace detail

class Connection {
 public:
  // Opens the file and applies the pragmas of the Rust port (`configure_connection`).
  [[nodiscard]] static Result<Connection> open(const std::string& path, Role role);

  Connection(Connection&&) noexcept;
  Connection& operator=(Connection&&) noexcept;
  Connection(const Connection&) = delete;
  Connection& operator=(const Connection&) = delete;
  ~Connection();

  [[nodiscard]] sqlite3* handle() const noexcept { return db_; }
  [[nodiscard]] Role role() const noexcept { return role_; }

  // The scope that folds the statements of this connection (reader connections of a request).
  void set_scope(DependencyScope* scope) noexcept { scope_ = scope; }
  [[nodiscard]] DependencyScope* scope() const noexcept { return scope_; }

  // The first row, or nothing.
  template <class Row, class... A>
  [[nodiscard]] Result<std::optional<Row>> first(const Query<Row(A...)>& q, Arena& arena,
                                                 std::type_identity_t<A>... args) {
    auto st = begin(q);
    if (!st) {
      return std::unexpected(st.error());
    }
    if (const int rc = bind_all(*st, 1, args...); rc != SQLITE_OK) {
      return std::unexpected(finish_error(*st, rc, q));
    }
    const int rc = sqlite3_step(*st);
    if (rc == SQLITE_ROW) {
      fold(*st);
      RowReader reader(*st, arena);
      std::optional<Row> row(RowTraits<Row>::read(reader));
      end(*st, false);
      return row;
    }
    if (rc == SQLITE_DONE) {
      end(*st, true);
      return std::optional<Row>{};
    }
    return std::unexpected(finish_error(*st, rc, q));
  }

  // All rows, in the arena.
  template <class Row, class... A>
  [[nodiscard]] Result<std::pmr::vector<Row>> all(const Query<Row(A...)>& q, Arena& arena,
                                                  std::type_identity_t<A>... args) {
    std::pmr::vector<Row> rows{arena.resource()};
    auto st = begin(q);
    if (!st) {
      return std::unexpected(st.error());
    }
    if (const int rc = bind_all(*st, 1, args...); rc != SQLITE_OK) {
      return std::unexpected(finish_error(*st, rc, q));
    }
    while (true) {
      const int rc = sqlite3_step(*st);
      if (rc == SQLITE_ROW) {
        fold(*st);
        RowReader reader(*st, arena);
        rows.push_back(RowTraits<Row>::read(reader));
      } else if (rc == SQLITE_DONE) {
        end(*st, true);
        return rows;
      } else {
        return std::unexpected(finish_error(*st, rc, q));
      }
    }
  }

  // Runs a statement that returns no row. Gives the number of rows changed.
  template <class... A>
  [[nodiscard]] Result<std::int64_t> exec(const Query<void(A...)>& q, std::type_identity_t<A>... args) {
    auto st = begin(q);
    if (!st) {
      return std::unexpected(st.error());
    }
    if (const int rc = bind_all(*st, 1, args...); rc != SQLITE_OK) {
      return std::unexpected(finish_error(*st, rc, q));
    }
    int rc = sqlite3_step(*st);
    while (rc == SQLITE_ROW) {  // a statement with RETURNING: ignore the rows
      rc = sqlite3_step(*st);
    }
    if (rc != SQLITE_DONE) {
      return std::unexpected(finish_error(*st, rc, q));
    }
    const std::int64_t changes = sqlite3_changes64(db_);
    end(*st, true);
    return changes;
  }

  // Runs SQL text that has no parameters, one or more statements. Not prepared, not tracked.
  [[nodiscard]] Status exec_sql(const std::string& sql);

  [[nodiscard]] std::int64_t last_insert_rowid() const noexcept { return sqlite3_last_insert_rowid(db_); }

  // How many statements this connection prepared. A second use of a statement does not add to it.
  [[nodiscard]] std::uint64_t prepare_count() const noexcept { return prepares_; }

 private:
  Connection(sqlite3* db, Role role) noexcept : db_(db), role_(role) {}

  [[nodiscard]] Result<sqlite3_stmt*> prepare_slow(const QueryBase& q);
  [[nodiscard]] Result<sqlite3_stmt*> begin(const QueryBase& q) {
    const std::uint32_t i = q.index();
    sqlite3_stmt* st = i < stmts_.size() ? stmts_[i] : nullptr;
    if (st == nullptr) {
      auto prepared = prepare_slow(q);
      if (!prepared) {
        return prepared;
      }
      st = *prepared;
    }
    if (scope_ != nullptr) {
      scope_->begin_statement(i);
    }
    return st;
  }
  void fold(sqlite3_stmt* st) noexcept {
    if (scope_ != nullptr) {
      scope_->fold_row(st);
    }
  }
  // Resets the statement. `done` is true when the result is complete.
  void end(sqlite3_stmt* st, bool done) noexcept {
    if (done && scope_ != nullptr) {
      scope_->end_statement();
    }
    sqlite3_reset(st);
    sqlite3_clear_bindings(st);
  }
  [[nodiscard]] Error finish_error(sqlite3_stmt* st, int rc, const QueryBase& q);

  template <class T, class... Rest>
  int bind_all(sqlite3_stmt* st, int i, const T& v, const Rest&... rest) noexcept {
    if (const int rc = detail::bind_value(st, i, v, scope_); rc != SQLITE_OK) {
      return rc;
    }
    return bind_all(st, i + 1, rest...);
  }
  int bind_all(sqlite3_stmt*, int) noexcept { return SQLITE_OK; }

  struct Closer {
    void operator()(sqlite3* db) const noexcept;
  };
  sqlite3* db_ = nullptr;
  Role role_ = Role::Reader;
  DependencyScope* scope_ = nullptr;
  std::vector<sqlite3_stmt*> stmts_;
  std::uint64_t prepares_ = 0;
};

// Makes the error of a failed SQLite call. A constraint failure is `InvalidArgument`, a busy
// database is `Timeout`, other failures are `Internal`. The message starts with the SQLite name
// of the code.
[[nodiscard]] Error sqlite_error(sqlite3* db, int rc, std::string_view context);

}  // namespace campfire::db
