// End to end tests of the area A3: message create, edit, update and delete, boosts, the bot API, user autocomplete.
// Rails: messages_controller, messages/boosts_controller, messages/by_bots_controller, autocompletable/users_controller.
#include <doctest.h>

#include "app/message_presenter.hpp"
#include "app/tests/fixture.hpp"
#include "models/sound.hpp"

namespace campfire::app::testing {

namespace {

std::string cookie_pair(const Reply& reply, const std::string& name) {
  for (const auto& [k, v] : reply.headers) {
    if (k == "set-cookie" && v.starts_with(name + "=")) return v.substr(0, v.find(';'));
  }
  return {};
}

std::string sign_in(Client& c, const std::string& email) {
  const Reply r =
      c.request("POST", "/session", kSameOrigin + kForm, "email_address=" + email + "&password=" + kPassword);
  REQUIRE(r.status == 302);
  return "Cookie: " + cookie_pair(r, "session_token") + "\r\n";
}

// David (administrator, id 1) is in the account. Jason (member, id 2) and Bender (bot, id 3) join room 1.
void seed_room(Fixture& f) {
  const std::string digest = req::bcrypt::hash_password(kPassword, req::bcrypt::kMinCost);
  f.write([&](db::Tx& tx) -> Status {
    if (auto r = tx.conn().exec(kInsertUser, "Jason", "jason@example.com", digest, 0); !r) return std::unexpected(r.error());
    return tx.conn().exec_sql(
        "INSERT INTO users (name, role, status, bot_token, created_at, updated_at) VALUES "
        "('Bender', 2, 0, 'BenderToken', '2026-03-02 16:00:00', '2026-03-02 16:00:00');"
        "INSERT INTO rooms (name, type, creator_id, created_at, updated_at) VALUES "
        "('Designers', 'Rooms::Open', 1, '2026-03-02 16:00:00', '2026-03-02 16:00:00');"
        "INSERT INTO memberships (room_id, user_id, involvement, connections, created_at, updated_at) VALUES "
        "(1, 1, 'everything', 0, '2026-03-02 16:00:00', '2026-03-02 16:00:00'), "
        "(1, 2, 'everything', 0, '2026-03-02 16:00:00', '2026-03-02 16:00:00'), "
        "(1, 3, 'everything', 0, '2026-03-02 16:00:00', '2026-03-02 16:00:00');");
  });
}

std::string scalar(Fixture& f, const char* sql) {
  auto conn = f.state->db->open_reader();
  REQUIRE(conn.has_value());
  sqlite3_stmt* st = nullptr;
  REQUIRE(sqlite3_prepare_v2(conn->handle(), sql, -1, &st, nullptr) == SQLITE_OK);
  std::string out = "<none>";
  if (sqlite3_step(st) == SQLITE_ROW) {
    const auto* text = sqlite3_column_text(st, 0);
    out = text != nullptr ? reinterpret_cast<const char*>(text) : "<null>";
  }
  sqlite3_finalize(st);
  return out;
}

const std::string kTurbo = "Accept: text/vnd.turbo-stream.html, text/html, application/xhtml+xml\r\n";

}  // namespace

TEST_CASE("messages: create, show, edit, update and destroy") {
  Fixture f;
  seed_room(f);
  Client c(f.port());
  const std::string david = sign_in(c, "david@example.com");
  const std::string jason = sign_in(c, "jason@example.com");

  Reply r = c.request("POST", "/rooms/1/messages", david + kSameOrigin + kForm + kTurbo,
                      "message%5Bbody%5D=%3Cp%3EHello%3C%2Fp%3E&message%5Bclient_message_id%5D=cm-1");
  REQUIRE(r.status == 200);
  CHECK(r.header("content-type") == "text/vnd.turbo-stream.html; charset=utf-8");
  CHECK(r.body.starts_with("<turbo-stream action=\"append\" target=\"messages_rooms_open_1\"><template>"));
  CHECK(r.body.ends_with("</template></turbo-stream>\n"));
  CHECK(scalar(f, "SELECT client_message_id FROM messages") == "cm-1");
  CHECK(scalar(f, "SELECT body FROM action_text_rich_texts WHERE record_type = 'Message'") == "<p>Hello</p>");
  CHECK(scalar(f, "SELECT body FROM message_search_index WHERE rowid = 1") == "Hello");
  CHECK(scalar(f, "SELECT unread_at FROM memberships WHERE user_id = 2") == "2026-03-02 16:00:00");
  CHECK(scalar(f, "SELECT COUNT(*) FROM memberships WHERE user_id = 1 AND unread_at IS NOT NULL") == "0");

  // The HTML format has no template for `create`.
  r = c.request("POST", "/rooms/1/messages", david + kSameOrigin + kForm, "message%5Bbody%5D=x");
  CHECK(r.status == 406);
  // A room that is gone renders `room_not_found`.
  r = c.request("POST", "/rooms/99/messages", david + kSameOrigin + kForm + kTurbo, "message%5Bbody%5D=x");
  CHECK(r.status == 200);
  CHECK(r.body.find("This room was deleted.") != std::string::npos);
  // `params.require(:message)`
  r = c.request("POST", "/rooms/1/messages", david + kSameOrigin + kForm + kTurbo, "x=1");
  CHECK(r.status == 400);

  r = c.request("GET", "/rooms/1/messages/1", david);
  CHECK(r.status == 200);
  r = c.request("GET", "/rooms/1/messages/1/edit", david);
  CHECK(r.status == 200);
  CHECK(r.body.find("<lexxy-editor rows=\"1\" class=\"input lexxy-content\"") != std::string::npos);
  CHECK(r.body.find("value=\"&lt;p&gt;Hello&lt;/p&gt;\"") != std::string::npos);
  CHECK(r.body.find("<turbo-frame id=\"edit_message_cm-1\">") != std::string::npos);
  r = c.request("GET", "/rooms/1/messages/1/edit", jason);
  CHECK(r.status == 403);
  r = c.request("GET", "/rooms/1/messages/new", david);
  CHECK(r.status == 404);

  r = c.request("PUT", "/rooms/1/messages/1", jason + kSameOrigin + kForm + kTurbo, "message%5Bbody%5D=x");
  CHECK(r.status == 403);
  r = c.request("PUT", "/rooms/1/messages/1", david + kSameOrigin + kForm + kTurbo, "message%5Bbody%5D=%3Cp%3EEdited%3C%2Fp%3E");
  CHECK(r.status == 302);
  CHECK(r.header("location") == "http://test.example/rooms/1/messages/1");
  CHECK(scalar(f, "SELECT body FROM action_text_rich_texts WHERE record_type = 'Message'") == "<p>Edited</p>");
  CHECK(scalar(f, "SELECT body FROM message_search_index WHERE rowid = 1") == "Edited");

  r = c.request("DELETE", "/rooms/1/messages/1", jason + kSameOrigin + kTurbo);
  CHECK(r.status == 403);
  r = c.request("DELETE", "/rooms/1/messages/1", david + kSameOrigin + kTurbo);
  CHECK(r.status == 200);
  CHECK(r.body == "<turbo-stream action=\"remove\" target=\"message_cm-1\"></turbo-stream>\n");
  // The request with the wrong format made message 2: `create` ran before the 406.
  CHECK(scalar(f, "SELECT COUNT(*) FROM messages WHERE id = 1") == "0");
  CHECK(scalar(f, "SELECT COUNT(*) FROM action_text_rich_texts WHERE record_id = 1") == "0");
  CHECK(scalar(f, "SELECT COUNT(*) FROM message_search_index WHERE rowid = 1") == "0");
  CHECK(scalar(f, "SELECT COUNT(*) FROM messages") == "1");
  r = c.request("GET", "/rooms/1/messages/1", david);
  CHECK(r.status == 404);
}

TEST_CASE("boosts: create, list, delete") {
  Fixture f;
  seed_room(f);
  Client c(f.port());
  const std::string david = sign_in(c, "david@example.com");
  const std::string jason = sign_in(c, "jason@example.com");
  Reply r = c.request("POST", "/rooms/1/messages", david + kSameOrigin + kForm + kTurbo,
                      "message%5Bbody%5D=%3Cp%3EHello%3C%2Fp%3E&message%5Bclient_message_id%5D=cm-1");
  REQUIRE(r.status == 200);

  r = c.request("POST", "/messages/1/boosts", jason + kSameOrigin + kForm + kTurbo, "boost%5Bcontent%5D=%F0%9F%91%8D");
  CHECK(r.status == 302);
  CHECK(r.header("location") == "http://test.example/messages/1/boosts");
  CHECK(scalar(f, "SELECT content FROM boosts") == "\xF0\x9F\x91\x8D");
  CHECK(scalar(f, "SELECT booster_id FROM boosts") == "2");
  r = c.request("POST", "/messages/1/boosts", jason + kSameOrigin + kForm + kTurbo, "x=1");
  CHECK(r.status == 400);
  r = c.request("POST", "/messages/99/boosts", jason + kSameOrigin + kForm + kTurbo, "boost%5Bcontent%5D=x");
  CHECK(r.status == 404);

  r = c.request("GET", "/messages/1/boosts", jason);
  CHECK(r.status == 200);
  r = c.request("GET", "/messages/1/boosts/new", jason);
  CHECK(r.status == 200);
  CHECK(r.body.find("<input autofocus=\"autofocus\" autocomplete=\"off\"") != std::string::npos);
  r = c.request("GET", "/messages/1/boosts/1", jason);
  CHECK(r.status == 404);

  r = c.request("DELETE", "/messages/1/boosts/1", david + kSameOrigin + kTurbo);
  CHECK(r.status == 404);  // the booster only
  r = c.request("DELETE", "/messages/1/boosts/1", jason + kSameOrigin + kTurbo);
  CHECK(r.status == 204);
  CHECK(scalar(f, "SELECT COUNT(*) FROM boosts") == "0");
}

TEST_CASE("bot API: messages and boosts with the bot key") {
  Fixture f;
  seed_room(f);
  Client c(f.port());
  const std::string key = "3-BenderToken";
  const std::string base = "/rooms/1/" + key;

  Reply r = c.request("POST", base + "/messages", "Content-Type: text/plain\r\n", "Build 1044 passed");
  REQUIRE(r.status == 201);
  CHECK(r.header("location") == "http://test.example/messages/1");
  CHECK(scalar(f, "SELECT body FROM action_text_rich_texts") == "Build 1044 passed");
  CHECK(scalar(f, "SELECT creator_id FROM messages") == "3");
  r = c.request("POST", base + "/messages", "Content-Type: text/plain\r\n", " ");
  CHECK(r.status == 422);
  r = c.request("POST", "/rooms/1/3-wrong/messages", "Content-Type: text/plain\r\n", "x");
  CHECK(r.status == 302);  // not authenticated: sign in
  r = c.request("POST", "/rooms/2/" + key + "/messages", "Content-Type: text/plain\r\n", "x");
  CHECK(r.status == 404);

  r = c.request("GET", base + "/messages");
  REQUIRE(r.status == 200);
  CHECK(r.header("x-total-count") == "1");
  CHECK(r.header("content-type") == "application/json; charset=utf-8");
  CHECK(r.body.find("\"plain_text\":\"Build 1044 passed\"") != std::string::npos);
  CHECK(r.body.find("\"url\":\"http://test.example/rooms/1/messages/1\"") != std::string::npos);

  r = c.request("PUT", base + "/messages/1", "Content-Type: application/json\r\n", "{\"message\":{\"body\":\"edited\"}}");
  CHECK(r.status == 200);
  r = c.request("POST", base + "/messages/1/boosts", "Content-Type: text/plain\r\n", "\xF0\x9F\xA4\x96");
  REQUIRE(r.status == 201);
  CHECK(r.body.find("\"content\":\"\xF0\x9F\xA4\x96\"") != std::string::npos);
  r = c.request("DELETE", base + "/messages/1/boosts/1");
  CHECK(r.status == 204);
  r = c.request("DELETE", base + "/messages/1");
  CHECK(r.status == 204);
  CHECK(scalar(f, "SELECT COUNT(*) FROM messages") == "0");
}

TEST_CASE("autocompletable users: the mention prompt and the JSON list") {
  Fixture f;
  seed_room(f);
  Client c(f.port());
  const std::string david = sign_in(c, "david@example.com");
  Reply r = c.request("GET", "/autocompletable/users?query=j", david + "Accept: application/json\r\n");
  REQUIRE(r.status == 200);
  CHECK(r.header("x-total-count") == "1");
  CHECK(r.body.starts_with("[{\"name\":\"Jason\",\"value\":2,\"avatar_url\":\"http://test.example/users/"));
  r = c.request("GET", "/autocompletable/users?room_id=1&filter=a", david);
  REQUIRE(r.status == 200);
  CHECK(r.body.find("<lexxy-prompt-item search=\"David\" sgid=\"") != std::string::npos);
  r = c.request("GET", "/autocompletable/users?room_id=9", david);
  CHECK(r.status == 404);
}

TEST_CASE("message presenter helpers") {
  CHECK(all_emoji("\xF0\x9F\x91\x8D"));
  CHECK(all_emoji("\xE2\x9D\xA4\xEF\xB8\x8F"));
  CHECK_FALSE(all_emoji("hi \xF0\x9F\x91\x8D"));
  CHECK_FALSE(all_emoji(""));
  CHECK(to_sentence({"A", "B"}, " and ") == "A and B");
  CHECK(to_sentence({"A", "B", "C"}, " and ") == "A, B, and C");
  CHECK(to_sentence({}, " and ").empty());
  CHECK(models::sounds::sound_in("/play tada") != nullptr);
  CHECK(models::sounds::sound_in("/play nope") == nullptr);
  CHECK(models::sounds::sound_in("/play tada ") == nullptr);
}

}  // namespace campfire::app::testing
