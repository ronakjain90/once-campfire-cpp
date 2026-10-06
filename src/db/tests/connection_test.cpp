// Tests of Connection: pragmas, statement cache, binding, row reading.
#include "db/tests/test_util.hpp"

namespace campfire::db {
using namespace testing;

namespace {
std::int64_t pragma_int(Connection& c, const char* sql) {
  sqlite3_stmt* st = nullptr;
  REQUIRE(sqlite3_prepare_v2(c.handle(), sql, -1, &st, nullptr) == SQLITE_OK);
  REQUIRE(sqlite3_step(st) == SQLITE_ROW);
  const std::int64_t v = sqlite3_column_int64(st, 0);
  sqlite3_finalize(st);
  return v;
}
}  // namespace

TEST_CASE("connection pragmas follow the Rust port") {
  const TempDir dir;
  auto writer = Connection::open(dir.file("a.sqlite3"), Role::Writer);
  REQUIRE(writer);
  CHECK(pragma_int(*writer, "PRAGMA foreign_keys") == 1);
  CHECK(pragma_int(*writer, "PRAGMA synchronous") == 1);  // NORMAL
  CHECK(pragma_int(*writer, "PRAGMA journal_size_limit") == 67108864);
  CHECK(pragma_int(*writer, "PRAGMA cache_size") == 2000);
  CHECK(pragma_int(*writer, "PRAGMA wal_autocheckpoint") == 0);
  CHECK(pragma_int(*writer, "PRAGMA busy_timeout") == 5000);
  CHECK(pragma_int(*writer, "PRAGMA query_only") == 0);
  auto reader = Connection::open(dir.file("a.sqlite3"), Role::Reader);
  REQUIRE(reader);
  CHECK(pragma_int(*reader, "PRAGMA query_only") == 1);
  CHECK(pragma_int(*reader, "PRAGMA wal_autocheckpoint") == 1000);
  sqlite3_stmt* st = nullptr;
  REQUIRE(sqlite3_prepare_v2(reader->handle(), "PRAGMA journal_mode", -1, &st, nullptr) == SQLITE_OK);
  REQUIRE(sqlite3_step(st) == SQLITE_ROW);
  CHECK(std::string(reinterpret_cast<const char*>(sqlite3_column_text(st, 0))) == "wal");
  sqlite3_finalize(st);
}

TEST_CASE("FTS5 is compiled in") {
  const TempDir dir;
  auto conn = Connection::open(dir.file("a.sqlite3"), Role::Writer);
  REQUIRE(conn);
  CHECK(conn->exec_sql("CREATE VIRTUAL TABLE s USING fts5(body, tokenize='porter')"));
}

TEST_CASE("typed statements bind and read every type") {
  const TempDir dir;
  auto conn = Connection::open(dir.file("a.sqlite3"), Role::Writer);
  REQUIRE(conn);
  REQUIRE(conn->exec_sql("CREATE TABLE items (id INTEGER PRIMARY KEY, name TEXT NOT NULL, n INTEGER)"));
  Arena arena;
  CHECK(*conn->exec(InsertItem, "alpha", std::optional<std::int64_t>(7)) == 1);
  CHECK(*conn->exec(InsertItem, "beta", std::optional<std::int64_t>()) == 1);
  CHECK(*conn->exec(InsertItem, "", std::optional<std::int64_t>(3)) == 1);
  auto one = conn->first(ItemById, arena, 1);
  REQUIRE(one);
  REQUIRE(one->has_value());
  CHECK((*one)->name == "alpha");
  CHECK((*one)->n == 7);
  auto two = conn->first(ItemById, arena, 2);
  CHECK(!(*two)->n.has_value());
  CHECK((*conn->first(ItemById, arena, 3))->name.empty());  // empty text is not NULL
  CHECK(!conn->first(ItemById, arena, 99)->has_value());
  auto rows = conn->all(ItemsAbove, arena, 0);
  REQUIRE(rows);
  CHECK(rows->size() == 3);
  CHECK(*conn->first(CountItems, arena) == std::optional<std::int64_t>(3));
}

TEST_CASE("the statement cache prepares each statement once") {
  const TempDir dir;
  auto conn = Connection::open(dir.file("a.sqlite3"), Role::Writer);
  REQUIRE(conn);
  REQUIRE(conn->exec_sql("CREATE TABLE items (id INTEGER PRIMARY KEY, name TEXT NOT NULL, n INTEGER)"));
  Arena arena;
  CHECK(conn->prepare_count() == 0);
  for (int i = 0; i < 50; ++i) {
    REQUIRE(conn->exec(InsertItem, "x", std::optional<std::int64_t>(i)));
  }
  CHECK(conn->prepare_count() == 1);
  for (int i = 1; i <= 50; ++i) {
    REQUIRE(conn->first(ItemById, arena, i));
  }
  CHECK(conn->prepare_count() == 2);
  REQUIRE(conn->all(ItemsAbove, arena, 0));
  REQUIRE(conn->all(ItemsAbove, arena, 10));
  CHECK(conn->prepare_count() == 3);
  // A second connection has its own array.
  auto other = Connection::open(dir.file("a.sqlite3"), Role::Reader);
  REQUIRE(other->first(ItemById, arena, 1));
  CHECK(other->prepare_count() == 1);
}

TEST_CASE("errors come back as values") {
  const TempDir dir;
  auto conn = Connection::open(dir.file("a.sqlite3"), Role::Writer);
  REQUIRE(conn->exec_sql("CREATE TABLE items (id INTEGER PRIMARY KEY, name TEXT NOT NULL, n INTEGER)"));
  CHECK(*conn->exec(BadInsertItem, 1, "a") == 1);
  auto dup = conn->exec(BadInsertItem, 1, "a");  // the primary key exists
  REQUIRE(!dup);
  CHECK(dup.error().code == Errc::InvalidArgument);
  auto reader = Connection::open(dir.file("a.sqlite3"), Role::Reader);
  auto denied = reader->exec(InsertItem, "z", std::optional<std::int64_t>());
  REQUIRE(!denied);
  CHECK(denied.error().message.find("readonly") != std::string::npos);
  auto missing = conn->exec(Query<void()>{"INSERT INTO nope VALUES (1)"});
  CHECK(!missing);
}

}  // namespace campfire::db
