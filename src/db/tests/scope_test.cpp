// Tests of the dependency scope. Design: docs/architecture.md section 6.1.
#include "db/tests/test_util.hpp"

namespace campfire::db {
using namespace testing;

namespace {

struct Fixture {
  TempDir dir;
  Connection writer = std::move(*Connection::open(dir.file("a.sqlite3"), Role::Writer));
  Connection reader = std::move(*Connection::open(dir.file("a.sqlite3"), Role::Reader));
  Arena arena;
  Fixture() {
    REQUIRE(writer.exec_sql("CREATE TABLE items (id INTEGER PRIMARY KEY, name TEXT NOT NULL, n INTEGER)"));
    for (int i = 0; i < 5; ++i) {
      REQUIRE(writer.exec(InsertItem, "item", std::optional<std::int64_t>(i)));
    }
  }
  // The key of a page that reads items above `after` and item 1.
  Hash128 page(std::int64_t after, std::string_view host = "example.com") {
    DependencyScope scope;
    reader.set_scope(&scope);
    scope.facet("host", host);
    REQUIRE(reader.all(ItemsAbove, arena, after));
    REQUIRE(reader.first(ItemById, arena, 1));
    reader.set_scope(nullptr);
    return scope.key();
  }
};

}  // namespace

TEST_CASE("the dependency hash is stable") {
  Fixture f;
  const Hash128 a = f.page(0);
  CHECK(f.page(0) == a);
  CHECK(f.page(0) == a);
}

TEST_CASE("the dependency hash changes on a change of a returned row") {
  Fixture f;
  const Hash128 before = f.page(0);
  REQUIRE(f.writer.exec(RenameItem, "renamed", 3));
  const Hash128 after = f.page(0);
  CHECK(!(after == before));
  CHECK(f.page(0) == after);
  // A new row, and a deleted row, change it too.
  REQUIRE(f.writer.exec(InsertItem, "new", std::optional<std::int64_t>()));
  const Hash128 grown = f.page(0);
  CHECK(!(grown == after));
  REQUIRE(f.writer.exec_sql("DELETE FROM items WHERE id = 6"));
  CHECK(f.page(0) == after);
}

TEST_CASE("the dependency hash ignores rows that the page did not read") {
  Fixture f;
  const Hash128 before = f.page(3);  // reads items 4 and 5, and item 1
  REQUIRE(f.writer.exec(RenameItem, "renamed", 2));
  CHECK(f.page(3) == before);
  REQUIRE(f.writer.exec(RenameItem, "renamed", 5));
  CHECK(!(f.page(3) == before));
}

TEST_CASE("the dependency hash includes parameters, facets and the statement") {
  Fixture f;
  CHECK(!(f.page(0) == f.page(1)));
  CHECK(!(f.page(0, "a.example") == f.page(0, "b.example")));
  DependencyScope s1;
  DependencyScope s2;
  f.reader.set_scope(&s1);
  REQUIRE(f.reader.first(ItemById, f.arena, 1));
  f.reader.set_scope(&s2);
  REQUIRE(f.reader.all(ItemsAbove, f.arena, 0));
  f.reader.set_scope(nullptr);
  CHECK(!(s1.key() == s2.key()));
  CHECK(s1.statement_count() == 1);
  CHECK(s1.row_count() == 1);
  CHECK(s2.row_count() == 5);
}

TEST_CASE("the uncacheable flag and reset") {
  DependencyScope scope;
  CHECK(scope.cacheable());
  scope.mark_uncacheable();
  CHECK(!scope.cacheable());
  scope.facet("a", "b");
  const Hash128 k = scope.key();
  scope.reset();
  CHECK(scope.cacheable());
  scope.facet("a", "b");
  CHECK(scope.key() == k);
}

TEST_CASE("a long text value goes into the hash") {
  Fixture f;
  const std::string big(10'000, 'x');
  REQUIRE(f.writer.exec(InsertItem, big, std::optional<std::int64_t>()));
  const Hash128 a = f.page(0);
  REQUIRE(f.writer.exec(RenameItem, big + "y", 6));
  CHECK(!(f.page(0) == a));
}

}  // namespace campfire::db
