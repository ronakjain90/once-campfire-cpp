// Tests of the sanitizer. Rust: crates/richtext/src/sanitizer.rs (tests), tests/reference_tests.rs, tests/hardening.rs
#include "richtext/sanitizer.hpp"

#include <string>

#include "doctest.h"
#include "richtext/filters.hpp"

using namespace campfire::richtext;

namespace {

std::string clean(std::string_view html, const SafeList& list) {
  auto result = sanitize(html, list);
  REQUIRE(result.has_value());
  return *result;
}

std::string filtered(std::string_view body) {
  auto result = filter_message_html(body);
  REQUIRE(result.has_value());
  return *result;
}

bool contains(const std::string& haystack, std::string_view needle) {
  return haystack.find(needle) != std::string::npos;
}

}  // namespace

TEST_CASE("scrubs like Rails") {
  const SafeList& list = SafeList::content_filter();
  CHECK(clean("<div><a href=\"javascript:alert(1)\">x</a></div>", list) == "<div><a>x</a></div>");
  CHECK(clean("<a href=\"/x\" onmouseover=\"alert(1)\">x</a>", list) == "<a href=\"/x\">x</a>");
  CHECK(clean("<a href=\"data:text/html,pwned\">x</a>", list) == "<a>x</a>");
  CHECK(clean("<a href=\"a b\">x</a><!-- c -->", list) == "<a href=\"a%20b\">x</a>");
  CHECK(clean("<svg><a>x</a></svg>y<script>z</script>", list) == "yz");
  CHECK(clean("", list).empty());
}

TEST_CASE("re-escapes URL attributes as each attribute is scrubbed") {
  SafeList list;
  list.tags.add({"img", "a"});
  list.attributes.add({"src", "href", "name", "title", "alt"});
  // The blank src is removed, and the escaping it still triggers lets the href through.
  CHECK(clean("<img src=\" \" href=\" javascript:alert(1)\">", list) == "<img href=\"%20javascript:alert(1)\">");
  CHECK(clean("<img href=\" javascript:alert(1)\">", list) == "<img>");
  CHECK(clean("<a href=\"a b\" name=\"c d\" title=\"x y\">t</a>", list) ==
        "<a href=\"a%20b\" name=\"c%20d\" title=\"x y\">t</a>");
  CHECK(clean("<img src=\"a b\" alt=\"q\">", list) == "<img src=\"a%20b\" alt=\"q\">");
}

TEST_CASE("keeps only Lexxy's highlight colors in style") {
  const SafeList& list = SafeList::action_text();
  const std::string highlight =
      "<mark style=\"color: var(--highlight-1);background-color: var(--highlight-bg-2);\">x</mark>";
  CHECK(clean(highlight, list) == highlight);
  CHECK(clean("<span style=\"color: #f00; position: fixed; top: 0\">x</span>", list) ==
        "<span style=\"color: #f00;\">x</span>");
  const std::string rgb = "<span style=\"COLOR: rgb(1 2 3 / 50%)\">x</span>";
  CHECK(clean(rgb, list) == rgb);
  for (std::string_view hostile : {"background-color: url(https://evil.test/beacon)", "color: expression(alert(1))",
                                   "background-color: red; background-image: url(x)", "color: \\72 ed",
                                   "color: red /* */", "width: 100000px", ""}) {
    std::string html = clean("<span style=\"" + std::string(hostile) + "\">x</span>", list);
    const bool safe =
        !contains(html, "url") && !contains(html, "expression") && !contains(html, "\\") && !contains(html, "width");
    CHECK_MESSAGE(safe, std::string(hostile), ": ", html);
  }
  CHECK(clean("<span style=\"position: fixed\">x</span>", list) == "<span>x</span>");
  // Message pages drop styles (the auto link list has no style).
  CHECK(clean("<span style=\"color: red\">x</span>", SafeList::auto_link()) == "<span>x</span>");
}

TEST_CASE("checks URIs like Loofah") {
  CHECK_FALSE(allowed_uri("javascript:alert(1)"));
  CHECK_FALSE(allowed_uri("java\nscript:alert(1)"));
  CHECK_FALSE(allowed_uri("javascript&#58;alert(1)"));
  CHECK_FALSE(allowed_uri("&#106;avascript:alert(1)"));
  CHECK_FALSE(allowed_uri("javascript&colon;alert(1)"));
  CHECK(allowed_uri("/rooms/1"));
  CHECK(allowed_uri("https://example.com"));
  CHECK(allowed_uri("data:image/png;base64,xx"));
  CHECK_FALSE(allowed_uri("data:text/html,xx"));
  // CGI.unescapeHTML turns the zero-padded `&` into `&#106;`, which Loofah then decodes.
  CHECK(clean("<a href=\"&amp;#0000000000038;#106;avascript:alert(1)\">x</a>", SafeList::content_filter()) ==
        "<a>x</a>");
}

TEST_CASE("unescapes HTML like CGI") {
  struct Case {
    std::string_view escaped;
    std::string_view unescaped;
  };
  const Case cases[] = {
      {"&#65;", "A"},
      {"&#X41;", "A"},
      {"&#00000000065;", "A"},
      {"&#0000000000000000000000065;", "A"},
      {"&#x000000041;", "A"},
      {"&#x0000000000000000000041;", "A"},
      {"&#00000000000000000000000000000000106;avascript", "javascript"},
      {"&#65535;", "\xEF\xBF\xBF"},
      {"&#1114110;", "\xF4\x8F\xBF\xBE"},
      {"&#1114111;", "&#1114111;"},
      {"&#x10FFFF;", "&#x10FFFF;"},
      {"&#x110000;", "&#x110000;"},
      {"&#99999999999999999999;", "&#99999999999999999999;"},
      {"&#18446744073709551615;", "&#18446744073709551615;"},
      {"&#x10000000000000041;", "&#x10000000000000041;"},
      {"&#;", "&#;"},
      {"&#x;", "&#x;"},
      {"&#65", "&#65"},
      {"&#0x41;", "&#0x41;"},
      {"&amp;#65;", "&#65;"},
  };
  for (const Case& c : cases) {
    CHECK_MESSAGE(cgi_unescape_html(c.escaped) == c.unescaped, c.escaped);
  }
  CHECK(cgi_unescape_html("&#0;") == std::string("\0", 1));
}

// Rust: tests/reference_tests.rs and tests/hardening.rs, the cases that need no attachments.
TEST_CASE("message contains a forbidden tag") {
  CHECK(filtered("Hello <img src=\"https://ssecurityrise.com/tests/billionlaughs-cache.svg\">World") == "Hello World");
}

TEST_CASE("message with a link using an unsafe URI scheme") {
  std::string result = filtered("<div><a href=\"javascript:alert(1)\">x</a></div>");
  CHECK_FALSE(contains(result, "javascript:"));
  CHECK(contains(result, "<a>x</a>"));
}

TEST_CASE("message with an event handler attribute on an allowed tag") {
  std::string result =
      filtered("<div><a href=\"/x\" onmouseover=\"alert(1)\">x</a> <span onclick=\"alert(2)\">y</span></div>");
  CHECK_FALSE(contains(result, "onmouseover"));
  CHECK_FALSE(contains(result, "onclick"));
  CHECK(contains(result, "<a href=\"/x\">x</a>"));
  CHECK(contains(result, "<span>y</span>"));
}

TEST_CASE("message with a data URI link") {
  std::string result = filtered("<div><a href=\"data:text/html,pwned\">x</a></div>");
  CHECK_FALSE(contains(result, "data:"));
  CHECK(contains(result, "<a>x</a>"));
}

TEST_CASE("message with a safe link and formatting is preserved") {
  std::string result = filtered(
      "<div><a href=\"https://example.com\">example</a> <strong>bold</strong> <code>code</code>"
      "<ul><li>one</li><li>two</li></ul></div>");
  CHECK(contains(result, "<a href=\"https://example.com\">example</a>"));
  CHECK(contains(result, "<strong>bold</strong>"));
  CHECK(contains(result, "<code>code</code>"));
  CHECK(contains(result, "<ul><li>one</li><li>two</li></ul>"));
}

TEST_CASE("message keeps strikethrough, underline, mark and code block formatting") {
  const std::string body =
      "<p>Hello <s>struck</s> <u>under</u> <mark>marked</mark></p><pre data-language=\"ruby\">def x<br>end</pre>";
  CHECK(filtered(body) == body);
}

TEST_CASE("sanitize attributes neutralizes unsafe input and preserves benign content") {
  std::string result = filtered(
      "<div><a href=\"javascript:alert(1)\" onclick=\"x()\">link</a> <a href=\"data:text/html,pwned\">data</a> "
      "<span class=\"cf-twitter-avatar\" onmouseover=\"y()\">avatar</span> <img src=\"https://evil.example/x.svg\"> "
      "Hey</div>");
  for (std::string_view forbidden : {"javascript:", "data:text/html", "onclick", "onmouseover", "evil.example"}) {
    CHECK_MESSAGE(!contains(result, forbidden), forbidden, " in ", result);
  }
  CHECK(contains(result, "<span class=\"cf-twitter-avatar\">avatar</span>"));
  CHECK(contains(result, ">link<"));
}

TEST_CASE("message with formatting saved under Trix renders unchanged") {
  const std::string body =
      "<div>Hello <strong>bold</strong> <em>it</em> <del>gone</del> <a "
      "href=\"https://example.com/\">link</a><br>second line</div>"
      "<h1>Heading</h1><blockquote>quoted</blockquote><pre>line 1\nline "
      "2</pre><ul><li>one</li></ul><ol><li>first</li></ol>";
  CHECK(filtered(body) == body);
}

TEST_CASE("message with a table keeps the table") {
  const std::string body =
      "<figure "
      "class=\"lexxy-content__table-wrapper\"><table><tbody><tr><th><p>Name</p></th></tr><tr><td><p>Jason</p></td></"
      "tr></tbody></table></figure>";
  CHECK(filtered(body) == body);
}

TEST_CASE("name attributes cannot clobber the page's globals") {
  CHECK(filtered("<p><a name=\"body\" href=\"/x\">x</a><span name=\"cookie\">y</span></p>") ==
        "<p><a href=\"/x\">x</a><span>y</span></p>");
}

TEST_CASE("bodies beyond Gumbo's limits are refused") {
  CHECK_FALSE(filter_message_html(std::string(401 * 3, ' ') + "<b>x").has_value() == false);
  std::string deep;
  for (int i = 0; i < 401; ++i) {
    deep += "<b>";
  }
  CHECK(filter_message_html(deep).error() == ParseError::TreeTooDeep);
}

TEST_CASE("Ruby strip removes NUL and ASCII whitespace at both ends") {
  CHECK(ruby_strip(std::string_view("\0 \t\v x\0 \f\n", 10)) == "x");
  CHECK(ruby_strip("\xC2\xA0x\xC2\xA0") == "\xC2\xA0x\xC2\xA0");
}
