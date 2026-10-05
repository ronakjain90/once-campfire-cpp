// Tests of core/html.hpp. The expected text is what Ruby ERB::Util.html_escape returns.
#include "core/html.hpp"

#include <doctest.h>

#include <string>

#include "core/out.hpp"

using campfire::Out;

namespace {
std::string escape(std::string_view text) {
  Out out;
  campfire::html_escape(out, text);
  return out.to_string();
}
}  // namespace

TEST_CASE("html_escape replaces the five characters") {
  CHECK(escape("&") == "&amp;");
  CHECK(escape("<") == "&lt;");
  CHECK(escape(">") == "&gt;");
  CHECK(escape("\"") == "&quot;");
  CHECK(escape("'") == "&#39;");
  CHECK(escape("<script>alert('x') & \"y\"</script>") ==
        "&lt;script&gt;alert(&#39;x&#39;) &amp; &quot;y&quot;&lt;/script&gt;");
}

TEST_CASE("html_escape keeps everything else") {
  CHECK(escape("").empty());
  CHECK(escape("plain text 123") == "plain text 123");
  CHECK(escape("caf\xC3\xA9 \xE2\x9C\x93") == "caf\xC3\xA9 \xE2\x9C\x93");
  CHECK(escape("`=/") == "`=/");  // Ruby does not escape these
  CHECK(escape(std::string("a\0b&", 4)) == std::string("a\0b&amp;", 8));
  CHECK(escape("&amp;") == "&amp;amp;");  // Ruby escapes every &, even in an entity
}

TEST_CASE("needs_html_escape") {
  CHECK(campfire::needs_html_escape("a<b"));
  CHECK_FALSE(campfire::needs_html_escape("ab"));
}

TEST_CASE("SafeHtml is appended with no change") {
  Out out;
  out.append(campfire::SafeHtml::literal("<b>"));
  out.append(campfire::SafeHtml::trusted("<i>"));
  CHECK(out.to_string() == "<b><i>");
}
