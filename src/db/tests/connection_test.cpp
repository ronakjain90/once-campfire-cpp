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

TEST_CASE("a read transaction keeps one snapshot until it ends") {
  const TempDir dir;
  auto writer = Connection::open(dir.file("a.sqlite3"), Role::Writer);
  REQUIRE(writer);
  REQUIRE(writer->exec_sql("CREATE TABLE items (id INTEGER PRIMARY KEY, name TEXT NOT NULL, n INTEGER)"));
  REQUIRE(writer->exec(InsertItem, "a", std::optional<std::int64_t>()));
  auto reader = Connection::open(dir.file("a.sqlite3"), Role::Reader);
  REQUIRE(reader);
  Arena arena;

  reader->set_read_transactions(true);
  CHECK(sqlite3_get_autocommit(reader->handle()) != 0);  // the first statement starts it
  CHECK(*reader->first(CountItems, arena) == std::optional<std::int64_t>(1));
  CHECK(sqlite3_get_autocommit(reader->handle()) == 0);
  REQUIRE(writer->exec(InsertItem, "b", std::optional<std::int64_t>()));
  CHECK(*reader->first(CountItems, arena) == std::optional<std::int64_t>(1));  // the same snapshot

  reader->end_read_transaction();
  CHECK(sqlite3_get_autocommit(reader->handle()) != 0);
  CHECK(*reader->first(CountItems, arena) == std::optional<std::int64_t>(2));  // a new snapshot
  REQUIRE(writer->exec(InsertItem, "c", std::optional<std::int64_t>()));
  CHECK(*reader->first(CountItems, arena) == std::optional<std::int64_t>(2));

  reader->set_read_transactions(false);
  CHECK(sqlite3_get_autocommit(reader->handle()) != 0);
  CHECK(*reader->first(CountItems, arena) == std::optional<std::int64_t>(3));  // autocommit
  CHECK(sqlite3_get_autocommit(reader->handle()) != 0);
}

TEST_CASE("a moved connection keeps its read transaction") {
  const TempDir dir;
  auto writer = Connection::open(dir.file("a.sqlite3"), Role::Writer);
  REQUIRE(writer);
  REQUIRE(writer->exec_sql("CREATE TABLE items (id INTEGER PRIMARY KEY, name TEXT NOT NULL, n INTEGER)"));
  auto reader = Connection::open(dir.file("a.sqlite3"), Role::Reader);
  REQUIRE(reader);
  Arena arena;
  reader->set_read_transactions(true);
  CHECK(*reader->first(CountItems, arena) == std::optional<std::int64_t>(0));
  Connection moved = std::move(*reader);
  CHECK(moved.read_transactions());
  moved.set_read_transactions(false);
  CHECK(sqlite3_get_autocommit(moved.handle()) != 0);
}

namespace {
const Query<std::int64_t(std::int64_t)> kNameAsNumber{"SELECT name FROM items WHERE id = ?"};
const Query<std::string_view()> kClock{"SELECT datetime('now')"};

struct Snapshot {
  std::vector<std::string> names;
  std::vector<std::optional<std::int64_t>> numbers;
  bool found_first = false;
  std::string first_name;
  bool found_missing = false;
  std::size_t first_two = 0;
  Hash128 key;
};

// One request's reads in one read transaction, with a scope.
Snapshot read_all(Connection& reader) {
  Arena arena;
  DependencyScope scope;
  reader.set_scope(&scope);
  reader.set_read_transactions(true);
  Snapshot out;
  auto rows = reader.all(ItemsAbove, arena, 0);
  REQUIRE(rows.has_value());
  for (const ItemRow& row : *rows) {
    out.names.emplace_back(row.name);
    out.numbers.push_back(row.n);
  }
  auto one = reader.first(ItemById, arena, 1);
  REQUIRE(one.has_value());
  out.found_first = one->has_value();
  if (*one) out.first_name = std::string((*one)->name);
  auto missing = reader.first(ItemById, arena, 999);
  REQUIRE(missing.has_value());
  out.found_missing = missing->has_value();
  auto two = reader.first_n(ItemsAbove, arena, 2, 0);
  REQUIRE(two.has_value());
  out.first_two = two->size();
  out.key = scope.key();
  reader.set_read_transactions(false);
  reader.set_scope(nullptr);
  return out;
}

void open_items(const TempDir& dir, std::optional<Connection>& writer, std::optional<Connection>& reader) {
  auto w = Connection::open(dir.file("a.sqlite3"), Role::Writer);
  REQUIRE(w);
  REQUIRE(w->exec_sql("CREATE TABLE items (id INTEGER PRIMARY KEY, name TEXT NOT NULL, n INTEGER)"));
  REQUIRE(w->exec(InsertItem, "a", std::optional<std::int64_t>(1)));
  REQUIRE(w->exec(InsertItem, "b", std::optional<std::int64_t>()));
  REQUIRE(w->exec(InsertItem, "", std::optional<std::int64_t>(3)));
  writer.emplace(std::move(*w));
  auto r = Connection::open(dir.file("a.sqlite3"), Role::Reader);
  REQUIRE(r);
  reader.emplace(std::move(*r));
}
}  // namespace

TEST_CASE("memo: the same snapshot gives the same rows and the same scope key from the memo") {
  const TempDir dir;
  std::optional<Connection> writer;
  std::optional<Connection> reader;
  open_items(dir, writer, reader);

  const Snapshot live = read_all(*reader);
  CHECK(reader->memo_stats().hits == 0);
  CHECK(reader->memo_stats().entries == 4);
  const Snapshot again = read_all(*reader);
  CHECK(reader->memo_stats().hits == 4);  // all, first with a row, first with none, first_n

  CHECK(again.names == live.names);
  CHECK(again.names == std::vector<std::string>{"a", "b", ""});
  CHECK(again.numbers == live.numbers);
  CHECK_FALSE(again.numbers[1].has_value());  // NULL stays NULL
  CHECK(again.found_first);
  CHECK(again.first_name == "a");
  CHECK_FALSE(again.found_missing);
  CHECK(again.first_two == 2);
  CHECK(again.key == live.key);  // a page key does not depend on where the rows came from
}

TEST_CASE("memo: a commit by another connection gives the new rows") {
  const TempDir dir;
  std::optional<Connection> writer;
  std::optional<Connection> reader;
  open_items(dir, writer, reader);
  const Snapshot before = read_all(*reader);
  REQUIRE(writer->exec(RenameItem, "renamed", 1));
  const Snapshot after = read_all(*reader);
  CHECK(after.names[0] == "renamed");
  CHECK(after.first_name == "renamed");
  CHECK(after.key != before.key);
  CHECK(reader->memo_stats().hits == 0);
  // No commit since then: the memo has the new rows.
  const Snapshot again = read_all(*reader);
  CHECK(again.names[0] == "renamed");
  CHECK(again.key == after.key);
  CHECK(reader->memo_stats().hits == 4);
}

TEST_CASE("memo: a read that converts a type, or SQL with the clock, is not kept") {
  const TempDir dir;
  std::optional<Connection> writer;
  std::optional<Connection> reader;
  open_items(dir, writer, reader);
  Arena arena;
  for (int round = 0; round < 2; ++round) {
    reader->set_read_transactions(true);
    auto number = reader->first(kNameAsNumber, arena, 1);  // TEXT read as an integer: SQLite converts it
    REQUIRE(number.has_value());
    CHECK(**number == 0);
    auto clock = reader->first(kClock, arena);
    REQUIRE(clock.has_value());
    CHECK((*clock)->size() == 19);
    reader->set_read_transactions(false);
  }
  CHECK(reader->memo_stats().hits == 0);
  CHECK(reader->memo_stats().entries == 0);
}

TEST_CASE("memo: off in autocommit") {
  const TempDir dir;
  std::optional<Connection> writer;
  std::optional<Connection> reader;
  open_items(dir, writer, reader);
  Arena arena;
  for (int round = 0; round < 2; ++round) {
    auto rows = reader->all(ItemsAbove, arena, 0);
    REQUIRE(rows.has_value());
    CHECK(rows->size() == 3);
  }
  CHECK(reader->memo_stats().entries == 0);
  CHECK(reader->memo_stats().hits == 0);
}

}  // namespace campfire::db
