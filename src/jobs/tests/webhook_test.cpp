// Tests of the webhook reply rules. Rust: crates/campfire/src/integrations/webhook.rs (tests).
#include "jobs/webhook.hpp"

#include <doctest.h>

using namespace campfire::jobs::webhook;

TEST_CASE("looks up MIME types like Rails") {
  const auto expect = [](std::string_view in, std::string_view symbol, std::string_view type) {
    auto found = mime_lookup(in);
    REQUIRE(found.has_value());
    CHECK(found->symbol == symbol);
    CHECK(found->content_type == type);
  };
  expect("image/jpeg", "jpeg", "image/jpeg");
  expect("application/x-gzip", "gzip", "application/gzip");
  expect("IMAGE/PNG", "", "IMAGE/PNG");
  expect("text/html; charset=utf-8", "html", "text/html");
  expect("video/quicktime", "", "video/quicktime");
  expect("audio/mp4", "m4a", "audio/aac");
  for (const char* invalid : {"image", "", "text/html, text", "a/b c"}) CHECK_FALSE(mime_lookup(invalid).has_value());
}

TEST_CASE("a reply is a text, an attachment or nothing") {
  auto text = reply_from(200, "text/plain", "caf\xE9");
  REQUIRE(text.has_value());
  CHECK(text->kind == Reply::Kind::Text);
  CHECK(text->text == "caf\xEF\xBF\xBD");

  CHECK(reply_from(200, std::nullopt, "x")->kind == Reply::Kind::None);

  auto error_html = reply_from(500, "text/html", "Internal Error!");
  REQUIRE(error_html.has_value());
  CHECK(error_html->kind == Reply::Kind::Attachment);
  CHECK(error_html->attachment.filename == "attachment.html");
  CHECK(error_html->attachment.content_type == "text/html");

  auto mixed = reply_from(200, "Text/Plain", "not text");
  REQUIRE(mixed.has_value());
  CHECK(mixed->attachment.filename == "attachment.");
  CHECK(mixed->attachment.content_type == "Text/Plain");

  CHECK_FALSE(reply_from(200, "image", "x").has_value());
  CHECK_FALSE(reply_from(200, "", "x").has_value());
}

TEST_CASE("a timeout is a text reply") {
  CHECK(timed_out(std::chrono::seconds(7)).text == "Failed to respond within 7 seconds");
}
