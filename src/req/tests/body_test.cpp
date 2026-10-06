// Tests of whole request bodies. The cases come from the tests of crates/kit/src/body.rs.
#include "req/body.hpp"

#include <doctest.h>

#include <filesystem>
#include <memory_resource>

#include "compat/json.hpp"

using namespace campfire::req;
namespace json = campfire::compat::json;

namespace {
std::pmr::memory_resource* mr() {
  return std::pmr::get_default_resource();
}
json::Value J(std::string_view text) {
  return *json::parse(text);
}
const std::filesystem::path kTmp = std::filesystem::temp_directory_path();
}  // namespace

TEST_CASE("body: urlencoded form, JSON, raw text") {
  auto form = parse_body("POST", "application/x-www-form-urlencoded", "a[b]=1&c=2", kTmp, mr());
  REQUIRE(form.has_value());
  CHECK(form->params->to_json() == J(R"({"a":{"b":"1"},"c":"2"})"));
  CHECK(form->raw == "a[b]=1&c=2");

  auto js = parse_body("POST", "application/json", R"({"url":"x"})", kTmp, mr());
  REQUIRE(js.has_value());
  CHECK(js->params->to_json() == J(R"({"url":"x"})"));

  auto text = parse_body("POST", "text/plain", "Hello!", kTmp, mr());
  REQUIRE(text.has_value());
  CHECK(text->params->empty());
  CHECK(text->raw == "Hello!");
}

TEST_CASE("body: a POST with no content type is a form") {
  auto post = parse_body("POST", std::nullopt, "Hello!", kTmp, mr());
  REQUIRE(post.has_value());
  CHECK(post->params->to_json() == J(R"({"Hello!":null})"));
  auto put = parse_body("PUT", std::nullopt, "Hello!", kTmp, mr());
  REQUIRE(put.has_value());
  CHECK(put->params->empty());
}

TEST_CASE("body: a body over the limit") {
  CHECK_FALSE(parse_body("POST", std::nullopt, std::string(20, 'x'), kTmp, mr(), 10).has_value());
  CHECK_FALSE(parse_body("POST", "multipart/form-data; boundary=B",
                         "--B\r\nContent-Disposition: form-data; name=\"f\"; "
                         "filename=\"x\"\r\n\r\nxxxxxxxxxxxxxxxxxxxxxx\r\n--B--\r\n",
                         kTmp, mr(), 50)
                  .has_value());
}

TEST_CASE("body: multipart") {
  auto r = parse_body("POST", "multipart/form-data; boundary=B",
                      "--B\r\nContent-Disposition: form-data; name=\"a\"\r\n\r\n1\r\n--B--\r\n", kTmp, mr());
  REQUIRE(r.has_value());
  CHECK(r->params->to_json() == J(R"({"a":"1"})"));
  CHECK(r->raw.empty());
}

TEST_CASE("body: malformed JSON is a parse error, not a size error") {
  auto r = parse_body("POST", "application/json", "{bad", kTmp, mr());
  REQUIRE(r.has_value());
  REQUIRE_FALSE(r->params.has_value());
  CHECK(r->params.error().code == ParamErrc::Parse);
}
