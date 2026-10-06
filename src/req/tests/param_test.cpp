// Tests of query and form params. data/params_vectors.json is the output of
// ActionDispatch::ParamBuilder.from_query_string in the Rails reference container.
#include <doctest.h>

#include <fstream>
#include <memory_resource>
#include <sstream>
#include <string>

#include "compat/json.hpp"
#include "req/query.hpp"

using namespace campfire::req;
namespace json = campfire::compat::json;

namespace {

std::pmr::memory_resource* mr() {
  return std::pmr::get_default_resource();
}

json::Value parse(std::string_view qs) {
  auto r = from_query_string(qs, mr());
  REQUIRE(r.has_value());
  return r->to_json();
}

std::optional<ParamErrc> error_of(std::string_view qs) {
  auto r = from_query_string(qs, mr());
  if (r) return std::nullopt;
  return r.error().code;
}

json::Value J(std::string_view text) {
  auto v = json::parse(text);
  REQUIRE(v.has_value());
  return *v;
}

}  // namespace

TEST_CASE("params: vectors of Rails ParamBuilder.from_query_string") {
  std::ifstream in(std::string(CAMPFIRE_PARAMS_VECTORS), std::ios::binary);
  REQUIRE(in.good());
  std::stringstream buffer;
  buffer << in.rdbuf();
  auto vectors = json::parse(buffer.str());
  REQUIRE(vectors.has_value());
  std::size_t failures = 0;
  for (const auto& v : vectors->as_array()) {
    const std::string& input = v.find("input")->as_string();
    auto r = from_query_string(input, mr());
    const json::Value actual = r ? r->to_json() : json::Value();
    if (!(actual == *v.find("output"))) {
      ++failures;
      if (failures < 5) MESSAGE("differs: " << input);
    }
  }
  CHECK(failures == 0);
  CHECK(vectors->as_array().size() == 2755);
}

TEST_CASE("params: pair splitting") {
  CHECK(parse("&&a=1&") == J(R"({"a":"1"})"));
  CHECK(parse("a=1&  b=2") == J(R"({"a":"1","b":"2"})"));
  CHECK(parse("=1") == J("{}"));
  CHECK(parse("a=b=c") == J(R"({"a":"b=c"})"));
  CHECK(parse("a;b=1") == J(R"({"a;b":"1"})"));
}

TEST_CASE("params: decoding") {
  CHECK(parse("a=x+y%20z") == J(R"({"a":"x y z"})"));
  CHECK(parse("caf%C3%A9=%E2%9C%93") == J(R"({"café":"✓"})"));
  CHECK(error_of("a=%") == ParamErrc::Invalid);
  CHECK(error_of("a=%zz") == ParamErrc::Invalid);
  CHECK(error_of("a=%FF") == ParamErrc::Invalid);
  CHECK(parse("=%FF") == J("{}"));
}

TEST_CASE("params: nested hashes and arrays") {
  CHECK(parse("a[b][c]=1&a[b][d]=2") == J(R"({"a":{"b":{"c":"1","d":"2"}}})"));
  CHECK(parse("[a]=1") == J(R"({"[a]":"1"})"));
  CHECK(parse("a[]=1&a[]=2") == J(R"({"a":["1","2"]})"));
  CHECK(parse("a[]") == J(R"({"a":[]})"));
  CHECK(parse("a[][b]=1&a[][c]=2&a[][b]=3") == J(R"({"a":[{"b":"1","c":"2"},{"b":"3"}]})"));
  CHECK(parse("a&a[]=1") == J(R"({"a":["1"]})"));
}

TEST_CASE("params: type conflicts and depth") {
  CHECK(error_of("a=1&a[]=2") == ParamErrc::Type);
  CHECK(error_of("a[b]=1&a[]=2") == ParamErrc::Type);
  std::string deep = "a";
  for (std::size_t i = 0; i < kDepthLimit - 1; ++i) deep += "[b]";
  CHECK_FALSE(error_of(deep + "=1").has_value());
  CHECK(error_of(deep + "[b]=1") == ParamErrc::TooDeep);
}

TEST_CASE("params: form body limits") {
  auto ok = from_form_body(std::string_view("a=1\0", 4), mr());
  REQUIRE(ok.has_value());
  CHECK(ok->to_json() == J(R"({"a":"1"})"));
  std::string many;
  for (std::size_t i = 0; i <= kFormParamsLimit; ++i) many += i == 0 ? "a=1" : "&a=1";
  auto r = from_form_body(many, mr());
  REQUIRE_FALSE(r.has_value());
  CHECK(r.error().code == ParamErrc::Limit);
}

TEST_CASE("params: JSON bodies are deep munged") {
  auto p = from_json_body(R"({"a": [1, null, "x"], "b": {"c": null}, "d": true})", mr());
  REQUIRE(p.has_value());
  CHECK(p->to_json() == J(R"({"a":[1,"x"],"b":{"c":null},"d":true})"));
  auto arr = from_json_body("[1,2]", mr());
  REQUIRE(arr.has_value());
  CHECK(arr->to_json() == J(R"({"_json":[1,2]})"));
  auto bad = from_json_body("{bad", mr());
  REQUIRE_FALSE(bad.has_value());
  CHECK(bad.error().code == ParamErrc::Parse);
}

TEST_CASE("params: require and permit") {
  auto params = from_query_string(
      "user[name]=Jo&user[admin]=1&user[tags][]=a&user[settings][x][y]=1&user[bad][]=1&blank=+&user[date(1i)]=2024",
      mr());
  REQUIRE(params.has_value());
  CHECK_FALSE(params->require("blank").has_value());
  CHECK_FALSE(params->require("missing").has_value());
  auto user = params->require("user");
  REQUIRE(user.has_value());
  const ParamMap* hash = (*user)->as_hash();
  REQUIRE(hash != nullptr);
  auto permitted = hash->permit({"name", "date", Permit::scalar_array("tags"), Permit::any_hash("settings"), "bad"});
  CHECK(permitted.to_json() == J(R"j({"name":"Jo","date(1i)":"2024","tags":["a"],"settings":{"x":{"y":"1"}}})j"));
}

TEST_CASE("params: permit nested") {
  auto params = from_query_string("a[b][c]=1&a[b][d]=2&list[][c]=1&list[][d]=2&ff[0][c]=1&ff[1][c]=2", mr());
  REQUIRE(params.has_value());
  const std::vector<Permit> c = {"c"};
  auto permitted =
      params->permit({Permit::nest("a", {Permit::nest("b", c)}), Permit::nest("list", c), Permit::nest("ff", c)});
  CHECK(permitted.to_json() == J(R"({"a":{"b":{"c":"1"}},"list":[{"c":"1"}],"ff":{"0":{"c":"1"},"1":{"c":"2"}}})"));
}

TEST_CASE("params: many keys build in linear time") {
  std::string qs;
  for (int i = 0; i < 100000; ++i) qs += (i ? "&k" : "k") + std::to_string(i) + "=1";
  auto r = from_query_string(qs, mr());
  REQUIRE(r.has_value());
  CHECK(r->size() == 100000);
}
