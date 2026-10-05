// Tests of the DOM and the serializer. Rust: crates/richtext/src/dom.rs (tests), tests/hardening.rs
#include <chrono>
#include <string>

#include "doctest.h"
#include "richtext/dom.hpp"

using campfire::richtext::ParseError;
using campfire::richtext::parse_fragment;
using campfire::richtext::to_html;

namespace {

std::string roundtrip(std::string_view html) {
  auto dom = parse_fragment(html);
  REQUIRE(dom.has_value());
  return to_html(dom->root());
}

std::string repeat(std::string_view piece, std::size_t count) {
  std::string out;
  out.reserve(piece.size() * count);
  for (std::size_t i = 0; i < count; ++i) {
    out.append(piece);
  }
  return out;
}

std::string numbered_attributes(int from, int to) {
  std::string out;
  for (int i = from; i <= to; ++i) {
    if (!out.empty()) {
      out.push_back(' ');
    }
    out += "a" + std::to_string(i) + "=" + std::to_string(i);
  }
  return out;
}

// nullopt: the parse worked.
std::optional<ParseError> parse_error(std::string_view html) {
  auto dom = parse_fragment(html);
  if (dom) {
    return std::nullopt;
  }
  return dom.error();
}

}  // namespace

TEST_CASE("serializes like Nokogiri") {
  CHECK(roundtrip("<td>x</td>") == "x");
  CHECK(roundtrip("<p><table><tr><td>a</td></tr></table>") ==
        "<p></p><table><tbody><tr><td>a</td></tr></tbody></table>");
  CHECK(roundtrip("<a title='a<b>c' href=\"x&y\xC2\xA0z\">t&lt;\xC2\xA0>\"'</a>") ==
        "<a title=\"a<b>c\" href=\"x&amp;y&nbsp;z\">t&lt;&nbsp;&gt;\"'</a>");
  CHECK(roundtrip("<pre>\n\nx</pre>") == "<pre>\nx</pre>");
  CHECK(roundtrip("<noscript><b>x</b></noscript>") == "<noscript><b>x</b></noscript>");
  CHECK(roundtrip("<?php x ?>") == "<!--?php x ?-->");
  CHECK(roundtrip("<SVG viewBox='0 0 1 1'><CLIPPATH/></SVG>") ==
        "<svg viewBox=\"0 0 1 1\"><clipPath></clipPath></svg>");
  CHECK(roundtrip("<br><img src=a><p>x") == "<br><img src=\"a\"><p>x</p>");
  CHECK(roundtrip("<script>a<b&c</script>") == "<script>a<b&c</script>");
  CHECK(roundtrip("<a b>x</a>") == "<a b=\"\">x</a>");
}

TEST_CASE("drops only a leading byte order mark") {
  CHECK(roundtrip("\xEF\xBB\xBF\xEF\xBB\xBFx") == "\xEF\xBB\xBFx");
  CHECK(roundtrip("<script></script>\xEF\xBB\xBFx") == "<script></script>\xEF\xBB\xBFx");
}

TEST_CASE("keeps what the tokenizer already deduplicated") {
  CHECK(roundtrip("<p title=a TITLE=b id=c title=d>x</p>") == "<p title=\"a\" id=\"c\">x</p>");
  CHECK(roundtrip("<svg xlink:href=a href=b viewbox=c><a xlink:href=d>x</a></svg>") ==
        "<svg xlink:href=\"a\" href=\"b\" viewBox=\"c\"><a xlink:href=\"d\">x</a></svg>");
}

TEST_CASE("enforces Gumbo's tree depth limit") {
  CHECK_FALSE(parse_error(repeat("<b>", 400)));
  CHECK(parse_error(repeat("<b>", 401)) == ParseError::TreeTooDeep);
  CHECK(parse_error(repeat("<b>", 401) + "x") == ParseError::TreeTooDeep);
  CHECK(parse_error(repeat("<b>", 400) + "<p>") == ParseError::TreeTooDeep);
  CHECK(parse_error(repeat("<b>", 397) + "<table><td>") == ParseError::TreeTooDeep);
}

TEST_CASE("counts open elements as Gumbo does") {
  // A void element never goes on the stack of open elements.
  CHECK_FALSE(parse_error(repeat("<b>", 400) + "<br>"));
  CHECK_FALSE(parse_error(repeat("<b>", 400) + "</p>"));
  // Closing everything again does not undo having been too deep.
  CHECK(parse_error(repeat("<b>", 401) + repeat("</b>", 401)) == ParseError::TreeTooDeep);
  // The adoption agency moves blocks back up, and what counts is how deep they were.
  CHECK_FALSE(parse_error(repeat("<b>" + repeat("<span>", 300) + repeat("<div>", 10) + "</b>", 3)));
  CHECK(parse_error("<b>" + repeat("<span>", 390) + repeat("<div>", 10) + "</b>" +
                    repeat("<div>", 300)) == ParseError::TreeTooDeep);
  // Text pending in a table reopens the <b>s past the limit when the input ends. Gumbo does not
  // check after that.
  std::string bs;
  for (int i = 1; i <= 399; ++i) {
    bs += "<b id=" + std::to_string(i) + ">";
  }
  const std::string reopened = "<p>" + bs + "</p><div><div><table>x";
  CHECK_FALSE(parse_error(reopened));
  CHECK(parse_error(reopened + "<!---->") == ParseError::TreeTooDeep);
}

TEST_CASE("enforces Gumbo's attribute limit") {
  CHECK_FALSE(parse_error("<p " + numbered_attributes(1, 400) + ">x</p>"));
  CHECK(parse_error("<p " + numbered_attributes(1, 401) + ">x</p>") == ParseError::TooManyAttributes);
  std::string same;
  for (int i = 0; i < 1000; ++i) {
    same += " a=1";
  }
  CHECK_FALSE(parse_error("<p" + same + ">x</p>"));
  CHECK_FALSE(parse_error("<textarea><p " + numbered_attributes(1, 401) + ">"));
}

TEST_CASE("counts attributes as Gumbo's tokenizer does") {
  const std::string attributes = numbered_attributes(1, 400);
  // Before dropping a duplicate, on end tags, and on a tag the input ends inside.
  CHECK(parse_error("<p " + attributes + " a1=again>x</p>") == ParseError::TooManyAttributes);
  CHECK(parse_error("<p>x</p " + attributes + " a401>") == ParseError::TooManyAttributes);
  CHECK(parse_error("<p " + attributes + " a401") == ParseError::TooManyAttributes);
}

namespace {

using Clock = std::chrono::steady_clock;

void check_quick(Clock::time_point started, const char* what, std::chrono::milliseconds bound) {
  auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - started);
  CHECK_MESSAGE(elapsed < bound, what, " took ", elapsed.count(), " ms");
}

}  // namespace

// Rust: tests/hardening.rs. The bounds are generous: they catch quadratic time, not noise.
TEST_CASE("deeply nested elements are refused quickly") {
  const auto started = Clock::now();
  CHECK(parse_error(repeat("<div>", 80000)) == ParseError::TreeTooDeep);
  CHECK(parse_error(repeat("<a><b>", 80000)) == ParseError::TreeTooDeep);
  CHECK(parse_error(repeat("<a><div><div>", 30000)) == ParseError::TreeTooDeep);
  check_quick(started, "refusing deep nesting", std::chrono::milliseconds(5000));
}

TEST_CASE("the rest of a body is not read once it is too deep") {
  const std::string too_deep = repeat("<div>", 401);
  const std::string rest(16 * 1024 * 1024, 'x');
  const auto started = Clock::now();
  CHECK(parse_error(too_deep + repeat("<a><b>", rest.size() / 6)) == ParseError::TreeTooDeep);
  CHECK(parse_error(too_deep + "<!--" + rest) == ParseError::TreeTooDeep);
  CHECK(parse_error(too_deep + "<" + rest) == ParseError::TreeTooDeep);
  check_quick(started, "refusing 16 MB bodies", std::chrono::milliseconds(5000));
}

TEST_CASE("a tag with too many attributes is refused quickly") {
  const auto started = Clock::now();
  CHECK(parse_error("<b " + numbered_attributes(1, 64000) + ">x</b>") == ParseError::TooManyAttributes);
  check_quick(started, "refusing 64,000 attributes", std::chrono::milliseconds(5000));
}

TEST_CASE("html tags in the body parse in linear time") {
  std::string body;
  for (int tag = 0; tag < 200; ++tag) {
    body += "<html " + numbered_attributes(tag * 400 + 1, tag * 400 + 400) + ">";
  }
  const auto started = Clock::now();
  CHECK(roundtrip(body).empty());
  check_quick(started, "550 KB of <html> tags", std::chrono::milliseconds(5000));
}

TEST_CASE("elements misplaced in a table parse in linear time") {
  const std::string body = "<table>" + repeat("<br>", 200000);
  const auto started = Clock::now();
  CHECK_FALSE(parse_error(body));
  check_quick(started, "800 KB of <br>s in a table", std::chrono::milliseconds(5000));
}
