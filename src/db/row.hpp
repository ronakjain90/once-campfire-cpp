// Reads the columns of the current row of a prepared statement into the request arena.
// Rails: ActiveRecord result rows. Rust: crates/db/src/sql.rs (row reads by position).
#pragma once

#include <sqlite3.h>

#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

#include "core/arena.hpp"
#include "db/scope.hpp"

namespace campfire::db {

// A view of the current row. Text and blob values are copied into the arena, so they stay valid
// after the statement steps on. Columns are read by position, in the order of the SELECT list.
//
// The reader also reads a row that the result memo kept (src/db/connection.hpp). It gives the same values as SQLite
// for reads that need no type conversion. A row that the memo records is read from the statement, and each read is
// checked against the column's storage type: a read that would convert the type marks the record as not reusable.
class RowReader {
 public:
  RowReader(sqlite3_stmt* stmt, Arena& arena) noexcept : stmt_(stmt), arena_(&arena) {}
  // A row from the statement that the memo records: `types` are the storage types of its columns.
  RowReader(sqlite3_stmt* stmt, Arena& arena, std::span<const CachedValue> types, bool& converted) noexcept
      : stmt_(stmt), arena_(&arena), cached_(types), converted_(&converted) {}
  // A row that the memo kept.
  RowReader(std::span<const CachedValue> row, const char* bytes, Arena& arena) noexcept
      : arena_(&arena), cached_(row), bytes_(bytes) {}

  [[nodiscard]] bool is_null(int col) const noexcept {
    if (stmt_ == nullptr) return value(col).type == SQLITE_NULL;
    return sqlite3_column_type(stmt_, col) == SQLITE_NULL;
  }
  [[nodiscard]] std::int64_t i64(int col) const noexcept {
    if (stmt_ == nullptr) return value(col).i;
    check(col, SQLITE_INTEGER, SQLITE_INTEGER);
    return sqlite3_column_int64(stmt_, col);
  }
  [[nodiscard]] bool boolean(int col) const noexcept { return i64(col) != 0; }
  [[nodiscard]] double f64(int col) const noexcept {
    if (stmt_ == nullptr) {
      const CachedValue& v = value(col);
      return v.type == SQLITE_INTEGER ? static_cast<double>(v.i) : v.f;  // SQLite converts an integer the same way
    }
    check(col, SQLITE_FLOAT, SQLITE_INTEGER);
    return sqlite3_column_double(stmt_, col);
  }
  // NULL gives an empty view.
  [[nodiscard]] std::string_view text(int col) const {
    if (stmt_ == nullptr) return cached_bytes(col);
    check(col, SQLITE_TEXT, SQLITE_BLOB);
    const auto* data = reinterpret_cast<const char*>(sqlite3_column_text(stmt_, col));
    const int size = sqlite3_column_bytes(stmt_, col);
    return data == nullptr ? std::string_view{} : arena_->copy({data, static_cast<std::size_t>(size)});
  }
  [[nodiscard]] std::string_view blob(int col) const {
    if (stmt_ == nullptr) return cached_bytes(col);
    check(col, SQLITE_BLOB, SQLITE_TEXT);
    const void* data = sqlite3_column_blob(stmt_, col);
    const int size = sqlite3_column_bytes(stmt_, col);
    return data == nullptr ? std::string_view{}
                           : arena_->copy({static_cast<const char*>(data), static_cast<std::size_t>(size)});
  }

  [[nodiscard]] std::optional<std::int64_t> i64_opt(int col) const noexcept {
    return is_null(col) ? std::nullopt : std::optional<std::int64_t>(i64(col));
  }
  [[nodiscard]] std::optional<bool> boolean_opt(int col) const noexcept {
    return is_null(col) ? std::nullopt : std::optional<bool>(boolean(col));
  }
  [[nodiscard]] std::optional<double> f64_opt(int col) const noexcept {
    return is_null(col) ? std::nullopt : std::optional<double>(f64(col));
  }
  [[nodiscard]] std::optional<std::string_view> text_opt(int col) const {
    return is_null(col) ? std::nullopt : std::optional<std::string_view>(text(col));
  }

  [[nodiscard]] Arena& arena() const noexcept { return *arena_; }

 private:
  [[nodiscard]] const CachedValue& value(int col) const noexcept { return cached_[static_cast<std::size_t>(col)]; }
  // A kept text or blob: NULL gives an empty view, as `sqlite3_column_text` gives a null pointer.
  [[nodiscard]] std::string_view cached_bytes(int col) const {
    const CachedValue& v = value(col);
    if (v.type == SQLITE_NULL) return {};
    return arena_->copy({bytes_ + v.offset, v.size});
  }
  // While the memo records: a read of another storage type than `a` or `b` (or NULL) converts the value.
  void check(int col, int a, int b) const noexcept {
    if (converted_ == nullptr) return;
    const int type = value(col).type;
    if (type != SQLITE_NULL && type != a && type != b) *converted_ = true;
  }

  sqlite3_stmt* stmt_ = nullptr;
  Arena* arena_;
  std::span<const CachedValue> cached_;
  const char* bytes_ = nullptr;
  bool* converted_ = nullptr;
};

// How a row type reads itself. A struct with `static Row read(RowReader&)` works at once (the
// generated row structs do). The scalar types read column 0.
template <class Row>
struct RowTraits {
  static Row read(RowReader& r) { return Row::read(r); }
};
template <>
struct RowTraits<std::int64_t> {
  static std::int64_t read(RowReader& r) { return r.i64(0); }
};
template <>
struct RowTraits<double> {
  static double read(RowReader& r) { return r.f64(0); }
};
template <>
struct RowTraits<std::string_view> {
  static std::string_view read(RowReader& r) { return r.text(0); }
};
template <>
struct RowTraits<std::optional<std::int64_t>> {
  static std::optional<std::int64_t> read(RowReader& r) { return r.i64_opt(0); }
};
template <>
struct RowTraits<std::optional<std::string_view>> {
  static std::optional<std::string_view> read(RowReader& r) { return r.text_opt(0); }
};

}  // namespace campfire::db
