// Tests of the search query. Rust: crates/campfire/src/integrations/search.rs (tests), crates/db/src/models/message.rs.
#include <doctest.h>

#include "models/search_query.hpp"

namespace campfire::models::search_query {

TEST_CASE("sanitize keeps only word characters, one space for each other character") {
  CHECK(sanitize("hello, world!") == "hello  world ");
  CHECK(sanitize("café_1 日本") == "café_1 日本");
  CHECK(sanitize("\"quoted\" AND-x") == " quoted  AND x");
  CHECK(sanitize("héllo wörld_1 ２ 日本語 ‿ a-b \xEF\xB8\x8F ❤ é") == "héllo wörld_1 ２ 日本語 ‿ a b \xEF\xB8\x8F   é");
  CHECK(sanitize("\"quoted\" OR NEAR(x*)") == " quoted  OR NEAR x  ");
  CHECK(sanitize("").empty());
  CHECK(sanitize("a\xFF"
                 "b") == "a b");
}

TEST_CASE("is_word classifies as Onigmo does") {
  for (const char32_t c :
       {U'a', U'Z', U'0', U'_', U'é', U'ß', U'日', U'‿', U'́', U'٣', U'ǅ', U'ʰ', U'Ⅻ', U'Ⓐ', U'‍'}) {
    CHECK(is_word(c));
  }
  for (const char32_t c : {U' ', U'-', U'*', U'"', U' ', U'❤', U'€', U'½', U'²'}) CHECK_FALSE(is_word(c));
}

TEST_CASE("is_present is false for white space only") {
  CHECK_FALSE(is_present(""));
  CHECK_FALSE(is_present("  \t\n\xC2\xA0"));
  CHECK(is_present(" a "));
}

TEST_CASE("match_terms quotes each word") {
  CHECK(match_terms("hello  world ") == "\"hello\" \"world\"");
  CHECK(match_terms("   ").empty());
  CHECK(match_terms("a\"b NOT") == "\"a\"\"b\" \"NOT\"");
  CHECK(match_terms(std::string_view("a\0b", 3)) == "\"a\" \"b\"");
}

}  // namespace campfire::models::search_query
