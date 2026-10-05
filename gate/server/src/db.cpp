#include "db.hpp"

#include <time.h>
#include <unistd.h>

#include <atomic>
#include <cstdio>
#include <cstring>

namespace gate {

const char* const kSql[Q_COUNT] = {
#define X(n, s) s,
#include "sql_texts.inc"
#include "sql_extra.inc"
#undef X
};

static thread_local int t_tid = 0;
int trace_thread_id() { return t_tid; }
void set_trace_thread_id(int id) { t_tid = id; }

int64_t now_us() {
  timespec ts;
  clock_gettime(CLOCK_REALTIME, &ts);
  return (int64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}

void fmt_us(int64_t us, char out[27]) {
  int64_t secs = us / 1000000;
  int frac = (int)(us % 1000000);
  int64_t days = secs / 86400;
  int rem = (int)(secs % 86400);
  // civil from days
  int64_t z = days + 719468;
  int64_t era = (z >= 0 ? z : z - 146096) / 146097;
  unsigned doe = (unsigned)(z - era * 146097);
  unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  int64_t y = (int64_t)yoe + era * 400;
  unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  unsigned mp = (5 * doy + 2) / 153;
  unsigned d = doy - (153 * mp + 2) / 5 + 1;
  unsigned m = mp < 10 ? mp + 3 : mp - 9;
  y += m <= 2;
  snprintf(out, 27, "%04d-%02u-%02u %02d:%02d:%02d.%06d", (int)y, m, d, rem / 3600, rem % 3600 / 60, rem % 60, frac);
}

static int trace_cb(unsigned type, void* ctx, void* p, void* x) {
  if (type != SQLITE_TRACE_STMT) return 0;
  Conn* c = (Conn*)ctx;
  const char* xs = (const char*)x;
  char* exp = nullptr;
  const char* text = xs;
  if (!(xs && xs[0] == '-' && xs[1] == '-')) {
    exp = sqlite3_expanded_sql((sqlite3_stmt*)p);
    if (exp) text = exp;
  }
  char line[8192];
  int n = snprintf(line, sizeof line - 1, "SQLTRACE start %s ThreadId(%d) %s\n", c->role(), trace_thread_id(), text ? text : "");
  if (n > (int)sizeof line - 1) n = sizeof line - 1;
  (void)!write(2, line, n);
  if (exp) sqlite3_free(exp);
  return 0;
}

Conn::~Conn() {
  for (auto*& s : stmts_) if (s) sqlite3_finalize(s);
  if (db_) sqlite3_close(db_);
}

bool Conn::open(const std::string& path, const char* role, bool query_only, bool trace) {
  role_ = role;
  trace_ = trace;
  int flags = SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_NOMUTEX | SQLITE_OPEN_URI;
  if (sqlite3_open_v2(path.c_str(), &db_, flags, nullptr) != SQLITE_OK) return false;
  // schema::configure_connection of the Rust app
  sqlite3_busy_timeout(db_, 5000);
  const char* pragmas[] = {"PRAGMA foreign_keys = 1", "PRAGMA journal_mode = wal", "PRAGMA synchronous = normal",
                           "PRAGMA journal_size_limit = 67108864", "PRAGMA cache_size = 2000"};
  for (auto p : pragmas) {
    char* err = nullptr;
    if (sqlite3_exec(db_, p, nullptr, nullptr, &err) != SQLITE_OK) { fprintf(stderr, "pragma %s: %s\n", p, err ? err : "?"); return false; }
  }
  if (query_only && sqlite3_exec(db_, "PRAGMA query_only = 1", nullptr, nullptr, nullptr) != SQLITE_OK) return false;
  if (trace) sqlite3_trace_v2(db_, SQLITE_TRACE_STMT, trace_cb, this);
  return true;
}

int Conn::exec(const char* sql) {
  int rc = sqlite3_exec(db_, sql, nullptr, nullptr, nullptr);
  if (trace_) {
    char line[1024];
    int n = snprintf(line, sizeof line - 1, "SQLTRACE end %s ThreadId(%d) rows=0 %s\n", role_, trace_thread_id(), sql);
    (void)!write(2, line, n);
  }
  return rc;
}

int Conn::run(Q q, std::initializer_list<Arg> args, RowSet& out) {
  out.clear();
  sqlite3_stmt*& st = stmts_[q];
  if (!st) {
    int rc = sqlite3_prepare_v3(db_, kSql[q], -1, SQLITE_PREPARE_PERSISTENT, &st, nullptr);
    if (rc != SQLITE_OK) { st = nullptr; return rc; }
  }
  int idx = 1;
  for (const Arg& a : args) {
    switch (a.kind) {
      case Arg::INT: sqlite3_bind_int64(st, idx, a.i); break;
      case Arg::TEXT: sqlite3_bind_text(st, idx, a.s.data(), (int)a.s.size(), SQLITE_STATIC); break;
      case Arg::NUL: sqlite3_bind_null(st, idx); break;
    }
    idx++;
  }
  int rc;
  int nc = sqlite3_column_count(st);
  out.ncols = nc;
  while ((rc = sqlite3_step(st)) == SQLITE_ROW) {
    out.nrows++;
    for (int c = 0; c < nc; c++) {
      Cell cell;
      switch (sqlite3_column_type(st, c)) {
        case SQLITE_INTEGER: cell.type = C_INT; cell.i = sqlite3_column_int64(st, c); break;
        case SQLITE_NULL: cell.type = C_NULL; break;
        case SQLITE_BLOB: {
          cell.type = C_BLOB;
          const void* b = sqlite3_column_blob(st, c);
          int n = sqlite3_column_bytes(st, c);
          cell.off = (uint32_t)out.arena.size(); cell.len = n;
          out.arena.append((const char*)b, n);
          break;
        }
        default: {  // text and float (as text)
          cell.type = C_TEXT;
          const char* t = (const char*)sqlite3_column_text(st, c);
          int n = sqlite3_column_bytes(st, c);
          cell.off = (uint32_t)out.arena.size(); cell.len = n;
          out.arena.append(t, n);
        }
      }
      out.cells.push_back(cell);
    }
  }
  sqlite3_reset(st);
  sqlite3_clear_bindings(st);
  if (trace_) {
    char line[8192];
    int n = snprintf(line, sizeof line - 1, "SQLTRACE end %s ThreadId(%d) rows=%zu %s\n", role_, trace_thread_id(), out.nrows, kSql[q]);
    if (n > (int)sizeof line - 1) n = sizeof line - 1;
    (void)!write(2, line, n);
  }
  return rc == SQLITE_DONE ? SQLITE_OK : rc;
}

}  // namespace gate
