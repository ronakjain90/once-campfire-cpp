// Rust: crates/db/src/database.rs (open_connection), crates/db/src/schema.rs (configure_connection).
#include "db/connection.hpp"

#include <utility>

namespace campfire::db {

namespace {

// `timeout: 5000` in config/database.yml.
constexpr int kBusyTimeoutMs = 5000;

Error make_error(sqlite3* db, int rc, std::string_view context) {
  Errc code = Errc::Internal;
  const int primary = rc & 0xff;
  if (primary == SQLITE_CONSTRAINT) {
    code = Errc::InvalidArgument;
  } else if (primary == SQLITE_BUSY || primary == SQLITE_LOCKED) {
    code = Errc::Timeout;
  }
  const char* text = db != nullptr ? sqlite3_errmsg(db) : sqlite3_errstr(rc);
  std::string message = std::string(sqlite3_errstr(rc)) + " (" + std::to_string(rc) + "): " + (text ? text : "");
  if (!context.empty()) {
    message += " [";
    message += context;
    message += "]";
  }
  return Error{code, std::move(message)};
}

}  // namespace

Error sqlite_error(sqlite3* db, int rc, std::string_view context) {
  return make_error(db, rc, context);
}

namespace detail {
int bind_value(sqlite3_stmt* s, int i, std::int64_t v, DependencyScope* d) noexcept {
  if (d != nullptr) {
    d->param_i64(v);
  }
  return sqlite3_bind_int64(s, i, v);
}
int bind_value(sqlite3_stmt* s, int i, double v, DependencyScope* d) noexcept {
  if (d != nullptr) {
    d->param_f64(v);
  }
  return sqlite3_bind_double(s, i, v);
}
int bind_value(sqlite3_stmt* s, int i, std::string_view v, DependencyScope* d) noexcept {
  if (d != nullptr) {
    d->param_text(v);
  }
  // A null pointer would bind NULL: an empty view is the empty text.
  return sqlite3_bind_text64(s, i, v.empty() ? "" : v.data(), v.size(), SQLITE_STATIC, SQLITE_UTF8);
}
int bind_value(sqlite3_stmt* s, int i, Blob v, DependencyScope* d) noexcept {
  if (d != nullptr) {
    d->param_blob(v.bytes);
  }
  return sqlite3_bind_blob64(s, i, v.bytes.empty() ? "" : v.bytes.data(), v.bytes.size(), SQLITE_STATIC);
}
int bind_value(sqlite3_stmt* s, int i, std::nullopt_t, DependencyScope* d) noexcept {
  if (d != nullptr) {
    d->param_null();
  }
  return sqlite3_bind_null(s, i);
}
}  // namespace detail

void Connection::Closer::operator()(sqlite3* db) const noexcept {
  sqlite3_close(db);
}

Result<Connection> Connection::open(const std::string& path, Role role) {
  sqlite3* db = nullptr;
  const int flags = SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_NOMUTEX | SQLITE_OPEN_URI;
  int rc = sqlite3_open_v2(path.c_str(), &db, flags, nullptr);
  if (rc != SQLITE_OK) {
    Error error = make_error(db, rc, "open " + path);
    sqlite3_close(db);
    return std::unexpected(std::move(error));
  }
  Connection conn(db, role);
  sqlite3_busy_timeout(db, kBusyTimeoutMs);
  // The pragmas of `configure_connection`, in the same order. `mmap_size` stays off.
  for (const char* pragma : {"PRAGMA foreign_keys = 1", "PRAGMA journal_mode = wal", "PRAGMA synchronous = normal",
                             "PRAGMA journal_size_limit = 67108864", "PRAGMA cache_size = 2000"}) {
    if (auto status = conn.exec_sql(pragma); !status) {
      return std::unexpected(status.error());
    }
  }
  if (role == Role::Reader) {
    if (auto status = conn.exec_sql("PRAGMA query_only = 1"); !status) {
      return std::unexpected(status.error());
    }
  }
  if (role == Role::Writer) {
    // The writer notes the WAL size itself (see database.cpp). SQLite must not checkpoint.
    if (auto status = conn.exec_sql("PRAGMA wal_autocheckpoint = 0"); !status) {
      return std::unexpected(status.error());
    }
  }
  return conn;
}

Connection::Connection(Connection&& other) noexcept
    : db_(std::exchange(other.db_, nullptr)),
      role_(other.role_),
      scope_(other.scope_),
      stmts_(std::move(other.stmts_)),
      prepares_(other.prepares_) {
  other.stmts_.clear();
}

Connection& Connection::operator=(Connection&& other) noexcept {
  if (this != &other) {
    for (sqlite3_stmt* st : stmts_) {
      sqlite3_finalize(st);
    }
    Closer{}(db_);
    db_ = std::exchange(other.db_, nullptr);
    role_ = other.role_;
    scope_ = other.scope_;
    stmts_ = std::move(other.stmts_);
    other.stmts_.clear();
    prepares_ = other.prepares_;
  }
  return *this;
}

Connection::~Connection() {
  for (sqlite3_stmt* st : stmts_) {
    sqlite3_finalize(st);
  }
  Closer{}(db_);
}

Status Connection::exec_sql(const std::string& sql) {
  char* message = nullptr;
  const int rc = sqlite3_exec(db_, sql.c_str(), nullptr, nullptr, &message);
  if (rc != SQLITE_OK) {
    Error error = make_error(db_, rc, sql);
    sqlite3_free(message);
    return std::unexpected(std::move(error));
  }
  return {};
}

Result<sqlite3_stmt*> Connection::prepare_slow(const QueryBase& q) {
  sqlite3_stmt* st = nullptr;
  const std::string_view sql = q.sql();
  const int rc = sqlite3_prepare_v3(db_, sql.data(), static_cast<int>(sql.size()), SQLITE_PREPARE_PERSISTENT, &st,
                                    nullptr);
  if (rc != SQLITE_OK) {
    return std::unexpected(make_error(db_, rc, sql));
  }
  if (q.index() >= stmts_.size()) {
    stmts_.resize(static_cast<std::size_t>(q.index()) + 16, nullptr);
  }
  stmts_[q.index()] = st;
  ++prepares_;
  return st;
}

Error Connection::finish_error(sqlite3_stmt* st, int rc, const QueryBase& q) {
  Error error = make_error(db_, rc, q.sql());
  sqlite3_reset(st);
  sqlite3_clear_bindings(st);
  return error;
}

}  // namespace campfire::db
