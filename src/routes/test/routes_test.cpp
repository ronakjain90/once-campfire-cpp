// Tests of the path helpers against the reference app (Rails) and the vectors.
//   spec/vectors/campfire_routes.json   routes and recognitions of config/routes.rb
//   test/named_routes.json              named routes (dump_named_routes.rb, run in campfire-reference)
//   test/path_cases.json                what the Rails *_path helpers return (dump_path_cases.rb)
#include "routes/routes.hpp"

#include <doctest.h>

#include <cstdio>
#include <fstream>
#include <functional>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "compat/json.hpp"
#include "compat/test/vectors.hpp"
#include "routes/query.hpp"

namespace routes = campfire::routes;
namespace json = campfire::compat::json;
using testing_support::at;
using testing_support::Group;
using testing_support::items;

namespace {

json::Value load_local(const char* file) {
  const std::string path = std::string(CAMPFIRE_ROUTES_TEST_DIR) + "/" + file;
  std::ifstream in(path, std::ios::binary);
  REQUIRE_MESSAGE(in.good(), "cannot open " << path);
  std::stringstream buffer;
  buffer << in.rdbuf();
  auto parsed = json::parse(buffer.str());
  REQUIRE_MESSAGE(parsed.has_value(), "invalid JSON in " << path);
  return std::move(*parsed);
}

using Args = std::vector<std::string>;
using Dispatch = std::function<std::string(const Args&)>;

template <class F, std::size_t... I>
std::string call_with(const F& f, const Args& args, std::index_sequence<I...> /*seq*/) {
  return f(args[I]...);
}

// Name -> a function that calls the helper with `count` string arguments.
const std::map<std::string, std::pair<std::size_t, Dispatch>>& dispatch_table() {
  static const auto table = [] {
    std::map<std::string, std::pair<std::size_t, Dispatch>> t;
#define CF_ROUTE(name, pattern, endpoint)                                                     \
  {                                                                                           \
    constexpr std::size_t kCount = routes::detail::Pattern<pattern>::kParams;                 \
    t[#name] = {kCount, [](const Args& args) {                                                \
                  return call_with([](const auto&... a) { return routes::name(a...); }, args, \
                                   std::make_index_sequence<kCount>{});                       \
                }};                                                                           \
  }
#define CF_ROUTE_ME(name, pattern, endpoint) CF_ROUTE(name, pattern, endpoint)
#include "routes/routes.def"
#undef CF_ROUTE_ME
#undef CF_ROUTE
    return t;
  }();
  return table;
}

// The pattern of a Rails route without the format part: "/rooms/:id(.:format)" -> "/rooms/:id".
std::string without_format(std::string path) {
  const std::string suffix = "(.:format)";
  if (path.size() >= suffix.size() && path.compare(path.size() - suffix.size(), suffix.size(), suffix) == 0) {
    path.resize(path.size() - suffix.size());
  }
  return path;
}

bool infrastructure_endpoint(const std::string& endpoint) {
  for (const char* prefix : {"action_mailbox/", "rails/conductor", "active_storage/", "turbo/"}) {
    if (endpoint.starts_with(prefix)) {
      return true;
    }
  }
  return endpoint.starts_with("rails/") && endpoint != "rails/health#show";
}

}  // namespace

TEST_CASE("routes: the route list equals the named routes of the reference app") {
  const json::Value rails = load_local("named_routes.json");
  Group group("named_routes.json", "name, pattern and endpoint");
  std::set<std::string> rails_names;
  std::map<std::string, std::pair<std::string_view, std::string_view>> ours;
  for (const auto& r : routes::kNamedRoutes) {
    ours[std::string(r.name)] = {r.pattern, r.endpoint};
  }
  for (const auto& r : items(rails)) {
    const std::string& name = at(r, "name").as_string();
    rails_names.insert(name);
    auto it = ours.find(name);
    group.check(it != ours.end() && it->second.first == at(r, "path").as_string() &&
                    it->second.second == at(r, "endpoint").as_string(),
                name);
  }
  group.finish();
  CHECK_EQ(rails_names.size(), std::size(routes::kNamedRoutes));  // nothing extra on our side
  CHECK_EQ(rails_names.size(), ours.size());
}

TEST_CASE("routes: every named route is in campfire_routes.json, and every route has a helper") {
  const auto& vectors = testing_support::load_vectors("campfire_routes.json");
  std::map<std::string, std::set<std::string>> by_path;  // path without format -> endpoints
  for (const auto& r : items(at(vectors, "routes"))) {
    by_path[without_format(at(r, "path").as_string())].insert(at(r, "endpoint").as_string());
  }
  Group named("campfire_routes.json", "helper pattern is a route");
  std::set<std::string> helper_paths;
  for (const auto& r : routes::kNamedRoutes) {
    helper_paths.emplace(r.pattern);
    auto it = by_path.find(std::string(r.pattern));
    named.check(it != by_path.end() && it->second.contains(std::string(r.endpoint)), std::string(r.name));
  }
  named.finish();

  Group covered("campfire_routes.json", "route path has a helper");
  for (const auto& r : items(at(vectors, "routes"))) {
    if (infrastructure_endpoint(at(r, "endpoint").as_string())) {
      continue;  // engines of Rails (Active Storage, Action Mailbox, Turbo native): not in routes.rb
    }
    const std::string path = without_format(at(r, "path").as_string());
    covered.check(helper_paths.contains(path), at(r, "verb").as_string() + " " + path);
  }
  covered.finish();
}

TEST_CASE("routes: helpers equal the Rails *_path helpers for tricky arguments") {
  const json::Value cases = load_local("path_cases.json");
  Group group("path_cases.json", "*_path helpers");
  for (const auto& c : items(at(cases, "cases"))) {
    const std::string& name = at(c, "name").as_string();
    Args args;
    for (const auto& a : items(at(c, "args"))) {
      args.push_back(a.as_string());
    }
    const auto it = dispatch_table().find(name);
    REQUIRE_MESSAGE(it != dispatch_table().end(), name);
    REQUIRE_EQ(it->second.first, args.size());
    group.check(it->second.second(args) == at(c, "path").as_string(), name + " " + at(c, "path").as_string());
  }
  group.finish();
}

TEST_CASE("routes: a route with the default user_id leaves it out") {
  CHECK_EQ(routes::user_sidebar(), "/users/me/sidebar");
  CHECK_EQ(routes::user_sidebar(7), "/users/7/sidebar");
  CHECK_EQ(routes::user_profile(), "/users/me/profile");
  CHECK_EQ(routes::edit_user_profile(), "/users/me/profile/edit");
  CHECK_EQ(routes::user_push_subscriptions(), "/users/me/push_subscriptions");
  CHECK_EQ(routes::user_push_subscription(4), "/users/me/push_subscriptions/4");
  CHECK_EQ(routes::user_push_subscription_test_notifications(4), "/users/me/push_subscriptions/4/test_notifications");
  CHECK_EQ(routes::user_push_subscription_test_notifications("me", 4),
           "/users/me/push_subscriptions/4/test_notifications");
}

TEST_CASE("routes: simple helpers") {
  CHECK_EQ(routes::root(), "/");
  CHECK_EQ(routes::room_at_message(1, 42), "/rooms/1/@42");
  CHECK_EQ(routes::room_bot_message_boost(1, "1-abc", 7, 3), "/rooms/1/1-abc/messages/7/boosts/3");
  CHECK_EQ(routes::rails_health_check(), "/up");
  CHECK_EQ(routes::user(std::int64_t{-5}), "/users/-5");
  CHECK_EQ(routes::user(std::uint64_t{18446744073709551615ULL}), "/users/18446744073709551615");
}

TEST_CASE("routes: direct routes fresh_account_logo and fresh_user_avatar") {
  const json::Value cases = load_local("path_cases.json");
  Group logo("path_cases.json", "fresh_account_logo");
  for (const auto& c : items(at(cases, "logo"))) {
    std::optional<std::string_view> v;
    std::optional<std::string_view> size;
    if (const auto* s = at(c, "v").get_string()) v = *s;
    if (const auto* s = at(c, "size").get_string()) size = *s;
    logo.check(routes::fresh_account_logo(v, size) == at(c, "path").as_string(), at(c, "path").as_string());
  }
  logo.finish();
  Group avatar("path_cases.json", "fresh_user_avatar");
  for (const auto& c : items(at(cases, "avatars"))) {
    avatar.check(
        routes::fresh_user_avatar(at(c, "token").as_string(), at(c, "v").as_string()) == at(c, "path").as_string(),
        at(c, "path").as_string());
  }
  avatar.finish();
  CHECK_EQ(routes::fresh_account_logo(), "/account/logo");
}

TEST_CASE("routes: query strings") {
  const json::Value cases = load_local("path_cases.json");
  const std::int64_t ids[] = {5, 6, 70};
  CHECK_EQ(routes::rooms_directs_with_users(ids), at(at(cases, "directs"), "rooms_directs_with_users").as_string());
  CHECK_EQ(routes::with_query("/x", {{"z", std::string("a b")}, {"a", std::string("1")}}), "/x?a=1&z=a+b");
  CHECK_EQ(routes::with_query("/x", {}), "/x");
  CHECK_EQ(routes::with_query("/x", {{"k", std::vector<std::string>{}}}), "/x");
}
