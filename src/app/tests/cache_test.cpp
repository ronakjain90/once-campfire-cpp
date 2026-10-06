// Tests of the session cache and its change hub, the fragment cache, the page cache and the rate limiter.
#include <doctest.h>

#include "app/fragment_cache.hpp"
#include "app/page_cache.hpp"
#include "app/rate_limit.hpp"
#include "app/session_cache.hpp"
#include "app/tests/fixture.hpp"
#include "app/worker_state.hpp"
#include "core/out.hpp"
#include "models/session.hpp"
#include "views/runtime.hpp"

namespace campfire::app {

namespace {

std::shared_ptr<const AuthEntry> entry(std::int64_t session_id, std::int64_t user_id) {
  AuthEntry e;
  e.session.id = session_id;
  e.session.user_id = user_id;
  e.user.id = user_id;
  return std::make_shared<const AuthEntry>(std::move(e));
}

}  // namespace

TEST_CASE("session cache: a change drops the entries of that session or user") {
  SessionCache cache;
  cache.put("cookie-a", entry(1, 10));
  cache.put("cookie-b", entry(2, 10));
  cache.put("cookie-c", entry(3, 11));
  REQUIRE(cache.size() == 3);
  const db::Change one[] = {{db::schema::Table::Sessions, 1}};
  cache.invalidate(one);
  CHECK(cache.find("cookie-a") == nullptr);
  CHECK(cache.find("cookie-b") != nullptr);
  const db::Change user[] = {{db::schema::Table::Users, 10}};
  cache.invalidate(user);
  CHECK(cache.find("cookie-b") == nullptr);
  CHECK(cache.find("cookie-c") != nullptr);
  const db::Change other[] = {{db::schema::Table::Messages, 3}};
  cache.invalidate(other);  // not a cached table
  CHECK(cache.find("cookie-c") != nullptr);
}

TEST_CASE("session cache: the writer clears the caches of every worker") {
  testing::Fixture f;
  // Two workers, each with its own cache and inbox (they share the thread here, as in a test).
  WorkerState first(*f.state);
  WorkerState second(*f.state);
  REQUIRE(f.state->changes->size() >= 2);
  QueueScheduler scheduler;
  auto started = db::testing::run_task(scheduler, f.state->db->write(scheduler, [&](db::Tx& tx) {
    return models::sessions::start(tx, 1, "agent", "10.0.0.1");
  }));
  REQUIRE(started.has_value());
  first.sessions().put("raw-cookie", entry(started->id, 1));
  second.sessions().put("raw-cookie", entry(started->id, 1));
  CHECK(first.sessions().find("raw-cookie") != nullptr);

  // Destroying the session posts a change to both inboxes. Each worker applies it before its next lookup.
  const models::Session session = *started;
  auto destroyed = db::testing::run_task(scheduler, f.state->db->write(scheduler, [&](db::Tx& tx) -> Status {
    return models::sessions::destroy(tx, session);
  }));
  REQUIRE(destroyed.has_value());
  CHECK(first.inbox().pending());
  CHECK(second.inbox().pending());
  CHECK(first.sessions().find("raw-cookie") == nullptr);
  CHECK(second.sessions().find("raw-cookie") == nullptr);
  CHECK_FALSE(first.inbox().pending());
}

TEST_CASE("change hub: only sessions and users reach the inboxes") {
  ChangeHub hub;
  ChangeInbox a;
  ChangeInbox b;
  hub.add(&a);
  hub.add(&b);
  const db::Change messages[] = {{db::schema::Table::Messages, 5}};
  hub.publish(messages);
  CHECK_FALSE(a.pending());
  const db::Change mixed[] = {{db::schema::Table::Messages, 5}, {db::schema::Table::Users, 9}};
  hub.publish(mixed);
  CHECK(a.pending());
  const auto taken = b.take();
  REQUIRE(taken.size() == 1);
  CHECK(taken[0].id == 9);
  hub.remove(&b);
  hub.publish(mixed);
  CHECK_FALSE(b.pending());
  CHECK(a.take().size() == 2);
}

TEST_CASE("fragment cache: bounded by bytes, least recently used goes first") {
  SharedFragmentCache cache(64 * 1024, 1);
  const std::string html(1000, 'x');
  for (int i = 0; i < 200; ++i) cache.put("key-" + std::to_string(i), html);
  CHECK(cache.bytes() <= cache.max_bytes());
  CHECK(cache.entries() < 200);
  CHECK(cache.entries() > 30);
  CHECK(cache.get("key-199") != nullptr);
  CHECK(cache.get("key-0") == nullptr);
  // A read makes an entry recent.
  SharedFragmentCache small(4 * 1024, 1);
  small.put("a", std::string(1500, 'a'));
  small.put("b", std::string(1500, 'b'));
  REQUIRE(small.get("a") != nullptr);
  small.put("c", std::string(1500, 'c'));
  CHECK(small.get("a") != nullptr);
  CHECK(small.get("b") == nullptr);
  // An entry that cannot fit is not stored.
  small.put("huge", std::string(10000, 'h'));
  CHECK(small.get("huge") == nullptr);
  // A new value replaces the old one and the size stays right.
  small.put("a", "short");
  CHECK(*small.get("a") == "short");
  CHECK(small.bytes() < 4 * 1024);
  small.clear();
  CHECK(small.bytes() == 0);
  CHECK(small.entries() == 0);
}

TEST_CASE("fragment cache: the cache block of a template reads and writes it") {
  SharedFragmentCache shared(1 << 20);
  WorkerFragmentCache worker(shared);
  views::set_fragment_cache(&worker);
  int renders = 0;
  const auto render = [&] {
    Out out;
    views::cached(out, std::string("[message, presentation-v3]"), [&](Out& o) {
      ++renders;
      o.append_raw("<p>hello</p>");
    });
    return out.to_string();
  };
  CHECK(render() == "<p>hello</p>");
  CHECK(render() == "<p>hello</p>");
  CHECK(renders == 1);
  CHECK(shared.entries() == 1);
  views::set_fragment_cache(nullptr);
}

TEST_CASE("page cache: miss, hit, gzip, ETag") {
  PageCache cache({.max_bytes = 1 << 20, .shards = 2, .audit_every = 0});
  const Hash128 key = xxh3_128("page one");
  CHECK(cache.get(key) == nullptr);
  const std::string body(5000, 'p');
  const auto stored = cache.put(key, body, "text/html; charset=utf-8");
  REQUIRE(stored != nullptr);
  const auto hit = cache.get(key);
  REQUIRE(hit != nullptr);
  CHECK(hit->identity == body);
  CHECK(testing::gunzip(hit->gzip) == body);
  CHECK(hit->gzip.size() < body.size());
  CHECK(hit->etag == body_etag(body));
  CHECK(hit->etag.size() == 36);
  CHECK(cache.hits() == 1);
  CHECK(cache.misses() == 1);
  // The ETag of a page can also come from the key.
  const auto keyed = cache.put(xxh3_128("page two"), "two", "text/html", key_etag(xxh3_128("page two")));
  CHECK(keyed->etag == "W/\"" + xxh3_128("page two").hex() + "\"");
  // The same body gives the same Rack::ETag.
  CHECK(body_etag("abc") == "W/\"ba7816bf8f01cfea414140de5dae2223\"");
}

TEST_CASE("page cache: bounded by bytes") {
  PageCache cache({.max_bytes = 64 * 1024, .shards = 1, .audit_every = 0});
  for (int i = 0; i < 100; ++i)
    cache.put(xxh3_128("p" + std::to_string(i)), std::string(2000, static_cast<char>('a' + i % 26)), "text/html");
  CHECK(cache.bytes() <= 64 * 1024);
  CHECK(cache.entries() < 100);
  CHECK(cache.get(xxh3_128("p99")) != nullptr);
  CHECK(cache.get(xxh3_128("p0")) == nullptr);
  // A page bigger than a shard is returned but not stored.
  const auto big = cache.put(xxh3_128("big"), std::string(200 * 1024, 'b'), "text/html");
  CHECK(big != nullptr);
  CHECK(cache.get(xxh3_128("big")) == nullptr);
}

TEST_CASE("page cache: the audit finds a page that changed without a new key") {
  PageCache cache({.max_bytes = 1 << 20, .shards = 1, .audit_every = 4});
  int due = 0;
  for (int i = 0; i < 16; ++i) due += cache.audit_due() ? 1 : 0;
  CHECK(due == 4);
  std::vector<std::string> failures;
  cache.set_failure_handler([&](std::string_view message) { failures.emplace_back(message); });
  const Hash128 key = xxh3_128("audited");
  const auto entry = cache.put(key, "<p>old</p>", "text/html");
  CHECK(cache.audit_compare(key, *entry, "<p>old</p>"));
  CHECK(failures.empty());
  CHECK_FALSE(cache.audit_compare(key, *entry, "<p>new</p>"));
  REQUIRE(failures.size() == 1);
  CHECK(failures[0].find("first difference at byte 3") != std::string::npos);
  CHECK(cache.audits() == 2);
  PageCache never({.max_bytes = 1024, .shards = 1, .audit_every = 0});
  CHECK_FALSE(never.audit_due());
}

TEST_CASE("rate limiter: a fixed window that starts at the first hit") {
  RateLimiter limiter;
  const Timestamp start = Timestamp::from_seconds(1000);
  for (std::uint64_t i = 1; i <= 11; ++i)
    CHECK(limiter.increment("ip", start.plus_seconds(static_cast<std::int64_t>(i)), 180) == i);
  CHECK(limiter.increment("other", start.plus_seconds(12), 180) == 1);
  // The window does not slide: it ended 180 seconds after the first hit.
  CHECK(limiter.increment("ip", start.plus_seconds(181 + 5), 180) == 1);
  CHECK(limiter.increment("ip", start.plus_seconds(190), 180) == 2);
}

}  // namespace campfire::app
