// The tracked dependency scope of a request. Design: docs/architecture.md section 6.1.
// A cacheable page key comes from the data that the page read: this scope folds it into XXH3-128.
#pragma once

#include <sqlite3.h>

#include <array>
#include <cstdint>
#include <cstring>
#include <span>
#include <string_view>

#include "core/xxh3.hpp"

namespace campfire::db {

// One column of a row that the result memo of a connection keeps (src/db/connection.hpp): its storage type and its
// value as SQLite stored it. Text and blob bytes are in the memo's byte buffer, at `offset`.
struct CachedValue {
  std::int64_t i = 0;
  double f = 0;
  std::uint32_t offset = 0;
  std::uint32_t size = 0;
  int type = SQLITE_NULL;
};

// Collects the dependencies of one request. A connection that has a scope folds into it: the
// index of each statement, the value of each parameter, every returned row (the raw bytes of
// each column), and the end of the result. The handler adds facets, and can mark the page as
// not cacheable. Not thread-safe: one request, one thread.
class DependencyScope {
 public:
  DependencyScope() = default;

  // Starts again for a new request. Keeps the hash state.
  void reset() noexcept {
    state_.reset();
    size_ = 0;
    uncacheable_ = false;
    statements_ = 0;
    rows_ = 0;
  }

  // A value that is not from SQL but changes the page: the host, the browser class, the format,
  // the `updated_at` of the current user.
  void facet(std::string_view name, std::string_view value) noexcept {
    tag('F');
    put_bytes(name);
    put_bytes(value);
  }
  void facet(std::string_view name, std::uint64_t value) noexcept {
    tag('G');
    put_bytes(name);
    put_u64(value);
  }

  // The page reads something that is not tracked (for example the clock). The page must not be
  // cached.
  void mark_uncacheable() noexcept { uncacheable_ = true; }
  [[nodiscard]] bool cacheable() const noexcept { return !uncacheable_; }

  // The key. It is the same for the same statements, parameters, rows and facets, in this order.
  [[nodiscard]] Hash128 key() noexcept {
    flush();
    return state_.digest();
  }

  [[nodiscard]] std::uint64_t statement_count() const noexcept { return statements_; }
  [[nodiscard]] std::uint64_t row_count() const noexcept { return rows_; }

  // Calls from the connection.
  void begin_statement(std::uint32_t index) noexcept {
    ++statements_;
    tag('S');
    put_u64(index);
  }
  void param_null() noexcept { tag('n'); }
  void param_i64(std::int64_t v) noexcept {
    tag('i');
    put_u64(static_cast<std::uint64_t>(v));
  }
  void param_f64(double v) noexcept {
    tag('f');
    std::uint64_t bits = 0;
    std::memcpy(&bits, &v, sizeof bits);
    put_u64(bits);
  }
  void param_text(std::string_view v) noexcept {
    tag('t');
    put_bytes(v);
  }
  void param_blob(std::string_view v) noexcept {
    tag('b');
    put_bytes(v);
  }
  void end_statement() noexcept { tag('E'); }

  // Folds a row that the result memo kept: the same bytes as `fold_row` folds for the same row.
  void fold_cached_row(std::span<const CachedValue> row, const char* bytes) noexcept {
    ++rows_;
    tag('R');
    for (const CachedValue& v : row) {
      switch (v.type) {
        case SQLITE_INTEGER: param_i64(v.i); break;
        case SQLITE_FLOAT: param_f64(v.f); break;
        case SQLITE_TEXT: param_text({bytes + v.offset, v.size}); break;
        case SQLITE_BLOB: param_blob({bytes + v.offset, v.size}); break;
        default: param_null(); break;
      }
    }
  }

  // Folds the current row of `stmt`. Call it before the reader converts a column.
  void fold_row(sqlite3_stmt* stmt) noexcept {
    ++rows_;
    tag('R');
    const int count = sqlite3_column_count(stmt);
    for (int i = 0; i < count; ++i) {
      switch (sqlite3_column_type(stmt, i)) {
        case SQLITE_INTEGER: param_i64(sqlite3_column_int64(stmt, i)); break;
        case SQLITE_FLOAT: param_f64(sqlite3_column_double(stmt, i)); break;
        case SQLITE_TEXT: {
          const auto* p = reinterpret_cast<const char*>(sqlite3_column_text(stmt, i));
          param_text({p, static_cast<std::size_t>(sqlite3_column_bytes(stmt, i))});
          break;
        }
        case SQLITE_BLOB: {
          const auto* p = static_cast<const char*>(sqlite3_column_blob(stmt, i));
          param_blob({p, static_cast<std::size_t>(sqlite3_column_bytes(stmt, i))});
          break;
        }
        default: param_null(); break;
      }
    }
  }

 private:
  static constexpr std::size_t kBuffer = 2048;

  void tag(char c) noexcept {
    if (size_ == kBuffer) {
      flush();
    }
    buffer_[size_++] = c;
  }
  void put_u64(std::uint64_t v) noexcept {
    if (kBuffer - size_ < 8) {
      flush();
    }
    std::memcpy(&buffer_[size_], &v, 8);  // the byte order of the host is fine: the key stays in memory
    size_ += 8;
  }
  void put_bytes(std::string_view v) noexcept {
    put_u64(v.size());
    if (v.size() <= kBuffer - size_) {
      if (!v.empty()) {
        std::memcpy(&buffer_[size_], v.data(), v.size());
      }
      size_ += v.size();
      return;
    }
    flush();
    if (v.size() <= kBuffer) {
      std::memcpy(buffer_.data(), v.data(), v.size());
      size_ = v.size();
    } else {
      state_.update(v);
    }
  }
  void flush() noexcept {
    if (size_ != 0) {
      state_.update({buffer_.data(), size_});
      size_ = 0;
    }
  }

  Xxh3State state_;
  std::array<char, kBuffer> buffer_{};
  std::size_t size_ = 0;
  bool uncacheable_ = false;
  std::uint64_t statements_ = 0;
  std::uint64_t rows_ = 0;
};

}  // namespace campfire::db
