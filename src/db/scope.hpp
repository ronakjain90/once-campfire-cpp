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

// A buffered XXH3-128 hash with the encodings of the scope: a tag byte before each value, lengths before bytes.
class Hasher {
 public:
  Hasher() = default;
  void reset() noexcept {
    state_.reset();
    size_ = 0;
  }
  [[nodiscard]] Hash128 digest() noexcept {
    flush();
    return state_.digest();
  }

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
  void put_hash(const Hash128& h) noexcept {
    put_u64(h.low);
    put_u64(h.high);
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

  // A row of a statement, from the statement (call it before the reader converts a column) or from the memo. Both
  // give the same bytes for the same row.
  void row(sqlite3_stmt* stmt) noexcept {
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
  void row(std::span<const CachedValue> row, const char* bytes) noexcept {
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

 private:
  static constexpr std::size_t kBuffer = 2048;

  void flush() noexcept {
    if (size_ != 0) {
      state_.update({buffer_.data(), size_});
      size_ = 0;
    }
  }

  Xxh3State state_;
  std::array<char, kBuffer> buffer_;  // not zeroed: only the first `size_` bytes are read
  std::size_t size_ = 0;
};

// Collects the dependencies of one request. A connection that has a scope folds into it: the
// index of each statement, the value of each parameter, the rows of the statement, and the end of
// the result. The rows of a statement (and its end) go in as one digest, so a result from the
// connection's memo adds its kept digest and does not hash its rows again. The handler adds
// facets, and can mark the page as not cacheable. Not thread-safe: one request, one thread.
class DependencyScope {
 public:
  DependencyScope() = default;

  // Starts again for a new request. Keeps the hash state.
  void reset() noexcept {
    main_.reset();
    rows_.reset();
    open_ = Open::None;
    uncacheable_ = false;
    statements_ = 0;
    rows_count_ = 0;
  }

  // A value that is not from SQL but changes the page: the host, the browser class, the format,
  // the `updated_at` of the current user.
  void facet(std::string_view name, std::string_view value) noexcept {
    close();
    main_.tag('F');
    main_.put_bytes(name);
    main_.put_bytes(value);
  }
  void facet(std::string_view name, std::uint64_t value) noexcept {
    close();
    main_.tag('G');
    main_.put_bytes(name);
    main_.put_u64(value);
  }

  // The page reads something that is not tracked (for example the clock). The page must not be
  // cached.
  void mark_uncacheable() noexcept { uncacheable_ = true; }
  [[nodiscard]] bool cacheable() const noexcept { return !uncacheable_; }

  // The key. It is the same for the same statements, parameters, rows and facets, in this order.
  [[nodiscard]] Hash128 key() noexcept {
    close();
    return main_.digest();
  }

  [[nodiscard]] std::uint64_t statement_count() const noexcept { return statements_; }
  [[nodiscard]] std::uint64_t row_count() const noexcept { return rows_count_; }

  // Calls from the connection: the statement, its parameters, its rows and its end.
  void begin_statement(std::uint32_t index) noexcept {
    close();
    ++statements_;
    main_.tag('S');
    main_.put_u64(index);
    open_ = Open::Rows;
  }
  void param_null() noexcept { main_.param_null(); }
  void param_i64(std::int64_t v) noexcept { main_.param_i64(v); }
  void param_f64(double v) noexcept { main_.param_f64(v); }
  void param_text(std::string_view v) noexcept { main_.param_text(v); }
  void param_blob(std::string_view v) noexcept { main_.param_blob(v); }
  void fold_row(sqlite3_stmt* stmt) noexcept {
    ++rows_count_;
    rows_.row(stmt);
  }
  void end_statement() noexcept { rows_.tag('E'); }
  // A statement's rows and end from the memo: `digest` is what `rows_digest` gave for them.
  void kept_rows(const Hash128& digest, std::uint64_t rows) noexcept {
    rows_count_ += rows;
    kept_ = digest;
    open_ = Open::Kept;
  }

  // The digest of rows (and of the end, if `ended`), as a statement's rows go into the scope.
  template <class Rows>
  [[nodiscard]] static Hash128 rows_digest(const Rows& rows, bool ended) noexcept {
    Hasher h;
    rows(h);
    if (ended) {
      h.tag('E');
    }
    return h.digest();
  }

 private:
  enum class Open : std::uint8_t { None, Rows, Kept };

  // Puts the digest of the open statement's rows into the key.
  void close() noexcept {
    if (open_ == Open::None) {
      return;
    }
    main_.tag('D');
    if (open_ == Open::Rows) {
      main_.put_hash(rows_.digest());
      rows_.reset();
    } else {
      main_.put_hash(kept_);
    }
    open_ = Open::None;
  }

  Hasher main_;
  Hasher rows_;
  Hash128 kept_;
  Open open_ = Open::None;
  bool uncacheable_ = false;
  std::uint64_t statements_ = 0;
  std::uint64_t rows_count_ = 0;
};

}  // namespace campfire::db
