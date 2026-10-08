// One SQLite connection with a lazily prepared statement array.
// Rust: crates/db/src/database.rs (open_connection), crates/db/src/schema.rs (configure_connection).
#pragma once

#include <sqlite3.h>

#include <cstdint>
#include <memory>
#include <memory_resource>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_map>
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
// Folds one parameter into a scope or a hasher, as `bind_value` does (for the memo key and for a replay).
template <class Sink>
void note_value(Sink& d, std::int64_t v) noexcept {
  d.param_i64(v);
}
template <class Sink>
void note_value(Sink& d, double v) noexcept {
  d.param_f64(v);
}
template <class Sink>
void note_value(Sink& d, std::string_view v) noexcept {
  d.param_text(v);
}
template <class Sink>
void note_value(Sink& d, Blob v) noexcept {
  d.param_blob(v.bytes);
}
template <class Sink>
void note_value(Sink& d, std::nullopt_t) noexcept {
  d.param_null();
}
template <class Sink>
void note_value(Sink& d, int v) noexcept {
  d.param_i64(v);
}
template <class Sink>
void note_value(Sink& d, bool v) noexcept {
  d.param_i64(static_cast<std::int64_t>(v));
}
template <class Sink>
void note_value(Sink& d, const char* v) noexcept {
  d.param_text(v);
}
template <class Sink>
void note_value(Sink& d, const std::string& v) noexcept {
  d.param_text(v);
}
template <class Sink, class T>
void note_value(Sink& d, const std::optional<T>& v) noexcept {
  if (v) {
    note_value(d, *v);
  } else {
    d.param_null();
  }
}

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

  // Read transactions. When they are on, the first statement starts a transaction (`BEGIN`), and
  // the statements after it read the same snapshot. SQLite then takes the WAL read lock one time,
  // not one time for each statement. `end_read_transaction` ends the transaction and the next
  // statement starts a new one. Turn them off to end the transaction and to go back to autocommit.
  // A reader connection uses them for one request (src/app/rq.cpp).
  void set_read_transactions(bool on) noexcept {
    if (!on) {
      end_read_transaction();
    }
    read_transactions_ = on;
  }
  [[nodiscard]] bool read_transactions() const noexcept { return read_transactions_; }
  void end_read_transaction() noexcept {
    if (in_read_transaction_) {
      finish_read_transaction();
    }
  }

  // The result memo of a reader connection. In a read transaction, the rows of a `SELECT` are kept with the
  // `PRAGMA data_version` of the snapshot. That value changes when any other connection commits (this one never
  // writes), so a later transaction with the same value reads the same database: the same statement with the same
  // parameters gives the same rows, and they come from the memo. A replay folds the same bytes into the scope as the
  // statement did, so page keys do not change. A result is not kept if a read converted a column's type, or if the
  // SQL can give another value for the same data (the clock, random values).
  struct MemoStats {
    std::uint64_t hits = 0;
    std::uint64_t misses = 0;
    std::size_t entries = 0;
    std::size_t bytes = 0;
  };
  [[nodiscard]] MemoStats memo_stats() const noexcept { return {memo_hits_, memo_misses_, memo_.size(), memo_bytes_}; }

  // The first row, or nothing.
  template <class Row, class... A>
  [[nodiscard]] Result<std::optional<Row>> first(const Query<Row(A...)>& q, Arena& arena,
                                                 std::type_identity_t<A>... args) {
    std::optional<Row> row;
    auto done = read<Row>(q, arena, ReadMode::First, 1, [&](Row&& r) { row.emplace(std::move(r)); }, args...);
    if (!done) {
      return std::unexpected(done.error());
    }
    return row;
  }

  // All rows, in the arena.
  template <class Row, class... A>
  [[nodiscard]] Result<std::pmr::vector<Row>> all(const Query<Row(A...)>& q, Arena& arena,
                                                  std::type_identity_t<A>... args) {
    std::pmr::vector<Row> rows{arena.resource()};
    auto done = read<Row>(q, arena, ReadMode::All, 0, [&](Row&& r) { rows.push_back(std::move(r)); }, args...);
    if (!done) {
      return std::unexpected(done.error());
    }
    return rows;
  }

  // The first `max_rows` rows, in the arena. Use it instead of `LIMIT ?`: SQLite prepares a statement again when it
  // binds a new value to a `LIMIT` parameter. The statement stops after `max_rows` rows.
  template <class Row, class... A>
  [[nodiscard]] Result<std::pmr::vector<Row>> first_n(const Query<Row(A...)>& q, Arena& arena, std::size_t max_rows,
                                                      std::type_identity_t<A>... args) {
    std::pmr::vector<Row> rows{arena.resource()};
    if (max_rows == 0) {
      return rows;
    }
    auto done =
        read<Row>(q, arena, ReadMode::FirstN, max_rows, [&](Row&& r) { rows.push_back(std::move(r)); }, args...);
    if (!done) {
      return std::unexpected(done.error());
    }
    return rows;
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

  enum class ReadMode : std::uint8_t { First, All, FirstN };

  // The rows of a statement, kept by the memo.
  struct MemoResult {
    std::uint32_t columns = 0;
    std::uint32_t rows = 0;
    bool ended = false;               // the statement ran to its end (the scope got `end_statement`)
    Hash128 digest;                   // `DependencyScope::rows_digest` of the rows and the end
    std::vector<CachedValue> values;  // rows × columns
    std::string bytes;                // text and blob values
    [[nodiscard]] std::span<const CachedValue> row(std::uint32_t r) const noexcept {
      return {values.data() + static_cast<std::size_t>(r) * columns, columns};
    }
    [[nodiscard]] std::size_t cost() const noexcept { return values.size() * sizeof(CachedValue) + bytes.size() + 128; }
  };
  struct MemoKeyHash {
    std::size_t operator()(const Hash128& k) const noexcept { return static_cast<std::size_t>(k.low); }
  };
  static constexpr std::size_t kMemoBudget = std::size_t{16} << 20;

  // Runs `q` and gives each row to `push`: from the memo when it has the result, else from the statement (and keeps
  // the result when it may). `First` stops after one row, `FirstN` after `max_rows` rows.
  template <class Row, class Push, class... A>
  [[nodiscard]] Status read(const QueryBase& q, Arena& arena, ReadMode mode, std::size_t max_rows, Push&& push,
                            const A&... args) {
    // The transaction starts first, so the memo knows the snapshot's version.
    if (read_transactions_ && !in_read_transaction_) {
      start_read_transaction();
    }
    const bool memo = memo_on_ && memo_allowed(q);
    Hash128 key;
    if (memo) {
      key = memo_key(q.index(), mode, max_rows, args...);
      if (const auto it = memo_.find(key); it != memo_.end()) {
        ++memo_hits_;
        replay<Row>(q.index(), it->second, arena, push, args...);
        return {};
      }
      ++memo_misses_;
    }
    auto st = begin(q);
    if (!st) {
      return std::unexpected(st.error());
    }
    if (const int rc = bind_all(*st, 1, args...); rc != SQLITE_OK) {
      return std::unexpected(finish_error(*st, rc, q));
    }
    MemoResult record;
    bool converted = false;
    if (memo) {
      record.columns = static_cast<std::uint32_t>(sqlite3_column_count(*st));
    }
    std::size_t count = 0;
    while (true) {
      const int rc = sqlite3_step(*st);
      if (rc == SQLITE_ROW) {
        fold(*st);
        if (memo) {
          record_row(*st, record);
          RowReader reader(*st, arena, record.row(record.rows - 1), converted);
          push(RowTraits<Row>::read(reader));
        } else {
          RowReader reader(*st, arena);
          push(RowTraits<Row>::read(reader));
        }
        ++count;
        if (mode == ReadMode::First) {
          end(*st, false);
          break;
        }
        if (mode == ReadMode::FirstN && count == max_rows) {
          end(*st, true);
          record.ended = true;
          break;
        }
      } else if (rc == SQLITE_DONE) {
        end(*st, true);
        record.ended = true;
        break;
      } else {
        return std::unexpected(finish_error(*st, rc, q));
      }
    }
    if (memo && !converted && memo_bytes_ + record.cost() <= kMemoBudget) {
      record.digest = DependencyScope::rows_digest(
          [&](Hasher& h) {
            for (std::uint32_t r = 0; r < record.rows; ++r) h.row(record.row(r), record.bytes.data());
          },
          record.ended);
      memo_bytes_ += record.cost();
      memo_.emplace(key, std::move(record));
    }
    return {};
  }

  template <class Row, class Push, class... A>
  void replay(std::uint32_t index, const MemoResult& m, Arena& arena, Push& push, const A&... args) {
    if (scope_ != nullptr) {
      scope_->begin_statement(index);
      (detail::note_value(*scope_, args), ...);
      scope_->kept_rows(m.digest, m.rows);  // the rows and the end, as one kept digest
    }
    for (std::uint32_t r = 0; r < m.rows; ++r) {
      RowReader reader(m.row(r), m.bytes.data(), arena);
      push(RowTraits<Row>::read(reader));
    }
  }

  template <class... A>
  [[nodiscard]] static Hash128 memo_key(std::uint32_t index, ReadMode mode, std::size_t max_rows, const A&... args) {
    Hasher key;
    key.tag('S');
    key.put_u64(index);
    key.param_i64(static_cast<std::int64_t>(mode));
    key.param_i64(static_cast<std::int64_t>(max_rows));
    (detail::note_value(key, args), ...);
    return key.digest();
  }

  // The SQL of `q` gives the same rows for the same data and parameters.
  [[nodiscard]] bool memo_allowed(const QueryBase& q) {
    const std::uint32_t i = q.index();
    if (i >= memo_allowed_.size()) {
      memo_allowed_.resize(static_cast<std::size_t>(i) + 16, 0);
    }
    if (memo_allowed_[i] == 0) {
      memo_allowed_[i] = deterministic_select(q.sql()) ? 1 : -1;
    }
    return memo_allowed_[i] == 1;
  }
  [[nodiscard]] static bool deterministic_select(std::string_view sql) noexcept;
  // Keeps the current row of `st` (its columns as SQLite stored them) in `record`.
  static void record_row(sqlite3_stmt* st, MemoResult& record);

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
    if (read_transactions_ && !in_read_transaction_) {
      start_read_transaction();
    }
    if (scope_ != nullptr) {
      scope_->begin_statement(i);
    }
    return st;
  }
  void start_read_transaction() noexcept;
  void finish_read_transaction() noexcept;
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
  bool read_transactions_ = false;
  bool in_read_transaction_ = false;
  sqlite3_stmt* begin_ = nullptr;         // `BEGIN`, prepared at the first use
  sqlite3_stmt* commit_ = nullptr;        // `COMMIT`, prepared at the first use
  sqlite3_stmt* data_version_ = nullptr;  // `PRAGMA data_version`, prepared at the first use
  // The result memo: the results of the snapshot with `memo_version_`. `memo_on_` while a read transaction runs.
  std::unordered_map<Hash128, MemoResult, MemoKeyHash> memo_;
  std::vector<std::int8_t> memo_allowed_;  // by statement index: 0 not known yet, 1 allowed, -1 not allowed
  std::int64_t memo_version_ = -1;
  std::size_t memo_bytes_ = 0;
  std::uint64_t memo_hits_ = 0;
  std::uint64_t memo_misses_ = 0;
  bool memo_on_ = false;
};

// Makes the error of a failed SQLite call. A constraint failure is `InvalidArgument`, a busy
// database is `Timeout`, other failures are `Internal`. The message starts with the SQLite name
// of the code.
[[nodiscard]] Error sqlite_error(sqlite3* db, int rc, std::string_view context);

}  // namespace campfire::db
