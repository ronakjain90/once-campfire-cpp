// Tests of the link unfurl: the cases that the Rails reference answered (spec/vectors/opengraph_*.json: fake DNS and a
// fake server for the fake public addresses), and the limits of the outbound policy. Rust: crates/campfire/src/
// integrations/opengraph/tests.rs.
#include <arpa/inet.h>
#include <doctest.h>

#include <fstream>
#include <map>
#include <set>
#include <sstream>

#include "app/opengraph/html.hpp"
#include "app/opengraph/opengraph.hpp"
#include "app/tests/fake_http_server.hpp"
#include "compat/base64.hpp"
#include "compat/json.hpp"

namespace campfire::app {

namespace {

using compat::json::Value;
using unfurl::Clock;

Value read_json(const std::string& name) {
  std::ifstream file(std::string(CAMPFIRE_SPEC_DIR) + "/vectors/" + name);
  REQUIRE(file.good());
  std::stringstream text;
  text << file.rdbuf();
  auto parsed = compat::json::parse(text.str());
  REQUIRE(parsed);
  return std::move(*parsed);
}

std::string address_bytes(const std::string& text) {
  unsigned char raw[16];
  if (inet_pton(AF_INET, text.c_str(), raw) == 1) return std::string(reinterpret_cast<char*>(raw), 4);
  REQUIRE(inet_pton(AF_INET6, text.c_str(), raw) == 1);
  return std::string(reinterpret_cast<char*>(raw), 16);
}

// Fixed answers for each host. A host with several lists gives the next one on each lookup, then the last one again.
struct FakeDns {
  std::map<std::string, std::vector<std::vector<std::string>>> hosts;
  std::vector<std::string> lookups;
  std::mutex mutex;

  HostLookup lookup() {
    return [this](const std::string& host) {
      std::scoped_lock lock(mutex);
      // getaddrinfo reads numeric forms (2130706433, 0x7f.1) without asking DNS.
      in_addr numeric{};
      if (inet_aton(host.c_str(), &numeric) != 0) return std::vector<std::string>{std::string(reinterpret_cast<char*>(&numeric), 4)};
      lookups.push_back(host);
      std::vector<std::string> out;
      const auto it = hosts.find(host);
      if (it == hosts.end()) return out;
      auto& lists = it->second;
      const std::vector<std::string> list = lists.front();
      if (lists.size() > 1) lists.erase(lists.begin());
      for (const std::string& text : list) out.push_back(address_bytes(text));
      return out;
    };
  }
};

unfurl::Network network_to(FakeDns& dns, std::uint16_t port, std::set<std::string> public_ips) {
  unfurl::Network network;
  network.lookup = dns.lookup();
  network.dial_override = [port, public_ips](std::string& ip, std::uint16_t& target) {
    if (public_ips.contains(ip)) {
      ip = "127.0.0.1";
      target = port;
    }
  };
  return network;
}

std::string decode_b64(const std::string& text) { return *compat::base64::strict_decode(text); }

test::FakeRoute route_of(const Value& spec) {
  const auto text = [&](const char* key) {
    const Value* value = spec.find(key);
    return value != nullptr && value->is_string() ? value->as_string() : std::string();
  };
  test::FakeRoute route;
  route.method = text("method");
  route.host = text("host");
  route.path = text("path");
  route.status = static_cast<int>(*spec.find("status")->to_int64());
  for (const Value& header : spec.find("headers")->as_array()) {
    route.headers.emplace_back(header.as_array()[0].as_string(), header.as_array()[1].as_string());
  }
  if (const Value* b64 = spec.find("body_b64")) {
    route.body = decode_b64(b64->as_string());
  } else if (const Value* repeat = spec.find("body_repeat")) {
    route.body.assign(static_cast<std::size_t>(*repeat->as_array()[1].to_int64()), repeat->as_array()[0].as_string()[0]);
  } else {
    route.body = text("body");
  }
  if (const Value* pad = spec.find("pad_to")) route.body.resize(static_cast<std::size_t>(*pad->to_int64()), ' ');
  if (const Value* chunked = spec.find("chunked")) route.chunked = chunked->as_bool();
  if (const Value* gzip = spec.find("gzip")) route.gzip = gzip->as_bool();
  return route;
}

}  // namespace

TEST_CASE("unfurl: the cases of the Rails reference") {
  const Value spec = read_json("opengraph_cases.json");
  const Value expected = read_json("opengraph_expected.json");
  std::vector<test::FakeRoute> routes;
  for (const Value& r : spec.find("routes")->as_array()) routes.push_back(route_of(r));
  test::FakeServer server(std::move(routes));
  std::set<std::string> public_ips;
  for (const Value& ip : spec.find("public_ips")->as_array()) public_ips.insert(ip.as_string());
  const auto& cases = spec.find("cases")->as_array();
  const auto& wanted = expected.as_array();
  REQUIRE(cases.size() == wanted.size());
  std::size_t checked = 0;
  for (std::size_t i = 0; i < cases.size(); ++i) {
    const std::string name = cases[i].find("name")->as_string();
    REQUIRE(name == wanted[i].find("name")->as_string());
    FakeDns dns;
    for (const auto& [host, lists] : spec.find("hosts")->as_object()) {
      for (const Value& list : lists.as_array()) {
        std::vector<std::string> ips;
        for (const Value& ip : list.as_array()) ips.push_back(ip.as_string());
        dns.hosts[host].push_back(std::move(ips));
      }
    }
    const unfurl::Network network = network_to(dns, server.port(), public_ips);
    const std::size_t before = server.received().size();
    const auto result = opengraph::unfurl(network, cases[i].find("url")->as_string(), Clock::now() + std::chrono::seconds(10));
    Value::Object response;
    if (!result) {
      response.emplace_back("status", 500);
      response.emplace_back("error", result.error().message);
    } else if (result->has_content) {
      response.emplace_back("status", 200);
      response.emplace_back("body", result->json);
    } else {
      response.emplace_back("status", 204);
    }
    Value::Array requests;
    const auto received = server.received();
    for (std::size_t r = before; r < received.size(); ++r) {
      const auto& item = received[r];
      requests.push_back(Value(Value::Array{item.method, item.header("host"), item.target, item.header("accept"),
                                            item.header("accept-encoding"), item.header("user-agent")}));
    }
    Value::Array lookups;
    for (const std::string& host : dns.lookups) lookups.emplace_back(host);
    const Value actual(Value::Object{{"response", Value(std::move(response))}, {"lookups", Value(std::move(lookups))},
                                     {"requests", Value(std::move(requests))}});
    const Value want(Value::Object{{"response", *wanted[i].find("response")}, {"lookups", *wanted[i].find("lookups")},
                                   {"requests", *wanted[i].find("requests")}});
    INFO(name);
    INFO("expected ", compat::json::generate(want));
    INFO("actual   ", compat::json::generate(actual));
    CHECK(actual == want);
    ++checked;
  }
  CHECK(checked == 90);
}

}  // namespace campfire::app
