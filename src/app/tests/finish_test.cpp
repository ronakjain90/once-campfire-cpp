// Tests of the finish pipeline and the page cache through the sign-in page: ETag, 304, gzip, header order.
#include <doctest.h>

#include "app/tests/fixture.hpp"

namespace campfire::app::testing {

namespace {

std::size_t position(const Reply& r, const char* name) {
  const auto names = r.header_names();
  return static_cast<std::size_t>(std::find(names.begin(), names.end(), name) - names.begin());
}

const db::Query<void(std::string_view)> kRenameAccount{"UPDATE accounts SET name = ?"};

}  // namespace

TEST_CASE("page cache: a miss renders, a hit sends the entry, the audit passes") {
  AppOptions options;
  options.audit_every = 1;  // compare each hit with a fresh render
  Fixture f(options);
  std::vector<std::string> failures;
  f.state->pages.set_failure_handler([&](std::string_view m) { failures.emplace_back(m); });
  Client c(f.port());
  const Reply first = c.request("GET", "/session/new");
  REQUIRE(first.status == 200);
  CHECK(f.state->pages.misses() == 1);
  CHECK(f.state->pages.entries() == 1);
  const Reply second = c.request("GET", "/session/new");
  CHECK(f.state->pages.hits() == 1);
  CHECK(second.body == first.body);
  CHECK(second.header("etag") == first.header("etag"));
  CHECK(f.state->pages.audits() == 1);
  CHECK(failures.empty());

  // The ETag is the one of Rack::ETag: the digest of the body.
  CHECK(first.header("etag") == body_etag(first.body));

  // Another input makes another page.
  const Reply other = c.request("GET", "/session/new?email_address=a%40b.example");
  CHECK(other.body != first.body);
  CHECK(other.body.find("value=\"a@b.example\"") != std::string::npos);
  CHECK(f.state->pages.entries() == 2);

  // A change in the data that the page read makes another key.
  QueueScheduler scheduler;
  REQUIRE(db::testing::run_task(scheduler, f.state->db->write(scheduler, [&](db::Tx& tx) -> Status {
            auto r = tx.conn().exec(kRenameAccount, "Renamed");
            if (!r) return std::unexpected(r.error());
            return {};
          })).has_value());
  const Reply renamed = c.request("GET", "/session/new");
  CHECK(renamed.body.find("<strong>Renamed</strong>") != std::string::npos);
  CHECK(renamed.header("etag") != first.header("etag"));
  CHECK(failures.empty());
}

TEST_CASE("conditional GET: 304 keeps the ETag, drops the type and the length") {
  Fixture f;
  Client c(f.port());
  const std::string any = "Accept: */*\r\n";  // `Vary: Accept` needs an Accept header
  const Reply page = c.request("GET", "/session/new", any);
  const std::string etag = page.header("etag");
  Reply r = c.request("GET", "/session/new", any + "If-None-Match: " + etag + "\r\n");
  CHECK(r.status == 304);
  CHECK(r.header("etag") == etag);
  CHECK(r.header("content-type").empty());
  CHECK(r.header("content-length").empty());
  CHECK(r.body.empty());
  // HeaderMap::remove moves the last header (cache-control) into the place of content-type.
  CHECK(r.headers.front().first == "cache-control");
  CHECK(r.header("vary") == "Accept");  // the deflater leaves a 304 alone
  r = c.request("GET", "/session/new", "If-None-Match: W/\"nope\", " + etag + "\r\n");
  CHECK(r.status == 304);
  r = c.request("GET", "/session/new", "If-None-Match: W/\"nope\"\r\n");
  CHECK(r.status == 200);
  r = c.request("HEAD", "/session/new", "If-None-Match: " + etag + "\r\n");
  CHECK(r.status == 304);
}

TEST_CASE("gzip: the encoding takes the place of the length, the vary header changes in place") {
  Fixture f;
  Client c(f.port());
  const Reply plain = c.request("GET", "/session/new", "Accept: */*\r\nAccept-Encoding: identity\r\n");
  const Reply gz = c.request("GET", "/session/new", "Accept: */*\r\nAccept-Encoding: gzip, br\r\n");
  REQUIRE(gz.status == 200);
  CHECK(gz.header("content-encoding") == "gzip");
  CHECK(gz.header("transfer-encoding") == "chunked");
  CHECK(gz.header("content-length").empty());
  CHECK(gunzip(gz.body) == plain.body);
  CHECK(gz.header("etag") == plain.header("etag"));
  CHECK(position(gz, "content-encoding") == position(plain, "content-length"));
  CHECK(position(gz, "x-request-id") == position(gz, "content-encoding") + 1);
  CHECK(position(gz, "vary") == 1);
  // An empty body (a redirect) is gzipped too, and gets Vary after the tail.
  const Reply redirect = c.request("GET", "/", "Accept-Encoding: gzip\r\n");
  CHECK(redirect.status == 302);
  CHECK(redirect.header("content-encoding") == "gzip");
  CHECK(redirect.header_names().back() == "transfer-encoding");
  CHECK(position(redirect, "vary") == position(redirect, "x-runtime") + 1);
  const Reply identity_redirect = c.request("GET", "/", "Accept-Encoding: identity\r\n");
  CHECK(identity_redirect.header("vary") == "Accept-Encoding");
}

TEST_CASE("errors: the public pages and their headers") {
  Fixture f;
  Client c(f.port());
  // Invalid UTF-8 in the query is a 400 with an empty body (no public/400.html).
  Reply r = c.request("GET", "/session/new?email_address=%FF");
  CHECK(r.status == 400);
  CHECK(r.header("content-length") == "0");
  CHECK(r.header("content-type") == "text/html; charset=UTF-8");
  r = c.request("POST", "/session", "Sec-Fetch-Site: cross-site\r\nAccept-Encoding: gzip\r\n", "a=b");
  CHECK(r.status == 422);
  CHECK(r.header("content-encoding") == "gzip");
  CHECK(gunzip(r.body).find("<title>That didn’t work (422)</title>") != std::string::npos);
  r = c.request("POST", "/session", "Sec-Fetch-Site: cross-site\r\nAccept: application/json\r\n", "a=b");
  CHECK(r.body == R"({"status":422,"error":"Unprocessable Content"})");
}

}  // namespace campfire::app::testing
