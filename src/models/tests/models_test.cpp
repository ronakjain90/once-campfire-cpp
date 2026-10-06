// Tests of the session, user, account and ban models.
#include <doctest.h>

#include <fstream>
#include <sstream>

#include "db/tests/test_util.hpp"
#include "models/account.hpp"
#include "models/ban.hpp"
#include "models/session.hpp"
#include "models/user.hpp"
#include "req/bcrypt.hpp"

namespace campfire::models {
using namespace db::testing;

namespace {

std::unique_ptr<db::Database> open_app_db(const TempDir& dir, db::DatabaseOptions options = {}) {
  const std::string path = dir.file("app.sqlite3");
  {
    std::ifstream in(CAMPFIRE_SPEC_DIR "/schema.sql");
    REQUIRE(in.good());
    std::stringstream sql;
    sql << in.rdbuf();
    auto conn = db::Connection::open(path, db::Role::Writer);
    REQUIRE(conn.has_value());
    REQUIRE(conn->exec_sql(sql.str()).has_value());
  }
  auto db = db::Database::open(path, std::move(options));
  REQUIRE(db.has_value());
  return std::move(*db);
}

const db::Query<void(std::string_view, std::string_view, std::string_view, std::int64_t)> kInsertUser{
    "INSERT INTO users (name, email_address, password_digest, role, status, created_at, updated_at) VALUES "
    "(?, ?, ?, ?, 0, '2026-03-02 16:00:00', '2026-03-02 16:00:00')"};

}  // namespace

TEST_CASE("sessions: start, find, resume, destroy") {
  TempDir dir;
  auto clock = TestClock::frozen_at(*from_civil(2026, 3, 2, 16, 0, 0));
  db::DatabaseOptions options;
  options.clock = clock;
  auto database = open_app_db(dir, options);
  QueueScheduler scheduler;
  auto reader = std::move(*database->open_reader());
  Arena arena;

  REQUIRE(run_task(scheduler, database->write(scheduler, [&](db::Tx& tx) -> Status {
                     auto r = tx.conn().exec(kInsertUser, "Kevin", "kevin@example.com", "digest", 0);
                     if (!r) return std::unexpected(r.error());
                     return {};
                   })).has_value());
  auto started = run_task(scheduler, database->write(scheduler, [&](db::Tx& tx) -> Result<Session> {
                            return sessions::start(tx, 1, "agent", "10.0.0.1");
                          }));
  REQUIRE(started.has_value());
  CHECK(started->token.size() == 24);
  CHECK(started->last_active_at == "2026-03-02 16:00:00");

  auto found = sessions::find_by_token(reader, arena, started->token);
  REQUIRE(found.has_value());
  REQUIRE(found->has_value());
  CHECK((*found)->user_id == 1);
  CHECK((*found)->ip_address == "10.0.0.1");
  CHECK_FALSE((*found)->needs_resume(clock->now()));

  clock->travel(3601);
  Session session = **found;
  CHECK(session.needs_resume(clock->now()));
  std::vector<db::Change> seen;
  database->subscribe([&](std::span<const db::Change> c) { seen.assign(c.begin(), c.end()); });
  auto resumed = run_task(scheduler, database->write(scheduler, [&](db::Tx& tx) -> Status {
                            return sessions::resume(tx, session, "agent2", std::nullopt);
                          }));
  REQUIRE(resumed.has_value());
  CHECK(session.last_active_at == "2026-03-02 17:00:01");
  REQUIRE(seen.size() == 1);
  CHECK(seen[0].table == db::schema::Table::Sessions);
  CHECK(seen[0].id == session.id);
  auto again = sessions::find_by_token(reader, arena, session.token);
  CHECK((*again)->user_agent == "agent2");
  CHECK_FALSE((*again)->ip_address.has_value());

  auto destroyed = run_task(scheduler, database->write(scheduler, [&](db::Tx& tx) -> Status {
                              return sessions::destroy(tx, session);
                            }));
  REQUIRE(destroyed.has_value());
  CHECK_FALSE(sessions::find_by_token(reader, arena, session.token)->has_value());
}

TEST_CASE("sessions: tokens are 24 base58 characters") {
  const std::string a = sessions::generate_token();
  const std::string b = sessions::generate_token();
  CHECK(a.size() == 24);
  CHECK(a != b);
  CHECK(a.find_first_of("0OIl") == std::string::npos);
}

TEST_CASE("users: authenticate_by, bots, owner, none") {
  TempDir dir;
  auto database = open_app_db(dir);
  QueueScheduler scheduler;
  auto reader = std::move(*database->open_reader());
  Arena arena;
  CHECK(*users::none(reader, arena));
  const std::string digest = req::bcrypt::hash_password("secret", req::bcrypt::kMinCost);
  auto wrote = run_task(scheduler, database->write(scheduler, [&](db::Tx& tx) -> Status {
                          auto r = tx.conn().exec(kInsertUser, "David", "david@example.com", digest, 1);
                          if (!r) return std::unexpected(r.error());
                          return {};
                        }));
  REQUIRE(wrote.has_value());
  CHECK_FALSE(*users::none(reader, arena));
  auto candidate = users::find_active_by_email_address(reader, arena, "david@example.com");
  REQUIRE(candidate->has_value());
  CHECK(users::authenticated(*candidate, "secret").has_value());
  CHECK_FALSE(users::authenticated(*candidate, "wrong").has_value());
  CHECK_FALSE(users::authenticated(*candidate, "").has_value());
  CHECK_FALSE(users::authenticated(std::nullopt, "secret").has_value());
  auto owner = users::first_administrator(reader, arena);
  REQUIRE(owner->has_value());
  CHECK((*owner)->name == "David");
  CHECK_FALSE(bans::banned(reader, arena, "1.2.3.4").value());
}

}  // namespace campfire::models
