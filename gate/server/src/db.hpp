// SQLite access: connections with prepared-statement caches, row sets, tracing.
#pragma once
#include <sqlite3.h>

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace gate {

enum Q : int {
#define X(n, s) Q_##n,
#include "sql_texts.inc"
#include "sql_extra.inc"
#undef X
  Q_COUNT
};

struct Arg {
  enum Kind : uint8_t { INT, TEXT, NUL } kind;
  int64_t i = 0;
  std::string_view s;
  Arg(int64_t v) : kind(INT), i(v) {}
  Arg(int v) : kind(INT), i(v) {}
  Arg(std::string_view v) : kind(TEXT), s(v) {}
  Arg(const std::string& v) : kind(TEXT), s(v) {}
  Arg(const char* v) : kind(TEXT), s(v) {}
  Arg(std::nullptr_t) : kind(NUL) {}
};

enum CellType : uint8_t { C_NULL = 0, C_INT = 1, C_TEXT = 2, C_BLOB = 3 };
struct Cell {
  int64_t i = 0;
  uint32_t off = 0, len = 0;
  uint8_t type = C_NULL;
};

// All rows of a statement; every column of every row is read. Text lives in one arena.
struct RowSet {
  uint32_t ncols = 0;
  size_t nrows = 0;
  std::vector<Cell> cells;
  std::string arena;
  void clear() { ncols = 0; nrows = 0; cells.clear(); arena.clear(); }
  const Cell& at(size_t r, size_t c) const { return cells[r * ncols + c]; }
  std::string_view text(size_t r, size_t c) const { const Cell& x = at(r, c); return std::string_view(arena).substr(x.off, x.len); }
  int64_t num(size_t r, size_t c) const { return at(r, c).i; }
  bool null(size_t r, size_t c) const { return at(r, c).type == C_NULL; }
};

class Conn {
 public:
  ~Conn();
  // role: "reader" | "writer" | "checkpointer". Applies the Rust app's pragmas.
  bool open(const std::string& path, const char* role, bool query_only, bool trace);
  // Runs a cached prepared statement to completion, reading all rows. Returns the sqlite rc (SQLITE_OK or error).
  int run(Q q, std::initializer_list<Arg> args, RowSet& out);
  int exec(const char* sql);  // uncached, traced
  sqlite3* db() { return db_; }
  const char* role() const { return role_; }
  std::string error() const { return db_ ? sqlite3_errmsg(db_) : "no db"; }

 private:
  sqlite3* db_ = nullptr;
  const char* role_ = "";
  bool trace_ = false;
  sqlite3_stmt* stmts_[Q_COUNT] = {};
};

extern const char* const kSql[Q_COUNT];
int trace_thread_id();
void set_trace_thread_id(int id);

// time helpers
int64_t now_us();
void fmt_us(int64_t us, char out[27]);  // "YYYY-MM-DD HH:MM:SS.ffffff"

}  // namespace gate
