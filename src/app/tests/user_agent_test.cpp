// Vectors of the useragent gem and of ApplicationPlatform (tests/vectors/campfire_user_agents.json).
// Rust: the tests of crates/campfire/src/concerns/{user_agent,platform}.rs.
#include "app/user_agent.hpp"

#include <doctest.h>

#include <cstdio>
#include <fstream>
#include <sstream>

#include "app/platform.hpp"
#include "compat/json.hpp"

namespace campfire::app {
namespace {

namespace json = compat::json;

const json::Value& vectors() {
  static const json::Value value = [] {
    std::ifstream in(CAMPFIRE_TESTS_DIR "/vectors/campfire_user_agents.json", std::ios::binary);
    REQUIRE(in.good());
    std::stringstream buffer;
    buffer << in.rdbuf();
    auto parsed = json::parse(buffer.str());
    REQUIRE(parsed.has_value());
    return std::move(*parsed);
  }();
  return value;
}

const json::Value& at(const json::Value& v, std::string_view key) {
  static const json::Value null;
  const json::Value* m = v.find(key);
  return m != nullptr ? *m : null;
}

struct Tally {
  int total = 0;
  int pass = 0;
  std::vector<std::string> failures;
  void check(bool ok, const std::string& label) {
    ++total;
    if (ok) {
      ++pass;
    } else if (failures.size() < 10) {
      failures.push_back(label);
    }
  }
};

// A Ruby value that raised is `{"error": ...}` in the vectors.
bool raised(const json::Value& expected) {
  return expected.is_object() && expected.find("error") != nullptr;
}

template <class T, class ToJson>
bool matches(const json::Value& expected, const ua::Rb<T>& actual, ToJson to_json) {
  if (raised(expected)) return !actual.has_value();
  return actual.has_value() && to_json(*actual) == expected;
}

json::Value text(const std::optional<std::string>& s) {
  return s ? json::Value(*s) : json::Value();
}

}  // namespace

TEST_CASE("user agent vectors: the gem") {
  Tally t;
  for (const json::Value& c : vectors().find("user_agents")->as_array()) {
    const std::string ua = at(c, "ua").is_string() ? at(c, "ua").as_string() : std::string{};
    const ua::Agent agent = ua::parse(ua);
    t.check(matches(at(c, "browser"), agent.try_browser(), text), ua + " browser");
    t.check(matches(at(c, "version"), agent.try_version(),
                    [](const std::optional<ua::Version>& v) { return v ? json::Value(v->str()) : json::Value(); }),
            ua + " version");
    t.check(matches(at(c, "platform"), agent.try_platform(), text), ua + " platform");
    t.check(matches(at(c, "os"), agent.try_os(), text), ua + " os");
    t.check(at(c, "bot") == json::Value(agent.is_bot()), ua + " bot");
    t.check(matches(at(c, "mobile"), agent.try_mobile(), [](bool b) { return json::Value(b); }), ua + " mobile");
  }
  std::printf("VECTORS campfire_user_agents.json user_agents.gem cases=%d pass=%d\n", t.total, t.pass);
  std::string detail;
  for (const auto& f : t.failures) detail += "\n  " + f;
  CHECK_MESSAGE(t.pass == t.total, (t.total - t.pass) << " failures" << detail);
}

TEST_CASE("user agent vectors: ApplicationPlatform and allow_browser") {
  Tally t;
  for (const json::Value& c : vectors().find("user_agents")->as_array()) {
    const std::string ua = at(c, "ua").is_string() ? at(c, "ua").as_string() : std::string{};
    const ApplicationPlatform platform(ua);
    const json::Value& expected = at(c, "application_platform");
    const ua::Agent& agent = platform.agent();
    const auto browser = agent.try_browser();
    const auto os = platform.try_operating_system();
    const auto flag = [&](std::string_view name, bool actual) {
      t.check(at(expected, name) == json::Value(actual), ua + " " + std::string(name));
    };
    const auto raises_or = [&](std::string_view name, const ua::Rb<bool>& actual) {
      t.check(matches(at(expected, name), actual, [](bool b) { return json::Value(b); }), ua + " " + std::string(name));
    };
    const auto name_is = [&](std::initializer_list<std::string_view> names) -> ua::Rb<bool> {
      if (!browser || !*browser) return std::unexpected(ua::Raised{});
      for (std::string_view n : names) {
        if (browser->value().find(n) != std::string::npos) return true;
      }
      return false;
    };
    flag("ios", platform.ios());
    flag("android", platform.android());
    flag("mac", platform.mac());
    raises_or("chrome", name_is({"Chrome"}));
    raises_or("firefox", name_is({"Firefox", "FxiOS"}));
    raises_or("safari", name_is({"Safari"}));
    raises_or("edge", name_is({"Edg"}));
    flag("apple_messages", platform.apple_messages());
    flag("mobile", platform.mobile());
    flag("desktop", platform.desktop());
    t.check(matches(at(expected, "windows"), os,
                    [](const std::optional<std::string>& o) {
                      return json::Value(o == std::optional<std::string>("Windows"));
                    }),
            ua + " windows");
    t.check(matches(at(expected, "operating_system"), os, text), ua + " operating_system");
    t.check(matches(at(expected, "browser"), browser, text), ua + " browser");
    // The view answers false, or "", where Ruby raises.
    const views::Platform view = platform.to_view();
    const auto view_flag = [&](std::string_view name, bool actual) {
      const json::Value& e = at(expected, name);
      t.check((e.is_bool() && e.as_bool()) == actual, ua + " view " + std::string(name));
    };
    view_flag("windows", view.windows);
    view_flag("chrome", view.chrome);
    view_flag("firefox", view.firefox);
    view_flag("safari", view.safari);
    view_flag("edge", view.edge);
    t.check(view.browser == (at(expected, "browser").is_string() ? at(expected, "browser").as_string() : std::string{}),
            ua + " view browser");
    t.check(view.operating_system == (at(expected, "operating_system").is_string()
                                          ? at(expected, "operating_system").as_string()
                                          : std::string{}),
            ua + " view os");
    // `blocked`
    const json::Value& blocked = at(c, "blocked");
    t.check(raised(blocked) ? true : blocked == json::Value(platform.browser_blocked()), ua + " blocked");
  }
  std::printf("VECTORS campfire_user_agents.json user_agents.platform cases=%d pass=%d\n", t.total, t.pass);
  std::string detail;
  for (const auto& f : t.failures) detail += "\n  " + f;
  CHECK_MESSAGE(t.pass == t.total, (t.total - t.pass) << " failures" << detail);
}

TEST_CASE("user agent vectors: versions and comparisons") {
  Tally t;
  for (const json::Value& c : vectors().find("versions")->as_array()) {
    const ua::Version v(at(c, "string").as_string());
    json::Value::Array to_a;
    for (const ua::Segment& s : v.to_a()) to_a.emplace_back((s.is_int ? "i:" : "s:") + s.text);
    t.check(json::Value(v.is_nil()) == at(c, "nil") && json::Value(to_a) == at(c, "to_a"), at(c, "string").as_string());
  }
  for (const json::Value& c : vectors().find("comparisons")->as_array()) {
    const ua::Version a(at(c, "a").as_string());
    const ua::Version b(at(c, "b").as_string());
    const int cmp = a.compare(b);
    t.check(
        json::Value(cmp) == at(c, "cmp") && json::Value(a.less(b)) == at(c, "lt") && json::Value(a == b) == at(c, "eq"),
        at(c, "a").as_string() + " <=> " + at(c, "b").as_string());
  }
  std::printf("VECTORS campfire_user_agents.json versions+comparisons cases=%d pass=%d\n", t.total, t.pass);
  std::string detail;
  for (const auto& f : t.failures) detail += "\n  " + f;
  CHECK_MESSAGE(t.pass == t.total, (t.total - t.pass) << " failures" << detail);
}

TEST_CASE("blank user agents parse as the default") {
  const ua::Agent agent = ua::parse("  ");
  CHECK(agent.browser() == "Mozilla");
  CHECK(agent.version().str() == "4.0");
  CHECK(ApplicationPlatform("curl/8.4.0").to_view().operating_system.empty());
}

}  // namespace campfire::app
