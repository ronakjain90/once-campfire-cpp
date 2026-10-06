// Reads the columns of the current row of a prepared statement into the request arena.
// Rails: ActiveRecord result rows. Rust: crates/db/src/sql.rs (row reads by position).
#pragma once

#include <sqlite3.h>

#include <cstdint>
#include <optional>
#include <string_view>

#include "core/arena.hpp"

namespace campfire::db {

// A view of the current row. Text and blob values are copied into the arena, so they stay valid
// after the statement steps on. Columns are read by position, in the order of the SELECT list.
class RowReader {
 public:
  RowReader(sqlite3_stmt* stmt, Arena& arena) noexcept : stmt_(stmt), arena_(&arena) {}

  [[nodiscard]] bool is_null(int col) const noexcept { return sqlite3_column_type(stmt_, col) == SQLITE_NULL; }
  [[nodiscard]] std::int64_t i64(int col) const noexcept { return sqlite3_column_int64(stmt_, col); }
  [[nodiscard]] bool boolean(int col) const noexcept { return sqlite3_column_int64(stmt_, col) != 0; }
  [[nodiscard]] double f64(int col) const noexcept { return sqlite3_column_double(stmt_, col); }
  // NULL gives an empty view.
  [[nodiscard]] std::string_view text(int col) const {
    const auto* data = reinterpret_cast<const char*>(sqlite3_column_text(stmt_, col));
    const int size = sqlite3_column_bytes(stmt_, col);
    return data == nullptr ? std::string_view{} : arena_->copy({data, static_cast<std::size_t>(size)});
  }
  [[nodiscard]] std::string_view blob(int col) const {
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
  sqlite3_stmt* stmt_;
  Arena* arena_;
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
