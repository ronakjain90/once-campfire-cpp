// Tests of multipart parsing. The cases come from the tests of crates/kit/src/body.rs.
#include <doctest.h>

#include <filesystem>
#include <memory_resource>
#include <string>
#include <vector>

#include "compat/json.hpp"
#include "req/body.hpp"
#include "req/multipart.hpp"

using namespace campfire::req;
namespace json = campfire::compat::json;

namespace {

std::pmr::memory_resource* mr() { return std::pmr::get_default_resource(); }

std::filesystem::path tmp() {
  auto dir = std::filesystem::temp_directory_path() / "cf_req_test";
  std::filesystem::create_directories(dir);
  return dir;
}

std::size_t files_in(const std::filesystem::path& dir) {
  std::size_t n = 0;
  for ([[maybe_unused]] const auto& e : std::filesystem::directory_iterator(dir)) ++n;
  return n;
}

std::string body_of(std::string_view boundary, const std::vector<std::pair<std::string, std::string>>& parts) {
  std::string body;
  for (const auto& [head, content] : parts) {
    body += "--" + std::string(boundary) + "\r\n" + head + "\r\n\r\n" + content + "\r\n";
  }
  return body + "--" + std::string(boundary) + "--\r\n";
}

json::Value J(std::string_view text) { return *json::parse(text); }


std::string field(const std::string& name) { return "Content-Disposition: form-data; name=\"" + name + "\""; }

}  // namespace

TEST_CASE("multipart: Content-Disposition") {
  auto p = parse_disposition(R"(form-data; name="message[attachment]"; filename="C:\Users\me\cat.png")");
  CHECK(p.name == "message[attachment]");
  CHECK(p.filename == "cat.png");
  p = parse_disposition(R"(form-data; name="a"; filename="with \"quotes\".txt")");
  CHECK(p.filename == "with \"quotes\".txt");
  p = parse_disposition("form-data; name=file; filename*=UTF-8''r%C3%A9sum%C3%A9.pdf");
  CHECK(p.name == "file");
  CHECK(p.filename == "r\xC3\xA9sum\xC3\xA9.pdf");
  p = parse_disposition(R"(form-data; name="a"; filename="100%.txt")");
  CHECK(p.filename == "100%.txt");
}

TEST_CASE("multipart: the boundary") {
  CHECK(parse_boundary("multipart/form-data; boundary=XyZ") == "XyZ");
  CHECK(parse_boundary("multipart/form-data; boundary=\"a b\"") == "a b");
  CHECK_FALSE(parse_boundary("multipart/form-data").has_value());
  CHECK_FALSE(parse_boundary("text/plain; boundary=x").has_value());
}

TEST_CASE("multipart: fields and files") {
  const auto dir = tmp();
  const std::string body = body_of(
      "XyZ", {{field("_method"), "patch"},
              {field("user[name]"), "Jo"},
              {"Content-Disposition: form-data; name=\"user[avatar]\"; filename=\"me.png\"\r\nContent-Type: image/png",
               "PNGDATA"},
              {"Content-Disposition: form-data; name=\"user[empty]\"; filename=\"\"", ""},
              {field("tags[]"), "a"},
              {field("tags[]"), "b"}});
  MultipartParser parser("XyZ", dir, mr());
  // Feed in small pieces to test the streaming.
  for (std::size_t i = 0; i < body.size(); i += 7) REQUIRE(parser.feed(std::string_view(body).substr(i, 7)).has_value());
  auto params = parser.finish();
  REQUIRE(params.has_value());
  CHECK(params->str("_method") == "patch");
  const Param* user = params->get("user");
  REQUIRE(user != nullptr);
  CHECK(user->get("name")->as_str() == "Jo");
  CHECK(user->get("empty") == nullptr);
  const auto* avatar = user->get("avatar")->as_file();
  REQUIRE(avatar != nullptr);
  CHECK((*avatar)->original_filename == "me.png");
  CHECK((*avatar)->content_type == "image/png");
  CHECK((*avatar)->size == 7);
  CHECK((*avatar)->read() == "PNGDATA");
  CHECK((*avatar)->headers.find("content-type: image/png\r\n") != std::string::npos);
  CHECK(params->to_json().find("tags")->as_array().size() == 2);
}

TEST_CASE("multipart: temp files go when the params go") {
  const auto dir = tmp();
  const auto before = files_in(dir);
  {
    const std::string body = body_of("B", {{"Content-Disposition: form-data; name=\"f\"; filename=\"x\"", "data"}});
    MultipartParser parser("B", dir, mr());
    REQUIRE(parser.feed(body).has_value());
    auto params = parser.finish();
    REQUIRE(params.has_value());
    CHECK(files_in(dir) == before + 1);
  }
  CHECK(files_in(dir) == before);
}

TEST_CASE("multipart: a body over the size limit") {
  const std::string body =
      body_of("B", {{"Content-Disposition: form-data; name=\"f\"; filename=\"x\"", std::string(1000, 'x')}});
  MultipartParser parser("B", tmp(), mr(), 100);
  auto r = parser.feed(body);
  REQUIRE_FALSE(r.has_value());
  CHECK(r.error().code == ParamErrc::TooLarge);
}

TEST_CASE("multipart: text fields are capped together") {
  const std::string half(kMultipartTextLimit / 2, 'x');
  const auto file_head = "Content-Disposition: form-data; name=\"f\"; filename=\"x\"";
  {
    MultipartParser parser("B", tmp(), mr());
    REQUIRE(parser.feed(body_of("B", {{field("a[]"), half}, {field("a[]"), half}, {file_head, half}})).has_value());
    CHECK(parser.finish().has_value());
  }
  {
    MultipartParser parser("B", tmp(), mr());
    auto r = parser.feed(body_of("B", {{field("a[]"), half}, {field("a[]"), half}, {field("a[]"), "x"}}));
    REQUIRE_FALSE(r.has_value());
    CHECK(r.error().code == ParamErrc::TooLarge);
  }
}

TEST_CASE("multipart: part and file limits") {
  std::vector<std::pair<std::string, std::string>> parts;
  for (std::size_t i = 0; i <= kMultipartFileLimit; ++i) {
    parts.emplace_back("Content-Disposition: form-data; name=\"f" + std::to_string(i) + "\"; filename=\"x\"", "d");
  }
  MultipartParser parser("B", tmp(), mr());
  auto r = parser.feed(body_of("B", parts));
  REQUIRE_FALSE(r.has_value());
  CHECK(r.error().code == ParamErrc::Limit);
}

TEST_CASE("multipart: a body that stops early is a parse error") {
  MultipartParser parser("B", tmp(), mr());
  REQUIRE(parser.feed("--B\r\nContent-Disposition: form-data; name=\"a\"\r\n\r\nvalue").has_value());
  auto r = parser.finish();
  REQUIRE_FALSE(r.has_value());
  CHECK(r.error().code == ParamErrc::Parse);
}

TEST_CASE("multipart: a preamble and an epilogue are ignored, a part without a name is named by its type") {
  MultipartParser parser("B", tmp(), mr());
  REQUIRE(parser.feed("junk\r\n--B\r\nContent-Type: text/plain\r\n\r\nhello\r\n--B--\r\nepilogue").has_value());
  auto r = parser.finish();
  REQUIRE(r.has_value());
  CHECK(r->to_json() == J(R"({"text/plain":["hello"]})"));
}
