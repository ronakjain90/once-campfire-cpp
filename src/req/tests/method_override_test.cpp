// Tests of Rack::MethodOverride. The cases come from crates/kit/tests/http.rs (method_override_*).
#include "req/method_override.hpp"

#include <doctest.h>

#include <optional>
#include <string>
#include <string_view>

using campfire::req::method_override;

namespace {

constexpr std::string_view kForm = "application/x-www-form-urlencoded";

std::optional<std::string_view> form(std::string_view body) {
  return method_override(kForm, body, std::nullopt);
}

std::string multipart_body(std::string_view method_part) {
  std::string part;
  if (!method_part.empty()) part = "--b0undary\r\n" + std::string(method_part);
  return "--b0undary\r\nContent-Disposition: form-data; name=\"message[body]\"\r\n\r\n<p>a method</p>\r\n" + part +
         "--b0undary\r\nContent-Disposition: form-data; name=\"file\"; filename=\"a.txt\"\r\n"
         "Content-Type: text/plain\r\n\r\n_method\r\n"
         "--b0undary--\r\n";
}

constexpr std::string_view kMultipart = "multipart/form-data; boundary=b0undary";

}  // namespace

TEST_CASE("an urlencoded _method overrides the method") {
  CHECK(form("_method=patch&x=1") == "PATCH");
  CHECK(form("_method=delete") == "DELETE");
  CHECK(form("x=1&_method=Put") == "PUT");
  CHECK(form("%5Fmethod=patch") == "PATCH");
  // `req.POST` keeps the last value of a key.
  CHECK(form("_method=put&_method=patch") == "PATCH");
}

TEST_CASE("a POST with no content type is a form") {
  CHECK(method_override(std::nullopt, "_method=patch", std::nullopt) == "PATCH");
  CHECK(method_override("", "_method=patch", std::nullopt) == "PATCH");
}

TEST_CASE("an unknown method gives no override") {
  CHECK(form("_method=bogus") == std::nullopt);
  CHECK(form("_method=") == std::nullopt);
  // A `_method` param hides the header, as in Rack.
  CHECK(method_override(kForm, "_method=bogus", "PATCH") == std::nullopt);
}

TEST_CASE("LINK and UNLINK are methods of Rack::MethodOverride") {
  CHECK(form("_method=link") == "LINK");
  CHECK(form("_method=unlink") == "UNLINK");
}

TEST_CASE("the X-HTTP-Method-Override header overrides the method") {
  CHECK(method_override(kForm, "", "patch") == "PATCH");
  CHECK(method_override(kForm, "x=1", "DELETE") == "DELETE");
  CHECK(method_override(kForm, "x=1", "bogus") == std::nullopt);
  CHECK(method_override("application/json", "{}", "patch") == "PATCH");
}

TEST_CASE("a JSON body is not a form") {
  CHECK(method_override("application/json", R"({"_method":"patch"})", std::nullopt) == std::nullopt);
}

TEST_CASE("a multipart _method overrides the method") {
  const std::string body = multipart_body("Content-Disposition: form-data; name=\"_method\"\r\n\r\npatch\r\n");
  CHECK(method_override(kMultipart, body, std::nullopt) == "PATCH");
  CHECK(method_override("multipart/mixed; boundary=b0undary", body, std::nullopt) == "PATCH");
  CHECK(method_override("multipart/form-data; boundary=\"b0undary\"", body, std::nullopt) == "PATCH");
}

TEST_CASE("a multipart file part named _method is not the param") {
  const std::string body = multipart_body(
      "Content-Disposition: form-data; name=\"_method\"; filename=\"m.txt\"\r\nContent-Type: text/plain\r\n\r\n"
      "patch\r\n");
  CHECK(method_override(kMultipart, body, std::nullopt) == std::nullopt);
}

TEST_CASE("a multipart body with no _method part gives no override") {
  CHECK(method_override(kMultipart, multipart_body(""), std::nullopt) == std::nullopt);
  CHECK(method_override(kMultipart, multipart_body(""), "delete") == "DELETE");
}

TEST_CASE("a broken body gives no _method") {
  CHECK(form("_method=%zz") == std::nullopt);
  CHECK(method_override("multipart/form-data", "--x\r\n_method", std::nullopt) == std::nullopt);
  CHECK(method_override(kMultipart, "--b0undary\r\nContent-Disposition: form-data; name=\"_method\"\r\n\r\npatch",
                        std::nullopt) == std::nullopt);
}
