// Tests of the outbound policy of the link unfurl: private addresses, the pinned address, redirects, the caps, the
// timeouts and the HTML scanner. Rust: crates/campfire/src/integrations/opengraph/{html,fetch}.rs (tests).
#include <doctest.h>

#include <set>

#include "app/opengraph/html.hpp"
#include "app/opengraph/opengraph.hpp"
#include "app/tests/fake_http_server.hpp"

namespace campfire::app {

namespace {

using unfurl::Clock;

constexpr const char* kPage =
    "<meta property=\"og:title\" content=\"Hey!\"><meta property=\"og:url\" content=\"http://www.example.com/\">"
    "<meta property=\"og:description\" content=\"desc..\">";

// www.example.com at a fake public address, which connects to the local server.
unfurl::Network network_to(std::uint16_t port, std::vector<std::string>* dialed = nullptr) {
  unfurl::Network network;
  network.lookup = [](const std::string& host) {
    std::vector<std::string> out;
    if (host == "www.example.com") out.push_back(std::string("\x5d\xb8\xd8\x22", 4));  // 93.184.216.34
    if (host == "private.example") out.push_back(std::string("\x0a\x00\x00\x01", 4));   // 10.0.0.1
    return out;
  };
  network.dial_override = [port, dialed](std::string& ip, std::uint16_t& target) {
    if (dialed != nullptr) dialed->push_back(ip + ":" + std::to_string(target));
    if (ip == "93.184.216.34") {
      ip = "127.0.0.1";
      target = port;
    }
  };
  return network;
}

test::FakeRoute page(const std::string& path, const std::string& body) {
  return {"GET", "*", path, 200, {{"Content-Type", "text/html"}}, body};
}

Result<opengraph::Unfurl> run(const unfurl::Network& network, const std::string& url,
                              std::chrono::milliseconds budget = std::chrono::seconds(10)) {
  return opengraph::unfurl(network, url, Clock::now() + budget);
}

std::optional<std::string> title_of(std::string_view html) {
  std::optional<std::string> found;
  for (const auto& [name, value] : opengraph::opengraph_attributes(html)) {
    if (name == "title") found = value;
  }
  return found;
}

}  // namespace

TEST_CASE("unfurl policy: private addresses are never connected to") {
  std::vector<std::string> dialed;
  test::FakeServer server({page("/", kPage)});
  const auto network = network_to(server.port(), &dialed);
  for (const char* url : {"http://private.example/", "http://127.0.0.1/", "http://10.1.2.3/", "http://[::1]/",
                          "http://169.254.169.254/latest/meta-data/", "http://localhost/"}) {
    const auto result = run(network, url);
    REQUIRE(result);
    CHECK_FALSE(result->has_content);
  }
  CHECK(dialed.empty());
  CHECK(server.received().empty());
}

TEST_CASE("unfurl policy: the address that the guard checked is the one that is connected to") {
  std::vector<std::string> dialed;
  test::FakeServer server({page("/", kPage)});
  const auto result = run(network_to(server.port(), &dialed), "http://www.example.com/");
  REQUIRE(result);
  CHECK(result->has_content);
  REQUIRE(dialed.size() == 1);
  CHECK(dialed[0] == "93.184.216.34:80");
}

TEST_CASE("unfurl policy: a redirect to a private address is not followed") {
  test::FakeServer server({{"GET", "*", "/", 302, {{"Location", "http://private.example/secret"}}, ""},
                           {"GET", "*", "/ip", 301, {{"Location", "http://10.0.0.1/"}}, ""}});
  std::vector<std::string> dialed;
  const auto network = network_to(server.port(), &dialed);
  CHECK_FALSE(run(network, "http://www.example.com/")->has_content);
  CHECK_FALSE(run(network, "http://www.example.com/ip")->has_content);
  CHECK(dialed == std::vector<std::string>{"93.184.216.34:80", "93.184.216.34:80"});
}

TEST_CASE("unfurl policy: ten responses at most") {
  std::vector<test::FakeRoute> routes;
  const auto redirect = [](const std::string& from, const std::string& to) {
    return test::FakeRoute{"GET", "*", from, 302, {{"Location", "http://www.example.com" + to}}, ""};
  };
  // r0 .. r8 redirect and r9 answers: ten responses. s0 .. s9 redirect and s10 answers: eleven.
  for (int i = 0; i < 9; ++i) routes.push_back(redirect("/r" + std::to_string(i), "/r" + std::to_string(i + 1)));
  routes.push_back(page("/r9", kPage));
  for (int i = 0; i < 10; ++i) routes.push_back(redirect("/s" + std::to_string(i), "/s" + std::to_string(i + 1)));
  routes.push_back(page("/s10", kPage));
  test::FakeServer server(std::move(routes));
  const auto network = network_to(server.port());
  CHECK(run(network, "http://www.example.com/r0")->has_content);
  const auto before = server.received().size();
  CHECK_FALSE(run(network, "http://www.example.com/s0")->has_content);
  CHECK(server.received().size() - before == 10);
}

TEST_CASE("unfurl policy: the body is at most 5 MB, by Content-Length and by what is read") {
  const std::string head = kPage;
  test::FakeRoute exact = page("/exact", head);
  exact.body.resize(opengraph::kMaxBodySize, ' ');
  exact.chunked = true;
  test::FakeRoute big = page("/big", head);
  big.body.resize(opengraph::kMaxBodySize + 1, ' ');
  big.chunked = true;
  test::FakeRoute length = page("/length", head);
  length.headers.emplace_back("Content-Length", std::to_string(opengraph::kMaxBodySize + 1));
  test::FakeServer server({exact, big, length});
  const auto network = network_to(server.port());
  CHECK(run(network, "http://www.example.com/exact")->has_content);
  CHECK_FALSE(run(network, "http://www.example.com/big")->has_content);
  CHECK_FALSE(run(network, "http://www.example.com/length")->has_content);
}

TEST_CASE("unfurl policy: a gzip bomb stops at the limit") {
  std::string zeros(1 << 20, '\0');
  const std::string member = test::gzip_member(zeros);
  const std::string packed_page = test::gzip_member(kPage);
  const auto bomb = [&](const std::string& path, int members) {
    std::string body = packed_page;
    for (int i = 0; i < members; ++i) body += member;
    test::FakeRoute route = page(path, body);
    route.headers.emplace_back("Content-Encoding", "gzip");
    return route;
  };
  test::FakeServer server({bomb("/", 1024), bomb("/small", 2)});
  const auto network = network_to(server.port());
  CHECK(run(network, "http://www.example.com/small")->has_content);
  const auto started = Clock::now();
  CHECK_FALSE(run(network, "http://www.example.com/")->has_content);
  CHECK(Clock::now() - started < std::chrono::seconds(2));
}

TEST_CASE("unfurl policy: a read that takes too long gives up") {
  test::FakeRoute slow = page("/", kPage);
  slow.delay = std::chrono::milliseconds(600);
  test::FakeServer server({slow});
  auto network = network_to(server.port());
  network.timeouts.read = std::chrono::milliseconds(200);
  const auto started = Clock::now();
  CHECK_FALSE(run(network, "http://www.example.com/")->has_content);
  CHECK(Clock::now() - started < std::chrono::milliseconds(550));
}

TEST_CASE("unfurl policy: a page that trickles in does not outlast the deadline") {
  test::FakeRoute trickle = page("/", "");
  trickle.trickle = true;
  test::FakeServer server({trickle});
  const auto started = Clock::now();
  CHECK_FALSE(run(network_to(server.port()), "http://www.example.com/", std::chrono::milliseconds(500))->has_content);
  CHECK(Clock::now() - started < std::chrono::milliseconds(1000));
}

TEST_CASE("unfurl policy: over TLS the certificate is checked against the host name") {
  const std::string dir = std::string(CAMPFIRE_SPEC_DIR) + "/vectors/tls/";
  test::FakeServer server({{"GET", "www.example.com", "/", 200, {{"Content-Type", "text/html"}}, kPage},
                           {"HEAD", "example.com", "/image.png", 200, {{"Content-Type", "image/png"}}, ""}},
                          dir + "server.pem", dir + "server.key");
  std::vector<std::string> dialed;
  auto network = network_to(server.port(), &dialed);
  network.ca_file = dir + "ca.pem";
  const auto trusted = run(network, "https://www.example.com/");
  REQUIRE(trusted);
  CHECK(trusted->has_content);
  REQUIRE(!dialed.empty());
  CHECK(dialed[0] == "93.184.216.34:443");
  // A certificate that does not verify is a failed fetch.
  network.ca_file = std::string(CAMPFIRE_SPEC_DIR) + "/vectors/tls/server.key";
  CHECK_FALSE(run(network, "https://www.example.com/")->has_content);
}

TEST_CASE("opengraph html: a tag keeps 256 attributes") {
  std::string html = "<meta charset=utf-8><meta property=\"og:title\"";
  for (int i = 0; i < 256; ++i) html += " a" + std::to_string(i);
  html += " content=\"late\">";
  CHECK_FALSE(title_of(html));
  std::string few = "<meta charset=utf-8><meta property=\"og:title\"";
  for (int i = 0; i < 254; ++i) few += " a" + std::to_string(i);
  few += " content=\"early\">";
  CHECK(title_of(few) == "early");
}

TEST_CASE("opengraph html: references decode like libxml2") {
  const auto title = [](const std::string& content) {
    return title_of("<meta charset=utf-8><meta property=\"og:title\" content=\"a" + content + "b\">");
  };
  CHECK(title("&apos;") == "a'b");
  CHECK(title("&eacute") == "a&eacuteb");
  CHECK(title("&eacute;x") == "a\xC3\xA9xb");
  CHECK(title("&#233") == "a\xC3\xA9" "b");
  CHECK(title("&AMP;") == "a&AMP;b");
  CHECK(title("&unknown;") == "a&unknown;b");
  CHECK(title("&#xD800;") == "a");
  CHECK(title("&amp;amp;") == "a&amp;b");
  CHECK(title("&#;") == "a");
}

TEST_CASE("opengraph html: the scanner skips what libxml2 skips") {
  const auto title = [](const std::string& html) { return title_of("<meta charset=utf-8>" + html); };
  CHECK(title("<script><meta property=\"og:title\" content=\"x\"></script><meta property=\"og:title\" content=\"after\">") == "after");
  CHECK_FALSE(title("<style><meta property=\"og:title\" content=\"x\"></style>"));
  CHECK_FALSE(title("<!-- <meta property=\"og:title\" content=\"x\"> --><p>"));
  CHECK(title("<!--> <meta property=\"og:title\" content=\"x\"> -->") == "x");
  CHECK(title("<meta property=og:title content=unquoted>") == "unquoted");
  CHECK(title("<META PROPERTY=\"og:title\" CONTENT=\"upper\">") == "upper");
  CHECK_FALSE(title("<![CDATA[ <meta property=\"og:title\" content=\"x\"> ]]>"));
}

}  // namespace campfire::app
