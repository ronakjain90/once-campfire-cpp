// Tests of the message, boost and room reference models (the write side of messages).
#include "models/message.hpp"

#include <doctest.h>

#include <fstream>
#include <sstream>

#include "db/tests/test_util.hpp"
#include "models/boost.hpp"
#include "models/room_ref.hpp"

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
    REQUIRE(
        conn->exec_sql("INSERT INTO users (id, name, role, status, created_at, updated_at) VALUES "
                       "(1, 'David', 1, 0, '2026-03-01 00:00:00', '2026-03-01 00:00:00'), "
                       "(2, 'Jason', 0, 0, '2026-03-01 00:00:00', '2026-03-01 00:00:00'), "
                       "(3, 'Bender', 2, 0, '2026-03-01 00:00:00', '2026-03-01 00:00:00');"
                       "INSERT INTO rooms (id, name, type, creator_id, created_at, updated_at) VALUES "
                       "(1, 'Designers', 'Rooms::Open', 1, '2026-03-01 00:00:00', '2026-03-01 00:00:00');"
                       "INSERT INTO memberships (room_id, user_id, involvement, connections, created_at, updated_at) "
                       "VALUES (1, 1, 'everything', 0, '2026-03-01 00:00:00', '2026-03-01 00:00:00'), "
                       "(1, 2, 'mentions', 0, '2026-03-01 00:00:00', '2026-03-01 00:00:00'), "
                       "(1, 3, 'invisible', 0, '2026-03-01 00:00:00', '2026-03-01 00:00:00');")
            .has_value());
  }
  auto db = db::Database::open(path, std::move(options));
  REQUIRE(db.has_value());
  return std::move(*db);
}

struct RecordingJobs final : JobSink {
  std::vector<std::pair<std::int64_t, std::int64_t>> pushed;
  void push_message(std::int64_t room, std::int64_t message) override { pushed.emplace_back(room, message); }
  void deliver_webhook(std::int64_t, std::int64_t) override {}
};

std::string text_of(db::Connection& conn, Arena& arena, const char* sql) {
  sqlite3_stmt* st = nullptr;
  REQUIRE(sqlite3_prepare_v2(conn.handle(), sql, -1, &st, nullptr) == SQLITE_OK);
  std::string out;
  if (sqlite3_step(st) == SQLITE_ROW) {
    const auto* t = sqlite3_column_text(st, 0);
    out = t != nullptr ? reinterpret_cast<const char*>(t) : "<null>";
  } else {
    out = "<none>";
  }
  sqlite3_finalize(st);
  static_cast<void>(arena);
  return out;
}

}  // namespace

TEST_CASE("messages: create, update, touch, destroy") {
  TempDir dir;
  auto clock = TestClock::frozen_at(*from_civil(2026, 3, 2, 16, 0, 0));
  db::DatabaseOptions options;
  options.clock = clock;
  auto database = open_app_db(dir, options);
  QueueScheduler scheduler;
  auto reader = std::move(*database->open_reader());
  Arena arena;
  RecordingJobs jobs;

  NewMessage attributes;
  attributes.room_id = 1;
  attributes.creator_id = 1;
  attributes.client_message_id = "cm-1";
  attributes.body = "<p>Hello</p>";
  attributes.plain_text = "Hello";
  auto created = run_task(scheduler, database->write(scheduler, [&](db::Tx& tx) -> Result<Message> {
    return messages::create(tx, attributes, &jobs);
  }));
  REQUIRE(created.has_value());
  CHECK(created->client_message_id == "cm-1");
  CHECK(created->created_at == "2026-03-02 16:00:00");
  CHECK(jobs.pushed == std::vector<std::pair<std::int64_t, std::int64_t>>{{1, created->id}});

  CHECK(text_of(reader, arena, "SELECT body FROM action_text_rich_texts") == "<p>Hello</p>");
  CHECK(text_of(reader, arena, "SELECT updated_at FROM rooms") == "2026-03-02 16:00:00");
  CHECK(text_of(reader, arena, "SELECT body FROM message_search_index WHERE rowid = 1") == "Hello");
  // The unread update: a member that is not the creator and not invisible.
  CHECK(text_of(reader, arena, "SELECT unread_at FROM memberships WHERE user_id = 1") == "<null>");
  CHECK(text_of(reader, arena, "SELECT unread_at FROM memberships WHERE user_id = 2") == "2026-03-02 16:00:00");
  CHECK(text_of(reader, arena, "SELECT unread_at FROM memberships WHERE user_id = 3") == "<null>");

  auto found = messages::find_in_room(reader, arena, 1, created->id);
  REQUIRE(found->has_value());
  CHECK_FALSE(messages::find_in_room(reader, arena, 2, created->id)->has_value());
  CHECK(messages::find_reachable(reader, arena, 2, created->id)->has_value());

  clock->travel(5);
  Message message = **found;
  auto same = run_task(scheduler, database->write(scheduler, [&](db::Tx& tx) -> Status {
    return messages::update_body(tx, message, "<p>Hello</p>", "Hello");
  }));
  REQUIRE(same.has_value());
  CHECK(text_of(reader, arena, "SELECT updated_at FROM messages") == "2026-03-02 16:00:00");
  auto updated = run_task(scheduler, database->write(scheduler, [&](db::Tx& tx) -> Status {
    return messages::update_body(tx, message, "<p>Bye</p>", "Bye");
  }));
  REQUIRE(updated.has_value());
  CHECK(text_of(reader, arena, "SELECT updated_at FROM messages") == "2026-03-02 16:00:05");
  CHECK(text_of(reader, arena, "SELECT updated_at FROM rooms") == "2026-03-02 16:00:05");
  CHECK(text_of(reader, arena, "SELECT body FROM action_text_rich_texts") == "<p>Bye</p>");
  CHECK(text_of(reader, arena, "SELECT body FROM message_search_index WHERE rowid = 1") == "Bye");

  clock->travel(1);
  auto boost = run_task(scheduler, database->write(scheduler, [&](db::Tx& tx) -> Result<Boost> {
    return boosts::create(tx, message.id, 2, "👍", "Bye");
  }));
  REQUIRE(boost.has_value());
  CHECK(text_of(reader, arena, "SELECT updated_at FROM messages") == "2026-03-02 16:00:06");
  auto listed = boosts::for_message_ordered(reader, arena, message.id);
  REQUIRE(listed->size() == 1);
  CHECK((*listed)[0].content == "👍");
  CHECK_FALSE(boosts::find_by_message_and_booster(reader, arena, message.id, boost->id, 1)->has_value());
  REQUIRE(boosts::find_by_message_and_booster(reader, arena, message.id, boost->id, 2)->has_value());
  auto unboosted = run_task(
      scheduler, database->write(scheduler, [&](db::Tx& tx) -> Status { return boosts::destroy(tx, *boost, "Bye"); }));
  REQUIRE(unboosted.has_value());
  CHECK(boosts::for_message_ordered(reader, arena, message.id)->empty());

  auto destroyed =
      run_task(scheduler, database->write(scheduler, [&](db::Tx& tx) -> Result<messages::ReplacedAttachment> {
        return messages::destroy(tx, message);
      }));
  REQUIRE(destroyed.has_value());
  CHECK_FALSE(destroyed->purged_blob_id.has_value());
  CHECK(text_of(reader, arena, "SELECT COUNT(*) FROM messages") == "0");
  CHECK(text_of(reader, arena, "SELECT COUNT(*) FROM action_text_rich_texts") == "0");
  CHECK(text_of(reader, arena, "SELECT COUNT(*) FROM message_search_index") == "0");
}

TEST_CASE("messages: pages") {
  TempDir dir;
  auto database = open_app_db(dir);
  QueueScheduler scheduler;
  auto reader = std::move(*database->open_reader());
  Arena arena;
  for (int i = 0; i < 45; ++i) {
    NewMessage attributes;
    attributes.room_id = 1;
    attributes.creator_id = 2;
    attributes.plain_text = "m";
    REQUIRE(run_task(scheduler, database->write(scheduler, [&](db::Tx& tx) -> Result<Message> {
              return messages::create(tx, attributes, nullptr);
            })).has_value());
  }
  auto last = messages::last_page(reader, arena, 1);
  REQUIRE(last.has_value());
  CHECK(last->size() == 40);
  CHECK(*messages::count_in_room(reader, arena, 1) == 45);
  CHECK(last->front().id < last->back().id);
  CHECK(last->front().client_message_id.size() == 36);
}

TEST_CASE("room refs: lookups") {
  TempDir dir;
  auto database = open_app_db(dir);
  auto reader = std::move(*database->open_reader());
  Arena arena;
  auto room = room_refs::find_for_user(reader, arena, 2, 1);
  REQUIRE(room->has_value());
  CHECK((*room)->param_key() == "rooms_open");
  CHECK_FALSE((*room)->direct());
  CHECK_FALSE(room_refs::find_for_user(reader, arena, 99, 1)->has_value());
  CHECK(room_refs::member_user_ids(reader, arena, 1)->size() == 3);
  auto bots = room_refs::active_bots(reader, arena, 1);
  REQUIRE(bots->size() == 1);
  CHECK((*bots)[0].name == "Bender");
  CHECK(room_refs::members_among(reader, arena, 1, {1, 3, 9})->size() == 2);
  CHECK(room_refs::autocompletable_users(reader, arena, std::nullopt, std::nullopt)->size() == 3);
  CHECK(room_refs::autocompletable_users(reader, arena, 1, "ja")->size() == 1);
}

}  // namespace campfire::models
